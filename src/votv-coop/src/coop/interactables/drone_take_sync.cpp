// coop/drone_take_sync.cpp -- see coop/drone_take_sync.h (KROFNE FORK batch-1, corrective pass).

#include "coop/interactables/drone_take_sync.h"
#include "coop/interactables/drone_replay_domain.h"  // the carved pure admission domain (verbatim move)

#include "coop/dev/batch1_smoke.h"          // B1_* diagnostic milestones (log-only)

#include "coop/element/registry.h"
#include "coop/net/send_backlog.h"         // KROFNE FORK v3 (blocker N): critical-send retries
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_lifecycle.h"     // DestroyLocalProp (destroy-suppressed retire)

#include "ue_wrap/actors/prop.h"           // IsDescendantOfProp
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD (host/client ingest assertions)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"      // profile::name::{GameplayStaticsClass,FinishSpawningActorFn}
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"   // InstallPostHook (chains after the two existing hooks)
#include "ue_wrap/core/vm_dispatch.h"      // RegisterVirtualVerb + CurrentThreadVerb (the causal bracket)
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>

namespace coop::drone_take_sync {
namespace {

namespace R   = ue_wrap::reflection;
namespace GT  = ue_wrap::game_thread;
namespace VM  = ue_wrap::vm_dispatch;
namespace D   = ue_wrap::drone;

std::atomic<coop::net::Session*> g_session{nullptr};
inline coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The 0x45 observation verb: the CAUSAL one. `dropSack` is the verb the drone BP's take option
// (actionOptionIndex option 7 per the 2026-06-08 bytecode probe) dispatches; observing IT -- not
// the multiplexing option dispatcher -- is what makes the lane unambiguous by construction: a
// Use/Open (option 4 -> openPropInv) never dispatches dropSack, so it can never author a
// request. The verb NAME is still shared across classes in general, so the ctx class gate below
// remains mandatory (vm_dispatch.h: the name is the seam's filter; class discrimination is the
// consumer's job).
constexpr int        kVerbDropSack = 4101;  // module-local verb id (arbitrary, unique among consumers)
const wchar_t*       kVerbName     = L"dropSack";
bool                 g_verbRegistered = false;
void*                g_finishSpawnFn  = nullptr;
bool                 g_captureHookInstalled = false;

// ---- client pending-request state (game thread only) ----------------------------------------
uint32_t g_nonce = 0;              // per-session monotonic; 0 = none sent yet
// NOTE (client, BLOCKER D consistency): g_nonce keeps counting across a RECONNECT inside one
// process (it resets only on module OnDisconnect). The host-side domain is reset for the reused
// slot on disconnect / generation change, so ANY nonce the rejoining client sends evaluates
// fresh -- counting up is always safe, restarting at 1 would be too.

struct PendingRequest {
    uint32_t nonce = 0;
    void*    drone = nullptr;      // the mirror the take ran on (ctx identity)
    int32_t  droneIdx = -1;
    uint64_t armedMs = 0;
};
PendingRequest g_pending;                        // at most ONE in flight
bool           g_pendingValid = false;
constexpr size_t   kMaxPhantoms      = 1;        // a take spawns EXACTLY ONE prop_dronesack_C; a
                                                 // second in-bracket spawn is a fail-closed
                                                 // invariant violation (logged, not captured)
constexpr uint64_t kPendingTtlMs     = 10000;    // verdict lost -> clear WITHOUT destroying

// one captured phantom: the actor + its GUObjectArray slot (IsLiveByIndex-safe retire)
struct CapturedPhantom {
    void*   actor;
    int32_t idx;
};
CapturedPhantom g_phantom{};                     // the single exact capture for g_pending
bool            g_phantomValid = false;

// ---- the mirror-divergence tripwire ----------------------------------------------------------
// Reference: the gates the HOST streams onto this mirror (drone_sync's client apply calls
// NoteStreamedGates for every DroneState packet). If the LOCAL hasSack drops below the
// streamed truth with no pending request of ours, a client-side mutation ran that we did not
// observe -- either the dropSack verb did not surface at the 0x45 seam, or a verdict was lost
// and the pending expired. Rate-limited WARN; never a gameplay action (fail-closed diagnosability).
std::atomic<bool> g_streamedHasSack{false};
std::atomic<bool> g_streamedValid{false};
uint64_t          g_lastTripwireMs = 0;
constexpr uint64_t kTripwireCooldownMs = 10000;

}  // namespace

void NoteStreamedGates(bool /*canTakeOff*/, bool hasSack) {
    g_streamedHasSack.store(hasSack, std::memory_order_release);
    g_streamedValid.store(true, std::memory_order_release);
}

namespace {

// ---- pure validator (unit-tested by SelfTest) -----------------------------------------------
// The admission machinery (Admit/DroneVerdict/Admission/DroneReplayDomain/AdmitRequest/
// RecordVerdict) lives in coop/interactables/drone_replay_domain.h since the test-contract
// pass: carved VERBATIM so the headless suites compile the REAL shipped decisions.

uint8_t ValidateRequestPure(uint8_t op, uint32_t nonce, uint32_t lastNonce) {
    if (op != coop::net::drone_action_op::kDropSack) return coop::net::drone_action_result::kBadOp;
    if (nonce == 0 || nonce <= lastNonce) return coop::net::drone_action_result::kReplay;
    return coop::net::drone_action_result::kOk;
}

// ---- host per-slot replay domains (GT-only; [0] unused -- the host is not a requester) --------
DroneReplayDomain g_domain[coop::net::kMaxPeers];

// ---- CLIENT: the 0x45 dropSack observation callback --------------------------------------------
// Fires at the ENTRY of the drone's native dropSack dispatch (game thread only, guaranteed by
// the substrate). The take has ALREADY been selected by the player; by the time this returns,
// the drone's BP body has run dropSack client-locally (ghost sack + local hasSack flip) and the
// sack spawn is INSIDE this bracket (the capture hook relies on that window). All we do is
// forward the INTENT; authority stays with the host.
void OnDroneDropSackEntry(const VM::Bracket& br) {
    if (!GT::IsGameThread()) return;                    // substrate guarantees GT; defensive
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Client) return;   // HOST: the native take already ran -- never
                                                        // forward (no double dispatch)
    if (g_pendingValid) return;                         // one verdict at a time
    // EXACT context gate: dropSack by name is not drone-exclusive across the whole game. Without
    // the exact-class compare, some other class's dropSack-shaped verb would author a bogus
    // request.
    if (!D::IsDrone(br.ctx)) return;

    ++g_nonce;
    coop::net::DroneActionRequestPayload p{};
    p.nonce = g_nonce;
    p.op    = coop::net::drone_action_op::kDropSack;    // ONE operation: "perform your native dropSack"
    coop::dev::batch1_smoke::Emit("B1_DRONE_VERB", "observed dropSack drone=%p idx=%d",
                                  br.ctx, R::InternalIndexOf(br.ctx));
    coop::dev::batch1_smoke::Emit("B1_DRONE_REQ", "nonce=%u", g_nonce);
    // BLOCKER N: the REQUEST is the take's only representation on the wire -- a silently
    // refused send would strand the pending state (and, pre-O-fix, re-arm the Take option)
    // for the whole pending TTL. SendCritical parks the EXACT bytes (same nonce) on a refusal.
    coop::net::send_backlog::SendCritical(*s, coop::net::ReliableKind::DroneActionRequest, -1,
                                          &p, sizeof(p));
    g_pending = PendingRequest{g_nonce, br.ctx, R::InternalIndexOf(br.ctx), NowMs()};
    g_pendingValid = true;
    g_phantomValid = false;
    g_phantom = CapturedPhantom{};
    UE_LOGI("[DRONE-TAKE] CLIENT observed native dropSack (nonce=%u drone=%p idx=%d) -- request "
            "sent, exact phantom capture armed", g_nonce, br.ctx, g_pending.droneIdx);
}

// ---- CLIENT: the phantom-capture FinishSpawningActor post-hook --------------------------------
// Chains after host_spawn_watcher's + prop_drop_intent's callbacks on the same UFunction (the
// hook table supports multiple post hooks). Client-only, only while a request is pending, and
// ONLY for the exact causal product: inside OUR dropSack bracket on the pending drone, exact
// prop_dronesack_C class. The native body spawns the sack synchronously inside the verb, so the
// bracket is guaranteed live at this hook. NO radius/time fallback by design: an actor we cannot
// prove is the take's product is left alone (a missed capture is a logged ghost, not a wrongly
// destroyed world prop).
void OnFinishSpawnCapture(void* /*context*/, void* /*srcObj*/, void* result) {
    if (!GT::IsGameThread()) return;
    if (!g_pendingValid) return;
    if (g_phantomValid) return;                         // exactly one capture per request
    auto* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    void* actor = result;
    if (!actor || !R::IsLive(actor)) return;
    // Mirror exclusion FIRST: the host's REAL sack (an ordinary PropSpawn apply) is marked
    // incoming before its Finish -- it must NEVER be captured, whichever packet order wins.
    if (coop::prop_echo_suppress::PeekIncomingSpawn(actor)) return;
    // EXACT class: the take's product is prop_dronesack_C, full stop. Anything else inside the
    // bracket (orderbox/giftbox variants, containers, effects) is NOT the phantom.
    if (!D::IsDroneSack(actor)) return;
    if (coop::element::Registry::Get().EidForActor(actor) != coop::element::kInvalidId) return;
    if (coop::hand_item::IsHandAxisActor(actor)) return;

    // The causal identity check: inside OUR dropSack bracket on the pending drone.
    const VM::ActiveVerb av = VM::CurrentThreadVerb();
    const bool inBracket = av.active && av.verbId == kVerbDropSack && av.ctx == g_pending.drone;
    if (!inBracket) return;                             // not ours -- nothing to do with it

    g_phantom = CapturedPhantom{actor, R::InternalIndexOf(actor)};
    g_phantomValid = true;
    coop::dev::batch1_smoke::Emit("B1_DRONE_PHANTOM_CAPTURE", "actor=%p idx=%d nonce=%u",
                                 actor, g_phantom.idx, g_pending.nonce);
    UE_LOGI("[DRONE-TAKE] CLIENT captured drone-sack phantom %p idx=%d (nonce=%u, in-bracket, "
            "exact prop_dronesack_C)", actor, g_phantom.idx, g_pending.nonce);
}

void RetirePhantoms(const char* why) {
    if (!g_phantomValid) {
        UE_LOGW("[DRONE-TAKE] verdict '%s' with ZERO captured phantoms -- nothing to retire "
                "(the local sack spawn was not observed inside the bracket; if a ghost sack is "
                "visible it will be reconciled by the next DroneState re-assert -- REPORT this "
                "log)", why);
    }
    if (g_phantomValid && g_phantom.actor) {
        // Identity-exact retire (pointer + GUObjectArray slot liveness), class-proven, and
        // SELF-HEAL-SAFE: native ReceiveDestroyed spawns a replacement sack when takenByDrone is
        // false at destroy time, so the flag MUST be set first -- and a failed flag write means
        // we DO NOT destroy (a leftover ghost is recoverable; a self-healed replacement is a new
        // phantom minted by the cleanup itself).
        if (R::IsLiveByIndex(g_phantom.actor, g_phantom.idx)) {
            if (!D::IsDroneSack(g_phantom.actor)) {
                UE_LOGW("[DRONE-TAKE] phantom %p is not prop_dronesack_C at retire time -- "
                        "REFUSING to destroy (fail closed)", g_phantom.actor);
            } else if (!D::SetSackTakenByDrone(g_phantom.actor)) {
                UE_LOGW("[DRONE-TAKE] could not set takenByDrone on phantom %p -- REFUSING to "
                        "destroy (a destroy would trigger the native sack self-heal)", g_phantom.actor);
            } else {
                coop::prop_lifecycle::DestroyLocalProp(g_phantom.actor, /*deferred=*/false);
                UE_LOGI("[DRONE-TAKE] retired phantom %p idx=%d (takenByDrone set -> no "
                        "self-heal; %s)", g_phantom.actor, g_phantom.idx, why);
            }
        } else {
            UE_LOGI("[DRONE-TAKE] phantom %p already gone -- nothing to retire", g_phantom.actor);
        }
    }
    g_phantomValid = false;
    g_phantom = CapturedPhantom{};
}

// ---- pure-logic self test (runs once at Install; logs PASS/FAIL) -------------------------------
int SelfTest() {
    int fails = 0;
    auto check = [&fails](bool ok, const char* what) {
        if (!ok) { ++fails; UE_LOGW("[DRONE-TAKE][SELFTEST] FAIL: %s", what); }
    };
    // Wire shapes.
    check(sizeof(coop::net::DroneActionRequestPayload) == 8, "request payload is 8 B");
    check(sizeof(coop::net::DroneActionResultPayload) == 8, "result payload is 8 B");
    // Serialization round-trip (the exact memcpy the transport performs).
    {
        coop::net::DroneActionRequestPayload p{};
        p.nonce = 0xDEADBEEFu; p.op = coop::net::drone_action_op::kDropSack;
        uint8_t buf[sizeof(p)];
        std::memcpy(buf, &p, sizeof(p));
        coop::net::DroneActionRequestPayload q{};
        std::memcpy(&q, buf, sizeof(q));
        check(q.nonce == p.nonce && q.op == p.op && q._pad[0] == 0, "request round-trip");
    }
    // The op enum: zero is never valid; only kDropSack passes the pure gate.
    check(coop::net::drone_action_op::kNone == 0, "op kNone is 0 (zero-filled default is invalid)");
    // Validator: bad op fails closed; replay/zero nonce refused; fresh nonce passes.
    using namespace coop::net::drone_action_result;
    check(ValidateRequestPure(0x2A, 5, 4) == kBadOp, "unknown op denied kBadOp");
    check(ValidateRequestPure(coop::net::drone_action_op::kNone, 5, 4) == kBadOp, "zero op denied");
    check(ValidateRequestPure(coop::net::drone_action_op::kDropSack, 4, 4) == kReplay, "replayed nonce denied");
    check(ValidateRequestPure(coop::net::drone_action_op::kDropSack, 3, 4) == kReplay, "older nonce denied");
    check(ValidateRequestPure(coop::net::drone_action_op::kDropSack, 0, 0) == kReplay, "zero nonce denied");
    check(ValidateRequestPure(coop::net::drone_action_op::kDropSack, 5, 4) == kOk, "fresh nonce accepted");
    // BLOCKER D regression matrix (the pure replay-domain machinery, headless):
    // 1. duplicate ACCEPTED request -> cached verdict resent, NO re-execution.
    {
        DroneReplayDomain d;
        check(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 1).action == Admit::Execute,
              "D1: first request executes");
        RecordVerdict(d, 1, 1, kOk);
        const Admission dup = AdmitRequest(d, coop::net::drone_action_op::kDropSack, 1);
        check(dup.action == Admit::ResendVerdict && dup.verdict.accepted == 1,
              "D1: duplicate accepted -> cached accept resent, no execute");
    }
    // 2. duplicate DENIED request -> cached deny resent; the deny can never become executable.
    {
        DroneReplayDomain d;
        RecordVerdict(d, 7, 0, kNoCargo);              // denied while the gate was closed
        const Admission dup = AdmitRequest(d, coop::net::drone_action_op::kDropSack, 7);
        check(dup.action == Admit::ResendVerdict && dup.verdict.accepted == 0 &&
              dup.verdict.reason == kNoCargo,
              "D2: duplicate denied -> cached deny resent, no execute");
    }
    // 3. a denied nonce stays denied even after eviction (bounded cache).
    {
        DroneReplayDomain d;
        RecordVerdict(d, 7, 0, kNoCargo);
        for (uint32_t n = 8; n <= 30; ++n) RecordVerdict(d, n, 1, kOk);   // push 7 out of the cache
        check(d.verdicts.find(7) == d.verdicts.end(), "D3: eviction happened as designed");
        check(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 7).action == Admit::Refuse,
              "D3: evicted ancient nonce refused, never re-executed");
    }
    // 4. slot reuse: a RESET domain admits nonce 1 again (the rejoin case).
    {
        DroneReplayDomain d;
        RecordVerdict(d, 5, 1, kOk);
        check(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 1).action == Admit::Refuse,
              "D4: same domain refuses nonce 1 after 5 was accepted");
        d = DroneReplayDomain{};                       // what OnDisconnectForSlot does
        check(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 1).action == Admit::Execute,
              "D4: reset domain (slot reuse / disconnect) admits nonce 1 fresh");
    }
    // 5. zero nonce + wrong op refuse without executing.
    { DroneReplayDomain d;
      check(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 0).action == Admit::Refuse,
          "D5: zero nonce refused"); }
    { DroneReplayDomain d;
      check(AdmitRequest(d, 0xBB, 9).action == Admit::Refuse,
          "D5: unknown op refused"); }
    // BLOCKER O regression: the pending take masks the mirrored hasSack gate (and ONLY the
    // gate -- the streamed truth is recorded separately for diagnostics).
    check(coop::drone_take_sync::EffectiveMirrorHasSack(true, true) == false,
          "O1: pending take + streamed hasSack=true -> mirror gate FALSE (Take cannot re-arm)");
    check(coop::drone_take_sync::EffectiveMirrorHasSack(true, false) == true,
          "O2: no pending take + streamed hasSack=true -> mirror gate true (normal presentation)");
    check(coop::drone_take_sync::EffectiveMirrorHasSack(false, true) == false,
          "O3: pending take + streamed hasSack=false -> mirror gate false");
    if (fails == 0) UE_LOGI("[DRONE-TAKE][SELFTEST] PASS (serialization + validator + replay-domain + gate mask, 23 checks)");
    return fails;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    static bool s_selftestRun = false;
    if (!s_selftestRun) { s_selftestRun = true; SelfTest(); }
}

void Tick() {
    auto* s = LoadSession();
    if (!s || !s->connected()) return;

    if (!g_verbRegistered) {
        g_verbRegistered = VM::RegisterVirtualVerb(kVerbName, kVerbDropSack, &OnDroneDropSackEntry);
        if (g_verbRegistered)
            UE_LOGI("[DRONE-TAKE] 0x45 verb '%ls' registered (ctx-gated to the exact drone class) "
                    "-- native dropSack observation armed", kVerbName);
    }
    VM::TickResolvePending();

    // Phantom-capture hook: install once (the same lazy shape prop_drop_intent uses; a third post
    // hook on the same UFunction is supported by the hook table).
    if (!g_captureHookInstalled) {
        static int s_retry = 0;
        if (s_retry > 0) { --s_retry; }
        else {
            if (!g_finishSpawnFn) {
                void* gsCls = R::FindClass(ue_wrap::profile::name::GameplayStaticsClass);
                if (!gsCls) { s_retry = 60; }
                else g_finishSpawnFn = R::FindFunction(gsCls, ue_wrap::profile::name::FinishSpawningActorFn);
            }
            if (g_finishSpawnFn) {
                if (ue_wrap::ufunction_hook::InstallPostHook(g_finishSpawnFn, &OnFinishSpawnCapture)) {
                    g_captureHookInstalled = true;
                    UE_LOGI("[DRONE-TAKE] FinishSpawningActor capture post-hook installed");
                } else {
                    UE_LOGW("[DRONE-TAKE] capture post-hook FAILED (hook table full?) -- phantom "
                            "reconcile unavailable, accepted takes would leave the client ghost");
                    g_captureHookInstalled = true;  // permanent give-up (do not re-walk)
                }
            }
        }
    }

    // Verdict-lost expiry: clear WITHOUT destroying (see OnDisconnect rationale).
    if (g_pendingValid) {
        const bool droneGone = !R::IsLiveByIndex(g_pending.drone, g_pending.droneIdx);
        if (droneGone || NowMs() - g_pending.armedMs > kPendingTtlMs) {
            UE_LOGW("[DRONE-TAKE] pending request nonce=%u expired with no verdict (%s) -- "
                    "clearing capture state (phantom left in place)", g_pending.nonce,
                    droneGone ? "drone mirror gone" : "TTL");
            g_pendingValid = false;
            g_phantomValid = false;
            g_phantom = CapturedPhantom{};
        }
    }

    // Mirror-divergence tripwire (client only, rate-limited): local hasSack fell below the
    // streamed host truth with no request of ours in flight.
    if (s->role() == coop::net::Role::Client && !g_pendingValid && g_streamedValid.load(std::memory_order_acquire)) {
        void* drone = D::Find();
        bool canTakeOff = false, hasSack = false;
        if (drone && R::IsLive(drone) && D::ReadGateFields(drone, canTakeOff, hasSack) &&
            !hasSack && g_streamedHasSack.load(std::memory_order_acquire)) {
            const uint64_t nowMs = NowMs();
            if (nowMs - g_lastTripwireMs >= kTripwireCooldownMs) {
                g_lastTripwireMs = nowMs;
                UE_LOGW("[DRONE-TAKE] TRIPWIRE: mirror hasSack=false while the host stream says "
                        "true, with no pending take request -- an unobserved client-side sack "
                        "mutation ran (dropSack not visible at the 0x45 seam, or a verdict was "
                        "lost). Drone take will NOT be host-authorized -- investigate.");
            }
        }
    }
}

void OnRequest(coop::net::Session& session, const coop::net::DroneActionRequestPayload& p,
               uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("drone_take_sync::OnRequest");
    if (session.role() != coop::net::Role::Host) return;   // router also gates; defensive
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;  // host spoof gate

    // --- replay domain: bind to the slot's occupancy generation (BLOCKER D) ---
    // A new occupant on a reused slot (generation moved) starts a FRESH domain, so the rejoining
    // client's nonce 1 is a first request, not an ancient replay.
    DroneReplayDomain& d = g_domain[senderSlot];
    const uint32_t gen = session.peerGenerationForSlot(senderSlot);
    if (d.generation != gen) {
        UE_LOGI("[DRONE-TAKE] slot %u replay domain reset (occupancy generation %u -> %u)",
                static_cast<unsigned>(senderSlot), static_cast<unsigned>(d.generation),
                static_cast<unsigned>(gen));
        d = DroneReplayDomain{};
        d.generation = gen;
    }

    // Every verdict below goes back to ONE slot: route it through the backlog (blocker N) so a
    // refused GNS queue cannot silently eat a verdict and strand the client's pending state.
    const auto sendVerdict = [&session, senderSlot](uint32_t nonce, uint8_t accepted, uint8_t reason) {
        coop::net::DroneActionResultPayload r{};
        r.nonce = nonce;
        r.accepted = accepted;
        r.reason = reason;
        coop::net::send_backlog::SendCritical(session, coop::net::ReliableKind::DroneActionResult,
                                              static_cast<int>(senderSlot), &r, sizeof(r));
    };

    // --- pure admission (nonce cache + monotonicity + the operation constant) ---
    const Admission adm = AdmitRequest(d, p.op, p.nonce);
    if (adm.action != Admit::Execute) {
        if (adm.action == Admit::ResendVerdict) {
            // Duplicate/retransmit: re-send the CACHED verdict (both accept and deny). The native
            // take MUST NOT execute again -- the verdict, not the action, is idempotent.
            sendVerdict(p.nonce, adm.verdict.accepted, adm.verdict.reason);
            UE_LOGW("[DRONE-TAKE] HOST duplicate slot=%u nonce=%u -- re-sent cached verdict "
                    "(accepted=%u reason=%u), no re-execution",
                    senderSlot, p.nonce, adm.verdict.accepted, adm.verdict.reason);
        } else {
            // Hard refuse (kBadOp / zero nonce / ancient evicted nonce). Ancient nonces get NO
            // second verdict (the original rode the reliable lane); op/zero malformed requests
            // are answered so a broken client does not hang on a phantom.
            if (p.op != coop::net::drone_action_op::kDropSack || p.nonce == 0) {
                sendVerdict(p.nonce, adm.verdict.accepted, adm.verdict.reason);
            }
            UE_LOGW("[DRONE-TAKE] HOST refused slot=%u nonce=%u op=%u reason=%u (pure gate)",
                    senderSlot, p.nonce, p.op, adm.verdict.reason);
        }
        return;
    }

    // --- engine validation against the HOST's OWN drone (never client-supplied state) ---
    void* drone = D::Find();
    bool canTakeOff = false, hasSack = false;
    uint8_t reason = coop::net::drone_action_result::kOk;
    if (!drone || !R::IsLive(drone) || !D::ReadGateFields(drone, canTakeOff, hasSack))
        reason = coop::net::drone_action_result::kDroneGone;
    else if (!canTakeOff)
        reason = coop::net::drone_action_result::kNotArrived;
    else if (!hasSack)
        reason = coop::net::drone_action_result::kNoCargo;
    if (reason != coop::net::drone_action_result::kOk) {
        RecordVerdict(d, p.nonce, /*accepted=*/0, reason);   // the deny is FINAL for this nonce
        sendVerdict(p.nonce, 0, reason);
        UE_LOGW("[DRONE-TAKE] HOST denied slot=%u nonce=%u reason=%u (host gates canTakeOff=%d "
                "hasSack=%d)", senderSlot, p.nonce, reason, canTakeOff, hasSack);
        return;
    }

    // --- ACCEPT: commit the verdict, then run the drone's OWN native verb on the authority ---
    coop::dev::batch1_smoke::Emit("B1_DRONE_HOST_ACCEPT", "slot=%u nonce=%u",
                                  static_cast<unsigned>(senderSlot), p.nonce);
    const bool dispatched = D::DispatchDropSack(drone);
    if (dispatched)
        coop::dev::batch1_smoke::Emit("B1_DRONE_NATIVE_DROPSACK", "nonce=%u", p.nonce);
    RecordVerdict(d, p.nonce, dispatched ? 1 : 0,
                  dispatched ? coop::net::drone_action_result::kOk
                             : coop::net::drone_action_result::kDispatch);
    sendVerdict(p.nonce, dispatched ? 1 : 0,
                dispatched ? coop::net::drone_action_result::kOk
                           : coop::net::drone_action_result::kDispatch);
    if (dispatched) {
        UE_LOGI("[DRONE-TAKE] HOST accepted slot=%u nonce=%u -- native dropSack dispatched on %p; "
                "the FSM owns hasSack/departure, the real cargo rides host_spawn_watcher's PropSpawn",
                senderSlot, p.nonce, drone);
    } else {
        UE_LOGW("[DRONE-TAKE] HOST native dispatch FAILED slot=%u nonce=%u (verdict sent denied -- "
                "the client retires its phantom; the take can be re-pressed)", senderSlot, p.nonce);
    }
}

void OnResult(const coop::net::DroneActionResultPayload& p) {
    UE_ASSERT_GAME_THREAD("drone_take_sync::OnResult");
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Client) return;   // host->client only; defensive
    if (!g_pendingValid) {
        UE_LOGI("[DRONE-TAKE] CLIENT result nonce=%u accepted=%u reason=%u -- no pending request "
                "(late/duplicate verdict) -- ignored", p.nonce, p.accepted, p.reason);
        return;
    }
    if (p.nonce != g_pending.nonce) {
        UE_LOGW("[DRONE-TAKE] CLIENT result nonce=%u != pending nonce=%u -- ignored",
                p.nonce, g_pending.nonce);
        return;
    }
    coop::dev::batch1_smoke::Emit("B1_DRONE_RESULT", "nonce=%u accepted=%u reason=%u",
                                  p.nonce, static_cast<unsigned>(p.accepted),
                                  static_cast<unsigned>(p.reason));
    // Retire the exact captured phantom either way: ACCEPT = the host's real cargo arrives as
    // an ordinary mirrored PropSpawn; DENY = the host's state stands and the next DroneState
    // packet re-asserts the gates. Both paths must not keep a client-local ghost.
    RetirePhantoms(p.accepted ? "accepted" : "denied");
    UE_LOGI("[DRONE-TAKE] CLIENT verdict nonce=%u accepted=%u reason=%u -- request closed",
            p.nonce, p.accepted, p.reason);
    g_pendingValid = false;
    g_pending = PendingRequest{};
}

void OnDisconnect() {
    // The session is gone: clear the wire-facing state but DO NOT destroy a captured phantom --
    // if the host never processed the take, the phantom is the only cargo in what is now a local
    // world, and the drone's restored ReceiveTick (drone_sync::OnDisconnect) runs the native FSM.
    if (g_pendingValid)
        UE_LOGI("[DRONE-TAKE] session teardown: pending nonce=%u dropped, captured phantom left "
                "in the (now local) world", g_pending.nonce);
    g_pendingValid = false;
    g_pending = PendingRequest{};
    g_phantomValid = false;
    g_phantom = CapturedPhantom{};
    g_streamedValid.store(false, std::memory_order_release);
    for (auto& d : g_domain) d = DroneReplayDomain{};   // every replay domain dies with the session
}

void OnDisconnectForSlot(int slot) {
    // BLOCKER D: the leaver's replay domain dies with their occupancy. The NEXT occupant of this
    // slot starts a fresh replay domain even if the occupancy generation binding somehow missed
    // (belt and braces on top of the generation check in OnRequest).
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    UE_ASSERT_GAME_THREAD("drone_take_sync::OnDisconnectForSlot");
    if (g_domain[slot].generation != 0 || g_domain[slot].lastAcceptedNonce != 0 ||
        !g_domain[slot].verdicts.empty()) {
        UE_LOGI("[DRONE-TAKE] slot %u left -- replay domain reset (%zu cached verdict(s) dropped)",
                slot, g_domain[slot].verdicts.size());
    }
    g_domain[slot] = DroneReplayDomain{};
}

bool HasPendingTake() {
    // GT-only state; read from the GT (drone_sync's packet apply is GT). No lock: the mod's
    // invariant is that session ingest/apply/tick all run on one thread.
    return g_pendingValid;
}

}  // namespace coop::drone_take_sync
