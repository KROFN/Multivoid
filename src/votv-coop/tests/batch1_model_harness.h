// tests/batch1_model_harness.h -- KROFNE FORK (2133), Batch-1 TEST CONTRACT layer 2 + 5.
//
// The MODEL harness: pure fakes around the REAL shipped pure cores. This does NOT test
// individual helpers -- it composes them into a SYSTEM (clients, host, transport, world,
// container, drone) and asserts the INVARIANTS the fork promises, after every event.
//
// REAL production code driving the model (compiled headless):
//   coop::net::send_backlog::BacklogCore          -- the exact retry/outbox core (blocker N)
//   coop::props::extract_pairing::Ledger          -- the exact host pairing ledger (L + M)
//   coop::props::container_extract_wire::Pack/Parse -- the exact blob codec (blocker K)
//   coop::drone_take_sync::{DroneReplayDomain,AdmitRequest,RecordVerdict,EffectiveMirrorHasSack}
//                                                 -- the exact drone admission + mirror gate (D, O)
//   ue_wrap::save_record::SaveRecord + the record codec (linked from save_record_wire.cpp)
//
// FAKE world pieces (the UE parts, modeled at the smallest honest fidelity):
//   FakeTransport  -- one reliable-lane pipe: refuses enqueues (GNS backpressure), duplicates,
//                     reorders, delays, drops queued traffic for a disconnected slot (GNS
//                     closes the connection and its queue dies with it), slot reuse with a
//                     new occupancy generation.
//   FakeDrone      -- the HOST drone's interaction gates (canTakeOff / hasSack) + the counters
//                     the drone invariants need (native dropSack executions, sacks spawned).
//   FakeContainer  -- the host's authoritative container records + the baseHash the CAS judges.
//   FakeWorld      -- the host's authoritative prop registry: SpawnProp returns an actor whose
//                     OBSERVED key may differ from the requested key (the modeled setKey
//                     defect); the BIRTH POLICY (below) decides what that proves.
//   FakeClient     -- one peer: mirror gates, the pending take, the local phantom, extraction
//                     ghosts, its own outbox (BacklogCore).
//   FakeHost       -- replay domains + generation table, the drone validator, the container
//                     CAS, the REAL Ledger, its own outbox, the authoritative births.
//
// THE BIRTH POLICY under test (review finding T):
//   PointerImpliesOk -- the CURRENT-HEAD contract: an actor pointer from the spawn call is
//                      reported as an established birth, so kOk goes out even when the live
//                      actor's observed key is NOT the requested key. The T invariant
//                      ("kOk only over a PROVEN key") FAILS under this policy by design --
//                      that is the RED demonstration; the fix hardens the production author.
//   KeyProofRequired -- the hardened contract: the live actor's observed key must equal the
//                      requested key; a mis-keyed pointer is reaped and retried (transient).
//
// THE EXTRACTION INVARIANT (the fork's promise, asserted after EVERY event):
//   for one logical item identity,
//     authoritativeCopies = (host container holds it ? 1 : 0) + (host world has the keyed prop ? 1 : 0)
//   is 1 at every TERMINAL state. Interim 0-copies windows exist by design ONLY while a
//   bounded recovery is armed (the birth retry; the pairing window). The ONE policy loss the
//   design accepts: the bounded birth-retry window lapses -> kBirthFailed -> the item is in
//   NEITHER place, loudly. Those terminals are surfaced EXPECTED-FAIL, never called lossless.
//
// DRONE INVARIANTS D1-D6 asserted after every event (see batch1_model_selftest.cpp).

#pragma once

#include "coop/net/protocol.h"
#include "coop/net/send_backlog.h"
#include "coop/props/container_extract_wire.h"
#include "coop/props/extract_pairing.h"
#include "coop/interactables/drone_take_sync.h"
#include "coop/interactables/drone_replay_domain.h"
#include "coop/player/sleep_dilation_ownership.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Link stub: save_record_wire/signal_wire reference the project logger; headless tests route
// it nowhere (the tested logic never logs).
namespace ue_wrap::log {
enum class Level { Info, Warn, Error };
void Write(Level, const char*, ...) {}
}

namespace b1model {

inline bool g_ingestTrace = false;   // B1_TRACE_INGEST=1: one line per contents ingest

using coop::net::ReliableKind;
using coop::props::container_extract_wire::ParsedBlob;
using coop::props::container_extract_wire::PackContents;
using coop::props::container_extract_wire::PackContentsExtract;
using coop::props::container_extract_wire::ParseContentsBlob;
using coop::props::extract_pairing::BirthVerdict;
using coop::props::extract_pairing::ExtractKey;
using coop::props::extract_pairing::Ledger;
using ue_wrap::save_record::SaveRecord;

constexpr int kModelSlots = 8;   // model peer slots 1..7 (0 = host)
constexpr uint8_t kKindContents = 118;   // the STOCK kind the op=1 blob rides (never a backlog lane)

// ---- deterministic PRNG (splitmix64; the fuzz layer seeds it, scenarios use a fixed one) ----
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0u; }
    bool Chance(uint32_t percent) { return (Next() % 100u) < percent; }
};

// ---- the wire --------------------------------------------------------------------------------
struct WireMsg {
    int  fromSlot = -1;                 // -1 = host-originated
    int  toSlot   = -1;                 // -1 = host-directed
    uint8_t kind = 0;
    std::vector<uint8_t> bytes;
    int  holdPumps = 0;                 // delay transform: pumps left before delivery
};

class FakeTransport {
public:
    // scripted fault injection (the fuzz layer randomizes all of these)
    int  refuseNextEnqueues = 0;        // enqueue returns false N times (GNS refused to queue)
    int  dupNextEnqueues = 0;           // the next N enqueued messages are queued TWICE
    int  delayNextEnqueues = 0;         // the next N messages stall their lane for 2 pumps
                                        // (per-sender FIFO stall -- later same-sender traffic is
                                        // delayed BEHIND them; later OTHER-sender traffic is not)
    // NOTE on "reordering": GNS guarantees per-connection ORDER of reliable messages, so two
    // messages of ONE lane can never swap. Real reordering comes from (a) DIFFERENT senders'
    // lanes racing at the receiver and (b) critical sends retried by the BACKLOG landing after
    // newer direct traffic -- both are produced by this model (multi-peer scenarios + backlog
    // retries) without fabricating an impossible same-lane swap.

    std::vector<WireMsg> wire;
    size_t accepted = 0, refused = 0;
    std::function<void(const WireMsg&)> onDeliver;

    bool Enqueue(int fromSlot, int toSlot, uint8_t kind, const void* data, size_t len) {
        if (refuseNextEnqueues > 0) { --refuseNextEnqueues; ++refused; return false; }
        ++accepted;
        WireMsg m;
        m.fromSlot = fromSlot; m.toSlot = toSlot; m.kind = kind;
        m.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
        if (delayNextEnqueues > 0) { --delayNextEnqueues; m.holdPumps = 2; }
        const bool dup = dupNextEnqueues > 0;
        if (dup) --dupNextEnqueues;
        wire.push_back(m);
        if (dup) wire.push_back(m);                  // the duplicate (second copy, same bytes)
        return true;
    }

    // One delivery pass. Held messages count down; everything due is delivered FIFO.
    // Deliveries may ENQUEUE new messages (a request handler sends a verdict) -- iterate a
    // snapshot so the new sends land in the wire for the NEXT pass, never inside this one.
    // FIFO STALL: a held message stalls EVERY LATER message of the SAME sender behind it
    // (GNS delivers a connection's reliable traffic in order -- one buffered message delays
    // the whole lane behind it; it can never be overtaken by the same sender's later send).
    void Pump() {
        std::vector<WireMsg> current;
        current.swap(wire);
        std::vector<WireMsg> held;
        std::vector<int> stalled;   // senders with a message held during THIS pass
        auto isStalled = [&](int from) {
            for (int s : stalled) if (s == from) return true;
            return false;
        };
        for (WireMsg& m : current) {
            if (isStalled(m.fromSlot)) { held.push_back(m); continue; }   // stuck behind the head
            if (m.holdPumps > 0) {
                --m.holdPumps;                   // decrement BEFORE the copy (the stored count must tick)
                held.push_back(m);               // held this pass (delivered when it enters at 0)
                stalled.push_back(m.fromSlot);   // the lane stalls behind it for THIS pass
                continue;
            }
            if (onDeliver) onDeliver(m);
        }
        wire.insert(wire.begin(), held.begin(), held.end());   // held keep their earlier position
    }

    // GNS semantics: closing a connection drops ITS queued reliable traffic -- both directions.
    // This is why in-flight traffic can never reach a slot's SUCCESSOR; the one queue that
    // DOES survive a disconnect is OUR OWN send_backlog (the S review finding).
    void DisconnectSlot(int slot) {
        std::vector<WireMsg> keep;
        for (WireMsg& m : wire)
            if (m.toSlot != slot && m.fromSlot != slot) keep.push_back(m);
        wire.swap(keep);
    }
    size_t InFlight() const { return wire.size(); }
};

// The SendCritical SHAPE (the production wrapper's contract, modeled): try the transport
// ONCE; a refusal parks the EXACT bytes in the caller's outbox; nothing is "shipped" unless
// the transport accepted.
template <typename CoreT>
bool CriticalSend(CoreT& core, FakeTransport& t, uint64_t nowMs, ReliableKind kind, int fromSlot,
                  int toSlot, const void* data, size_t len) {
    if (t.Enqueue(fromSlot, toSlot, static_cast<uint8_t>(kind), data, len)) return true;
    core.SetTime(nowMs);
    core.Park(kind, toSlot, data, len);
    return false;
}

// ---- the authoritative world ------------------------------------------------------------------

struct Actor {
    uint64_t    id = 0;
    std::wstring observedKey;    // the key the actor ACTUALLY carries (may differ from the request)
};

class FakeWorld {
public:
    // THE BIRTH POLICY UNDER TEST (review finding T). Default = the SHIPPED (hardened)
    // contract: KeyProofRequired. PointerImpliesOk is kept executable so the model suite can
    // DEMONSTRATE, by execution, that the pre-fix contract violated the T invariant (the RED
    // run of the test contract pass, captured at HEAD 49db879a).
    enum class BirthPolicy { PointerImpliesOk, KeyProofRequired };
    BirthPolicy birthPolicy = BirthPolicy::KeyProofRequired;

    int      keyStickFailures = 0;    // the next N spawns carry NO key (the modeled setKey defect)
    int      spawnFails = 0;          // the next N spawn attempts fail outright (no actor at all)
    uint64_t nextActorId = 1;
    std::vector<Actor> actors;        // live actors
    int      reapedMisKeyed = 0;      // KeyProofRequired: mis-keyed zombies destroyed
    int      birthsByKey = 0;         // births that landed with the EXACT requested key

    Actor* SpawnProp(const std::wstring& requestedKey) {
        if (spawnFails > 0) { --spawnFails; return nullptr; }   // the birth could not be established
        Actor a;
        a.id = nextActorId++;
        if (keyStickFailures > 0) { --keyStickFailures; a.observedKey.clear(); }
        else                      { a.observedKey = requestedKey; }
        actors.push_back(a);
        return &actors.back();
    }
    void Destroy(uint64_t id) {
        for (size_t i = 0; i < actors.size(); ++i)
            if (actors[i].id == id) { actors.erase(actors.begin() + static_cast<long>(i)); return; }
    }
    bool KeyProven(const std::wstring& key) const {
        for (const Actor& a : actors)
            if (a.observedKey == key) return true;
        return false;
    }
    int  KeyProvenCount(const std::wstring& key) const {
        int n = 0;
        for (const Actor& a : actors)
            if (a.observedKey == key) ++n;
        return n;
    }
};

// ---- the host's drone (the interaction gates the native verb would read) ----------------------
struct FakeDrone {
    bool canTakeOff = true;
    bool hasSack    = true;
};

// ---- helpers ----------------------------------------------------------------------------------
inline std::wstring WireKeyToW(const coop::net::WireKey& k) {
    return std::wstring(k.data, k.data + k.len);
}
inline void FillWireKey(coop::net::WireKey& k, const std::wstring& s) {
    k.len = static_cast<uint8_t>(s.size());
    for (size_t i = 0; i < s.size() && i < sizeof(k.data); ++i) k.data[i] = static_cast<char>(s[i]);
}
inline void FillWireClassName(coop::net::WireClassName& k, const std::wstring& s) {
    k.len = static_cast<uint8_t>(s.size());
    for (size_t i = 0; i < s.size() && i < sizeof(k.data); ++i) k.data[i] = static_cast<char>(s[i]);
}
inline uint64_t Fnv(const std::vector<uint8_t>& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}
inline SaveRecord MakeRecord(const wchar_t* cls, const wchar_t* key) {
    SaveRecord r;
    r.className = cls;
    r.key = key;
    r.xform = {1.f, 0.f, 0.f, 0.f, 1.f, 2.f, 3.f, 1.f, 1.f, 1.f};
    return r;
}

// ---- the host ----------------------------------------------------------------------------------

class FakeHost : public coop::props::extract_pairing::ResultSink,
                 public coop::props::extract_pairing::BirthSink {
public:
    FakeWorld*  world  = nullptr;
    FakeTransport* transport = nullptr;
    FakeDrone drone;

    // container (authoritative)
    uint32_t containerEid = 900;
    std::vector<SaveRecord> containerItems;

    // occupancy + replay
    uint32_t gen[kModelSlots] = {};   // 0 = empty; tests mint (non-zero)
    coop::drone_take_sync::DroneReplayDomain domain[kModelSlots];

    // counters the invariants read
    int dropSackExecutions = 0;       // native verb bodies run on the authority
    int sacksSpawned = 0;             // authoritative sacks created (one per native body)
    int acceptedRequests = 0;         // distinct accepted logical requests
    int deniedRequests = 0;
    struct AcceptedLogical { int slot; uint32_t gen; uint32_t nonce; };
    std::vector<AcceptedLogical> acceptedLogical;

    // extraction (the REAL ledger) + the proof instrumentation
    Ledger ledger;
    // PROOF-MAP KEYING (fuzz finding, seed 0 / scenario 4429 of the test-contract pass):
    // extraction tokens are PER-CLIENT counters -- two live peers can both mint token 1 -- so
    // this instrumentation map MUST be keyed by {senderSlot, token}, exactly like the REAL
    // ledger's ExtractKey maps (the production ledger is fully {slot, generation, token}-keyed;
    // keying the PROOF CHECK by raw token alone validated a slot's kOk against ANOTHER slot's
    // requested key and raised a false T violation). Slot+token is sufficient here: a token
    // value can only be reused within a slot after a reconnect, and DisconnectSlot purges the
    // predecessor's ledger state before a successor can mint.
    std::map<std::pair<uint8_t, uint64_t>, std::wstring> birthKeyByToken;  // {slot, token} -> key
    std::vector<std::pair<uint64_t, bool>> kOkProof;          // (token, key proven at kOk time)
    std::vector<std::pair<uint64_t, uint8_t>> rejectLog;      // (token, reason)

    // the outbox (the REAL backlog core; entries park on transport refusal)
    coop::net::send_backlog::BacklogCore outbox{0};
    uint64_t nowMs = 0;

    // The host's container-truth broadcast (production: BroadcastContainer). Fired on every
    // refused client write and on every applied client write -- the peers' believed base hash
    // and local view converge to the host truth (this is what makes a refused author re-derive
    // from a FRESH base instead of hammering a stale one).
    std::function<void()> onBroadcast;

    // ---- container CAS -------------------------------------------------------------------------
    uint64_t CurrentHash() const {
        return Fnv(PackContents(containerEid, 0, containerItems));
    }
    bool ContainerHolds(const std::wstring& key) const {
        for (const SaveRecord& r : containerItems)
            if (r.key == key) return true;
        return false;
    }

    // ---- sends ---------------------------------------------------------------------------------
    void SendResult(int slot, uint32_t nonce, uint8_t accepted, uint8_t reason) {
        coop::net::DroneActionResultPayload r{};
        r.nonce = nonce; r.accepted = accepted; r.reason = reason;
        CriticalSend(outbox, *transport, nowMs, ReliableKind::DroneActionResult, -1, slot, &r, sizeof(r));
    }
    void SendExtractResult(uint8_t toSlot,
                           const coop::net::ContainerExtractResultPayload& p) override {
        const auto it = birthKeyByToken.find({toSlot, p.extractToken});
        const bool proven = it != birthKeyByToken.end() && world->KeyProven(it->second);
        if (p.accepted) kOkProof.push_back({p.extractToken, proven});
        else            rejectLog.push_back({p.extractToken, p.reason});
        CriticalSend(outbox, *transport, nowMs, ReliableKind::ContainerExtractResult, -1, toSlot, &p,
                     sizeof(p));
    }

    // ---- the birth author (THE POLICY UNDER TEST) ----------------------------------------------
    BirthVerdict AuthorBirth(const coop::net::ContainerExtractIntentPayload& p,
                             uint8_t /*senderSlot*/) override {
        const std::wstring key = WireKeyToW(p.birth.key);
        // the dup-guard probe first (production SpawnExtractionBirth shape): the exact key
        // already live IS the birth -> AlreadyExists.
        if (!key.empty() && world->KeyProven(key)) return BirthVerdict::AlreadyExists;
        Actor* a = world->SpawnProp(key);
        if (world->birthPolicy == FakeWorld::BirthPolicy::PointerImpliesOk) {
            // CURRENT-HEAD contract: a pointer alone is reported as the birth (the T bug).
            if (a) { ++world->birthsByKey; return BirthVerdict::Spawned; }
            return BirthVerdict::TransientFailure;
        }
        // KeyProofRequired: the live actor's OWN key must equal the requested key.
        if (a && a->observedKey == key) { ++world->birthsByKey; return BirthVerdict::Spawned; }
        if (a) { world->Destroy(a->id); ++world->reapedMisKeyed; }   // reap the mis-keyed zombie
        return BirthVerdict::TransientFailure;
    }

    // ---- ingests --------------------------------------------------------------------------------
    void OnDroneRequest(int slot, const coop::net::DroneActionRequestPayload& p) {
        // generation-bound replay domain (production OnRequest shape)
        auto& d = domain[slot];
        if (d.generation != gen[slot]) {
            d = coop::drone_take_sync::DroneReplayDomain{};
            d.generation = gen[slot];
        }
        using namespace coop::drone_take_sync;
        const Admission adm = AdmitRequest(d, p.op, p.nonce);
        if (adm.action != Admit::Execute) {
            if (adm.action == Admit::ResendVerdict)
                SendResult(slot, p.nonce, adm.verdict.accepted, adm.verdict.reason);
            else if (p.op != coop::net::drone_action_op::kDropSack || p.nonce == 0)
                SendResult(slot, p.nonce, adm.verdict.accepted, adm.verdict.reason);
            return;
        }
        uint8_t reason = coop::net::drone_action_result::kOk;
        if (!drone.canTakeOff) reason = coop::net::drone_action_result::kNotArrived;
        else if (!drone.hasSack) reason = coop::net::drone_action_result::kNoCargo;
        if (reason != coop::net::drone_action_result::kOk) {
            RecordVerdict(d, p.nonce, 0, reason);
            ++deniedRequests;
            SendResult(slot, p.nonce, 0, reason);
            return;
        }
        // ACCEPT: exactly ONE native body = exactly ONE authoritative sack
        RecordVerdict(d, p.nonce, 1, coop::net::drone_action_result::kOk);
        ++dropSackExecutions;
        drone.hasSack = false;
        ++sacksSpawned;
        ++acceptedRequests;
        acceptedLogical.push_back({slot, gen[slot], p.nonce});
        SendResult(slot, p.nonce, 1, coop::net::drone_action_result::kOk);
    }

    // the stock kind-118 ingest (ParseAndApply shape: structural parse -> stale-generation
    // guard -> CAS -> ledger)
    void OnContentsBlob(int slot, const std::vector<uint8_t>& blob) {
        ParsedBlob pb;
        if (!ParseContentsBlob(blob, 64, pb)) return;             // wire garbage: rejected wholesale
        if (g_ingestTrace)
            std::printf("    [ingest] slot=%d op=%d tokens=%zu base=%llu cur=%llu\n", slot,
                        (int)pb.hasTail, pb.tokens.size(),
                        (unsigned long long)pb.baseHash, (unsigned long long)CurrentHash());
        // FUZZ FINDING (stale generation): a fork write whose sender's occupancy ended before
        // processing is refused WHOLE (no CAS, no pairing side effects) -- the item stays.
        if (!pb.tokens.empty() && gen[slot] == 0) { if (g_ingestTrace) std::printf("    [ingest] -> stale-gen refuse\n"); return; }
        std::vector<ExtractKey> keys;
        for (uint64_t t : pb.tokens) keys.push_back(ExtractKey{static_cast<uint8_t>(slot), gen[slot], t});
        if (pb.baseHash != CurrentHash()) {                        // the host CAS
            ledger.OnWriteRefused(keys, *this);
            if (onBroadcast) onBroadcast();                        // the refused author re-derives fresh
            return;
        }
        containerItems = pb.recs;                                  // Applied
        (void)ledger.OnWriteApplied(keys, pb.eid, nowMs, *this, *this);
        if (onBroadcast) onBroadcast();
    }

    void OnExtractIntent(int slot, const coop::net::ContainerExtractIntentPayload& p) {
        // Stale-occupancy guard (mirrors the write guard): an intent from a slot whose
        // occupancy ended can never pair (its write will be refused too) -- refuse here.
        if (gen[slot] == 0) return;
        birthKeyByToken[{static_cast<uint8_t>(slot), p.extractToken}] = WireKeyToW(p.birth.key);
        ExtractKey key{static_cast<uint8_t>(slot), gen[slot], p.extractToken};
        (void)ledger.ParkIntent(key, p.containerEid, p, nowMs, *this, *this);
    }

    // ---- per-slot teardown ----------------------------------------------------------------------
    void DisconnectSlot(int slot) {
        transport->DisconnectSlot(slot);                 // GNS: the connection's queue dies
        domain[slot] = coop::drone_take_sync::DroneReplayDomain{};
        ledger.OnDisconnectForSlot(static_cast<uint8_t>(slot));
        outbox.PurgeSlot(slot);                          // S: the leaver's parked sends die with their occupancy
        gen[slot] = 0;                                   // the slot is empty until a successor mints
    }
};

// ---- the client ---------------------------------------------------------------------------------

class FakeClient {
public:
    int slot = 0;
    FakeWorld* world = nullptr;
    FakeTransport* transport = nullptr;

    // mirror (the streamed gates) + pending request (the O gate)
    bool     streamedHasSack = false;
    bool     pendingTake = false;
    uint32_t pendingNonce = 0;
    uint32_t nextNonce = 0;
    int      localPhantoms = 0;             // the local ghost sacks the native body spawns
    int      resultsReceived = 0;           // EVERY drone verdict delivered (stale-traffic probe)
    int      extractResultsReceived = 0;    // EVERY extract verdict delivered (stale-traffic probe)
    bool     MirrorHasSack() const {
        return coop::drone_take_sync::EffectiveMirrorHasSack(streamedHasSack, pendingTake);
    }

    // extraction
    uint64_t nextToken = 0;
    std::map<uint64_t, uint64_t> ghosts;    // token -> local ghost actor id
    uint32_t containerEid = 900;
    uint64_t viewHash = 0;                  // the client's believed host base (the last host truth)
    std::vector<SaveRecord> viewItems;      // the client's local view of the contents
    // PRODUCTION FIDELITY (container_contents_sync g_queuedExtractTokens): extraction tokens
    // queue until a write CARRIES them; they pop ONLY when the transport ACCEPTS the write, so
    // a refused/lost write retries WITH the same tokens on the next publish (the re-derive).
    std::deque<uint64_t> pendingTokens;

    bool ContainerHolds(const wchar_t* key) const {
        for (const SaveRecord& r : viewItems)
            if (r.key == key) return true;
        return false;
    }

    coop::net::send_backlog::BacklogCore outbox{0};   // client -> host critical sends
    uint64_t* clockMs = nullptr;            // the model clock (owned by the Model)

    // ---- actions -------------------------------------------------------------------------------
    bool PressTake() {
        if (!MirrorHasSack()) return false;   // the Take option is NOT presented (the O gate)
        ++localPhantoms;                      // the native body ran client-locally
        ++nextNonce;
        coop::net::DroneActionRequestPayload p{};
        p.nonce = nextNonce; p.op = coop::net::drone_action_op::kDropSack;
        CriticalSend(outbox, *transport, *clockMs, ReliableKind::DroneActionRequest, slot, -1, &p, sizeof(p));
        pendingTake = true; pendingNonce = nextNonce;
        return true;
    }

    // The extraction edge: the native takeObj ran client-locally (item left the local view);
    // the token queues and rides the next publish, together with the birth intent.
    void DoExtract(const wchar_t* itemKey) {
        ++nextToken;
        for (size_t i = 0; i < viewItems.size(); ++i)
            if (viewItems[i].key == itemKey) { viewItems.erase(viewItems.begin() + static_cast<long>(i)); break; }
        const uint64_t localActor = world->nextActorId++;   // the LOCAL ghost (client-side prop)
        ghosts[nextToken] = localActor;
        pendingTokens.push_back(nextToken);

        coop::net::ContainerExtractIntentPayload ip{};
        FillWireKey(ip.birth.key, itemKey);
        FillWireClassName(ip.birth.className, L"Aprop_rock_C");
        ip.extractToken = nextToken;
        ip.containerEid = containerEid;
        CriticalSend(outbox, *transport, *clockMs, ReliableKind::ContainerExtractIntent, slot, -1, &ip, sizeof(ip));

        Publish();
    }

    // The re-derive / publish: compose the op=0/op=1 write from the CURRENT local view with the
    // believed base hash and up to 4 queued tokens; pop the tokens ONLY on transport acceptance
    // (production: PublishContainer + the g_queuedExtractTokens pop-on-success rule).
    void Publish() {
        std::vector<uint64_t> tokens;
        for (size_t i = 0; i < pendingTokens.size() && i < 4; ++i) tokens.push_back(pendingTokens[i]);
        const auto blob = tokens.empty()
            ? PackContents(containerEid, viewHash, viewItems)
            : PackContentsExtract(containerEid, viewHash, viewItems, tokens);
        if (transport->Enqueue(slot, -1, kKindContents, blob.data(), blob.size())) {
            pendingTokens.erase(pendingTokens.begin(),
                                pendingTokens.begin() + static_cast<std::ptrdiff_t>(tokens.size()));
        }
        // a refused enqueue keeps the tokens: the next publish retries with the same tokens
    }

    // ---- ingests -------------------------------------------------------------------------------
    void OnDroneState(bool hostHasSack) { streamedHasSack = hostHasSack; }

    void OnDroneResult(const coop::net::DroneActionResultPayload& r) {
        ++resultsReceived;                                     // every delivery counts (stale probe)
        if (!pendingTake || r.nonce != pendingNonce) return;   // late/duplicate/foreign verdict
        if (localPhantoms > 0) --localPhantoms;                // the exact phantom retires either way
        pendingTake = false; pendingNonce = 0;
    }

    void OnExtractResult(const coop::net::ContainerExtractResultPayload& r) {
        ++extractResultsReceived;                              // every delivery counts (stale probe)
        const auto it = ghosts.find(r.extractToken);
        if (it == ghosts.end()) return;                        // unknown/expired token
        ghosts.erase(it);                                      // terminal EITHER way (v3 blocker M)
        if (!r.accepted) world->Destroy(it->second);           // reject: the local ghost retires
    }
};

// ---- the assembled system ------------------------------------------------------------------------

struct ModelStats {
    int droneInvariantViolations = 0;
    int extractionInvariantViolations = 0;
    int staleDeliveryViolations = 0;
    int keyProofViolations = 0;         // kOk emitted over an unproven key (T)
    int expectedLossTerminals = 0;      // kBirthFailed terminals (the surfaced policy loss)
};

struct Model {
    FakeTransport transport;
    FakeWorld     world;
    FakeHost      host;
    FakeClient    clients[kModelSlots];   // indexed by slot; [0] unused
    uint64_t      nowMs = 1000;
    ModelStats    stats;

    // The host outbox's generation authority (the S binding): the model's gen table IS the
    // model of Session::peerGenerationForSlot.
    struct HostGenResolver : coop::net::send_backlog::BacklogCore::IGenerationResolver {
        FakeHost* h = nullptr;
        uint32_t GenerationForSlot(int slot) override {
            if (!h || slot < 0 || slot >= kModelSlots) return 0;
            return h->gen[slot];
        }
    } genResolver;

    // per-item bookkeeping for the extraction invariant: every ExtractKey that claimed the item
    // (two peers may extract the same item concurrently -- the CAS arbitrates; the invariant
    // check must watch ALL claimants, not just the latest).
    std::map<std::wstring, std::vector<ExtractKey>> itemKey;

    Model() {
        host.transport = &transport;
        host.world = &world;
        host.nowMs = nowMs;
        genResolver.h = &host;
        host.outbox.SetGenerationResolver(&genResolver);
        for (int s = 1; s < kModelSlots; ++s) {
            clients[s].slot = s;
            clients[s].world = &world;
            clients[s].transport = &transport;
            clients[s].containerEid = host.containerEid;
            clients[s].clockMs = &nowMs;
        }
        transport.onDeliver = [this](const WireMsg& m) { Deliver(m); };
        // the container-truth broadcast: every peer's believed base + local view converge.
        // PRODUCTION FIDELITY (test-contract fuzz finding): host truth overwrites the local
        // container -- any token still QUEUED (unsent) is CANCELLED by that truth (its item is
        // back / state superseded); its parked intent expires honestly via the pairing TTL.
        host.onBroadcast = [this]() {
            for (int s = 1; s < kModelSlots; ++s) {
                clients[s].viewItems = host.containerItems;
                clients[s].viewHash = host.CurrentHash();
                clients[s].pendingTokens.clear();
            }
        };
    }

    // Scenario setup: connect a client at `slot` with occupancy generation `generation`.
    // A generation change REQUIRES the previous occupant to have left (in production the slot's
    // connection CLOSED -- its in-flight queue died with it). Connecting over an occupied slot
    // therefore disconnects it first; a generation can never change under a live occupant.
    void Connect(int slot, uint32_t generation, bool droneHasSack = true) {
        if (host.gen[slot] != 0) host.DisconnectSlot(slot);
        FakeClient& c = clients[slot];
        c.streamedHasSack = false;
        c.pendingTake = false;
        c.pendingNonce = 0;
        c.nextNonce = 0;
        c.nextToken = 0;
        c.localPhantoms = 0;
        c.ghosts.clear();
        c.resultsReceived = 0;
        c.extractResultsReceived = 0;
        c.outbox.Clear();
        host.gen[slot] = generation;
        host.drone.hasSack = droneHasSack;
        c.viewItems = host.containerItems;
        c.viewHash = host.CurrentHash();
        StreamDroneState(slot);
    }

    // clock
    void Step(uint64_t dt) {
        nowMs += dt;
        host.nowMs = nowMs;
        for (int s = 1; s < kModelSlots; ++s)
            if (host.gen[s] != 0 && !clients[s].pendingTokens.empty())
                clients[s].Publish();   // the re-derive drain: pending tokens ride the next publish
        PumpOutboxes();
        transport.Pump();
        host.ledger.Sweep(nowMs, host, host);   // production sweeps ~4x/second; every step here is fine
    }
    void PumpOutboxes() {
        struct HostSink : coop::net::send_backlog::BacklogCore::ISendSink {
            FakeTransport* t = nullptr;
            bool Send(ReliableKind kind, int toSlot, const uint8_t* d, size_t n) override {
                return t->Enqueue(-1, toSlot, static_cast<uint8_t>(kind), d, n);
            }
        } hs;
        hs.t = &transport;
        host.outbox.SetTime(nowMs);
        host.outbox.Drain(hs);
        struct ClientSink : coop::net::send_backlog::BacklogCore::ISendSink {
            FakeTransport* t = nullptr;
            int from = 0;                                    // the draining client's OWN slot
            bool Send(ReliableKind kind, int toSlot, const uint8_t* d, size_t n) override {
                return t->Enqueue(from, toSlot, static_cast<uint8_t>(kind), d, n);
            }
        } cs;
        cs.t = &transport;
        for (int s = 1; s < kModelSlots; ++s) {
            cs.from = s;
            clients[s].outbox.SetTime(nowMs);
            clients[s].outbox.Drain(cs);
        }
    }
    // quiesce: pump until no in-flight traffic and both outboxes are dry (bounded)
    void DrainWire() {
        for (int i = 0; i < 64 && (transport.InFlight() || host.outbox.Size() || AnyClientOutbox()); ++i)
            Step(10);
        Step(10);
    }
    // advance the clock in 50 ms pumps until `done` (bounded by maxMs); retry cadences
    // (250 ms backlog / 500 ms birth retry) land naturally on these pumps.
    bool AdvanceUntil(const std::function<bool()>& done, uint64_t maxMs) {
        while (!done() && maxMs > 0) {
            Step(50);
            maxMs = maxMs > 50 ? maxMs - 50 : 0;
        }
        return done();
    }
    bool AnyClientOutbox() const {
        for (int s = 1; s < kModelSlots; ++s)
            if (clients[s].outbox.Size()) return true;
        return false;
    }

    void Deliver(const WireMsg& m) {
        if (m.toSlot == -1) {                      // client -> host
            if (m.kind == static_cast<uint8_t>(ReliableKind::DroneActionRequest)) {
                coop::net::DroneActionRequestPayload p{};
                std::memcpy(&p, m.bytes.data(), m.bytes.size());
                host.OnDroneRequest(m.fromSlot < 0 ? 0 : m.fromSlot, p);
            } else if (m.kind == static_cast<uint8_t>(ReliableKind::ContainerExtractIntent)) {
                coop::net::ContainerExtractIntentPayload p{};
                std::memcpy(&p, m.bytes.data(), m.bytes.size());
                host.OnExtractIntent(m.fromSlot < 0 ? 0 : m.fromSlot, p);
            } else if (m.kind == kKindContents) {
                host.OnContentsBlob(m.fromSlot < 0 ? 0 : m.fromSlot, m.bytes);
            }
            return;
        }
        if (m.fromSlot == -1) {                    // host -> client
            if (m.toSlot >= kModelSlots) return;
            FakeClient& c = clients[m.toSlot];
            if (m.kind == static_cast<uint8_t>(ReliableKind::DroneActionResult)) {
                coop::net::DroneActionResultPayload p{};
                std::memcpy(&p, m.bytes.data(), m.bytes.size());
                c.OnDroneResult(p);
            } else if (m.kind == static_cast<uint8_t>(ReliableKind::ContainerExtractResult)) {
                coop::net::ContainerExtractResultPayload p{};
                std::memcpy(&p, m.bytes.data(), m.bytes.size());
                c.OnExtractResult(p);
            }
        }
    }

    // the streamed DroneState (host truth -> the client's mirror; <=50ms in production, immediate here)
    void StreamDroneState(int slot) {
        if (slot >= 1 && slot < kModelSlots) clients[slot].OnDroneState(host.drone.hasSack);
    }
};

// ---- the invariants ------------------------------------------------------------------------------

struct InvariantReport {
    bool ok = true;
    const char* code = "";
    std::string what;
};

inline int AuthoritativeCopies(const FakeHost& host, const std::wstring& key) {
    return (host.ContainerHolds(key) ? 1 : 0) + (host.world->KeyProvenCount(key) > 0 ? 1 : 0);
}

// ALWAYS-ON checks (run after EVERY event by the scenarios and the fuzz runner):
//   D1/D5  native dropSack executions == distinct accepted logical requests
//   D4     authoritative sacks spawned == accepted requests (no more, no fewer)
//   D2     a client with a pending take can never hold a second phantom
//   T      every kOk verdict went out over a PROVEN key (world holds the requested key)
//   E-inv  no item's terminal copies may exceed 1 at ANY time (2 copies is the duplication
//          catastrophe; 0 copies is only tolerated inside a bounded recovery window or as
//          the surfaced kBirthFailed terminal -- checked at terminal points, see below)
inline InvariantReport CheckSystemInvariants(const Model& m) {
    InvariantReport rep;
    rep.ok = false;
    if (m.host.dropSackExecutions != static_cast<int>(m.host.acceptedLogical.size())) {
        rep.code = "D1/D5"; rep.what = "native dropSack executions != distinct accepted logical requests";
        return rep;
    }
    if (m.host.sacksSpawned != m.host.acceptedRequests) {
        rep.code = "D4"; rep.what = "authoritative sacks spawned != accepted logical requests";
        return rep;
    }
    for (int s = 1; s < kModelSlots; ++s) {
        if (m.clients[s].pendingTake && m.clients[s].localPhantoms > 1) {
            rep.code = "D2"; rep.what = "second local phantom while a take is pending";
            return rep;
        }
    }
    for (const auto& kp : m.host.kOkProof) {
        if (!kp.second) {
            rep.code = "T"; rep.what = "kOk emitted while the requested key was NOT proven on the live birth";
            return rep;
        }
    }
    for (const auto& item : m.itemKey) {
        const int copies = AuthoritativeCopies(m.host, item.first);
        if (copies > 1) {
            rep.code = "E-inv"; rep.what = "authoritativeCopies == 2 for '" + std::string(item.first.begin(), item.first.end()) + "'";
            return rep;
        }
    }
    rep.ok = true;
    return rep;
}

// A fresh occupant of a reused slot must start pristine (D6): no pending take, no phantom,
// no ghosts inherited from the PREVIOUS occupant's traffic.
inline InvariantReport CheckFreshOccupant(const Model& m, int slot) {
    InvariantReport rep;
    rep.ok = false;
    const FakeClient& c = m.clients[slot];
    if (c.pendingTake) { rep.code = "D6"; rep.what = "new occupant inherited a pending take"; return rep; }
    if (c.localPhantoms != 0) { rep.code = "D6"; rep.what = "new occupant inherited a phantom"; return rep; }
    if (!c.ghosts.empty()) { rep.code = "D6"; rep.what = "new occupant inherited extraction ghosts"; return rep; }
    rep.ok = true;
    return rep;
}

// TERMINAL copies check for one item (call after DrainWire() at a declared terminal point):
//   normally authoritativeCopies == 1;
//   documented policy losses (surfaced EXPECTED-FAIL, never called lossless):
//     loss #1  the bounded birth-retry window lapsed -> kBirthFailed (the client is TOLD);
//     loss #2  the write APPLIED but the intent never paired (marker expired, or the extractor
//              disconnected) -> the item is unbacked, nobody can even be told;
//   ANY other 0/2-copies terminal is an INVARIANT VIOLATION.
inline InvariantReport CheckCopiesTerminal(const Model& m, const std::wstring& key,
                                           int* surfacedLossOut = nullptr) {   // 0=none 1=verdict 2=unbacked
    InvariantReport rep;
    rep.ok = false;
    const auto it = m.itemKey.find(key);
    if (it == m.itemKey.end() || it->second.empty()) {
        rep.code = "E-inv"; rep.what = "item not registered"; return rep;
    }
    // terminal when NO claimant's pairing is still live
    for (const ExtractKey& k : it->second)
        if (m.host.ledger.IsPending(k)) {
            rep.code = "E-inv"; rep.what = "pairing state still pending (not terminal)";
            return rep;
        }
    const int copies = AuthoritativeCopies(m.host, key);
    if (copies == 1) { rep.ok = true; return rep; }
    if (copies == 0) {
        // loss #1: an honest kBirthFailed terminal verdict for any claimant
        for (const auto& rj : m.host.rejectLog)
            if (rj.second == coop::net::container_extract_result::kBirthFailed) {
                for (const ExtractKey& k : it->second)
                    if (rj.first == k.token) {
                        if (surfacedLossOut) *surfacedLossOut = 1;
                        rep.ok = true;
                        return rep;
                    }
            }
        // loss #2: the unbacked write (applied, intent never paired) for any claimant
        for (const ExtractKey& k : it->second)
            if (m.host.ledger.WasUnbacked(k)) {
                if (surfacedLossOut) *surfacedLossOut = 2;
                rep.ok = true;
                return rep;
            }
    }
    rep.code = "E-inv";
    rep.what = "terminal authoritativeCopies == " + std::to_string(copies) + " for '" +
               std::string(key.begin(), key.end()) + "'";
    return rep;
}

}  // namespace b1model
