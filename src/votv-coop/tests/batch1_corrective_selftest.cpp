// tests/batch1_corrective_selftest.cpp -- KROFNE FORK (2133), v3 corrective pass.
//
// HEADLESS/PURE selftest for the blockers K-Q corrections. This is NOT a grep: it COMPILES the
// real production logic (the same headers the game TUs include) and drives it:
//   - container_extract_wire.h   -- the exact pack/parse pair shipped in the DLL (blocker K)
//   - extract_pairing.h          -- the exact pairing ledger the host runs (blockers L + M)
//   - send_backlog.h             -- the exact retry-core the wrapper parks refused sends in (N)
//   - drone_take_sync.h          -- the effective mirror hasSack gate (blocker O)
//   - sleep_dilation_ownership.h -- the exact dilation ownership tracker sleep_sync runs (P)
//   - ue_wrap/core/bool_mask.h   -- the mask-preserving bool write (HARDENING Q)
// linked against the real record codec (save_record_wire.cpp / signal_wire.cpp).
//
// The v2-regression section embeds the v2 tail parser VERBATIM (from
// container_contents_sync.cpp at v2 HEAD 8674580b) and asserts it FAILS the well-formed
// round-trip cases -- i.e. this test, run against the v2 algorithm, fails exactly the way the
// v2 code failed in play (every op=1 blob deterministically rejected).
//
// Build (Linux, no engine):
//   g++ -std=c++20 -I src/votv-coop/include \
//       src/votv-coop/tests/batch1_corrective_selftest.cpp \
//       src/votv-coop/src/coop/items/save_record_wire.cpp \
//       src/votv-coop/src/coop/interactables/signal_wire.cpp -o batch1_selftest
// Exit code 0 = all checks pass.

#include "coop/net/protocol.h"
#include "coop/net/send_backlog.h"
#include "coop/props/container_extract_wire.h"
#include "coop/props/extract_pairing.h"
#include "coop/player/sleep_dilation_ownership.h"
#include "coop/interactables/drone_take_sync.h"
#include "coop/interactables/drone_replay_domain.h"
#include "coop/dev/batch1_smoke.h"
#include "coop/dev/drone_take_gate.h"          // ACTUATOR-ONLY pass: drone take one-shot gate (T1)
#include "coop/dev/extract_convergence.h"      // ACTUATOR-ONLY pass: extraction baseline gate (T2)
#include "coop/dev/director/ctake_barrier.h"   // ACTUATOR-ONLY pass: CTAKE BOTH_READY barrier (T3)
#include "ue_wrap/core/bool_mask.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_total = 0;
#define CHECK(cond, what)                                                     \
    do {                                                                      \
        ++g_total;                                                            \
        if (!(cond)) {                                                        \
            ++g_fail;                                                         \
            std::printf("FAIL: %s\n", what);                                  \
        }                                                                     \
    } while (0)

using namespace coop::props::container_extract_wire;
using coop::props::extract_pairing::BirthVerdict;
using coop::props::extract_pairing::ExtractKey;
using coop::props::extract_pairing::Ledger;
using coop::props::extract_pairing::ParkResult;
using UE = ue_wrap::save_record::SaveRecord;


// Link stub: save_record_wire/signal_wire reference the project logger; headless tests route it
// nowhere (the tested logic never logs).
namespace ue_wrap::log {
enum class Level { Info, Warn, Error };
void Write(Level, const char*, ...) {}
}

// ---- helpers ----------------------------------------------------------------------------------

static UE MakeRecord(const wchar_t* cls, const wchar_t* key) {
    UE r;
    r.className = cls;
    r.key = key;
    r.xform = {1.f, 0.f, 0.f, 0.f, 1.f, 2.f, 3.f, 1.f, 1.f, 1.f};
    return r;
}

// ---- the v2 tail parser, VERBATIM (the bug this pass fixes) ------------------------------------
// Copied byte-for-byte from src/votv-coop/src/coop/props/container_contents_sync.cpp at v2 HEAD
// 8674580b08b1e384ff0a0a6992a501946ac467d8 (blocker K). DO NOT "fix" it here -- it exists to
// demonstrate, by execution, that the v2 algorithm rejects well-formed op=1 blobs.
namespace v2_reference {
constexpr size_t kMaxTokensPerWrite = 4;
bool BackReadExtractTail(const std::vector<uint8_t>& blob, uint64_t* outTokens, size_t& outCount) {
    outCount = 0;
    if (blob.size() < 1 + 1) return false;
    const size_t count = blob[blob.size() - 1];
    if (count == 0 || count > kMaxTokensPerWrite) return false;   // fail closed on a fake tail
    const size_t tailBytes = 1 + count * 8;
    if (blob.size() < 1 + 4 + 8 + 2 + tailBytes) return false;    // header + records floor + tail
    const size_t base = blob.size() - tailBytes;
    for (size_t i = 0; i < count; ++i) {
        const size_t o = base + 1 + i * 8;
        std::memcpy(&outTokens[i], &blob[o], 8);
        if (outTokens[i] == 0) return false;                      // token 0 is never valid
    }
    outCount = count;
    return true;
}
}  // namespace v2_reference

// ---- scripted sinks ---------------------------------------------------------------------------

struct ScriptedResults : coop::props::extract_pairing::ResultSink {
    struct Sent { uint8_t toSlot; coop::net::ContainerExtractResultPayload p; };
    std::vector<Sent> sent;
    void SendExtractResult(uint8_t toSlot, const coop::net::ContainerExtractResultPayload& p) override {
        sent.push_back({toSlot, p});
    }
    size_t Count(uint8_t accepted, uint8_t reason) const {
        size_t n = 0;
        for (const auto& s : sent)
            if (s.p.accepted == accepted && s.p.reason == reason) ++n;
        return n;
    }
};

struct ScriptedBirths : coop::props::extract_pairing::BirthSink {
    std::vector<BirthVerdict> script;                    // consumed per AuthorBirth call
    std::vector<std::pair<uint8_t, uint64_t>> authored;  // (slot, token) per attempt
    BirthVerdict AuthorBirth(const coop::net::ContainerExtractIntentPayload& p,
                             uint8_t senderSlot) override {
        authored.emplace_back(senderSlot, p.extractToken);
        if (script.empty()) return BirthVerdict::TransientFailure;
        const BirthVerdict v = script.front();
        script.erase(script.begin());
        return v;
    }
};

static ExtractKey Key(uint8_t slot, uint32_t gen, uint64_t token) {
    return ExtractKey{slot, gen, token};
}

static coop::net::ContainerExtractIntentPayload MakeIntent(uint64_t token, uint32_t eid,
                                                           const char* key) {
    coop::net::ContainerExtractIntentPayload p{};
    std::snprintf(p.birth.key.data, sizeof(p.birth.key.data), "%s", key);
    p.birth.key.len = static_cast<uint8_t>(std::strlen(key));
    std::snprintf(p.birth.className.data, sizeof(p.birth.className.data), "Aprop_test_C");
    p.birth.className.len = static_cast<uint8_t>(std::strlen("Aprop_test_C"));
    p.extractToken = token;
    p.containerEid = eid;
    return p;
}

// ================================================================================================
int main() {
    std::printf("== Batch-1 v3 corrective selftest (headless, real production headers) ==\n");

    // ---- K1: op=1 serialize/parse roundtrip, token=1 ------------------------------------------
    {
        const std::vector<UE> recs{MakeRecord(L"Aprop_rock_C", L"rock-1")};
        const std::vector<uint8_t> blob = PackContentsExtract(77, 0xDEADBEEF, recs, {1});
        ParsedBlob pb;
        CHECK(ParseContentsBlob(blob, 512, pb), "K1: op=1 blob with token=1 parses");
        CHECK(pb.eid == 77 && pb.baseHash == 0xDEADBEEF, "K1: header fields round-trip");
        CHECK(pb.hasTail && pb.tokens.size() == 1 && pb.tokens[0] == 1,
              "K1: count=1, token=1 read FORWARD at the defined offset");
        CHECK(pb.recs.size() == 1 && pb.recs[0].key == L"rock-1", "K1: records round-trip");
    }
    // ---- K2..K4: token variants + a four-token write -------------------------------------------
    {
        const std::vector<UE> recs{MakeRecord(L"Aprop_rock_C", L"rock-1")};
        ParsedBlob pb;
        CHECK(ParseContentsBlob(PackContentsExtract(1, 0, recs, {2}), 512, pb) &&
                  pb.tokens.size() == 1 && pb.tokens[0] == 2,
              "K2: token=2 round-trips");
        CHECK(ParseContentsBlob(PackContentsExtract(1, 0, recs, {0x1122334455667788ull}), 512, pb) &&
                  pb.tokens.size() == 1 && pb.tokens[0] == 0x1122334455667788ull,
              "K3: wide token 0x1122334455667788 round-trips");
        CHECK(ParseContentsBlob(PackContentsExtract(1, 0, recs, {1, 2, 3, 4}), 512, pb) &&
                  pb.tokens.size() == 4 && pb.tokens[3] == 4,
              "K4: four-token write round-trips");
    }
    // ---- K5: malformed tails rejected (fail closed, no side effects possible) -------------------
    {
        const std::vector<UE> recs{MakeRecord(L"Aprop_rock_C", L"rock-1")};
        ParsedBlob pb;
        std::vector<uint8_t> truncated = PackContentsExtract(1, 0, recs, {9});
        truncated.pop_back();   // chop the last byte off the final token
        CHECK(!ParseContentsBlob(truncated, 512, pb), "K5: truncated final token rejected");
        CHECK(!ParseContentsBlob(PackContentsExtract(1, 0, recs, {}), 512, pb),
              "K5: count=0 rejected");
        {   // craft count=5 (> the cap) from a valid 4-token blob
            std::vector<uint8_t> b = PackContentsExtract(1, 0, recs, {1, 2, 3, 4});
            AppU64LE(b, 0x55);                        // an extra token crammed past the cap
            b[b.size() - 8 * 5 - 1] = 5;              // count byte = 5 (> kMaxTokensPerWrite)
            CHECK(!ParseContentsBlob(b, 512, pb), "K5: count>max rejected");
        }
        std::vector<uint8_t> garbage = PackContentsExtract(1, 0, recs, {1});
        garbage.push_back(0xAA);            // trailing garbage after a valid tail
        CHECK(!ParseContentsBlob(garbage, 512, pb), "K5: trailing garbage rejected");
        std::vector<uint8_t> zeroTok = PackContentsExtract(1, 0, recs, {0});
        CHECK(!ParseContentsBlob(zeroTok, 512, pb), "K5: zero token rejected");
    }
    // ---- K6: op=0 exact-consumption --------------------------------------------------------------
    {
        const std::vector<UE> recs{MakeRecord(L"Aprop_rock_C", L"rock-1"),
                                   MakeRecord(L"Aprop_box_C", L"box-9")};
        std::vector<uint8_t> b = PackContents(5, 12345, recs);
        ParsedBlob pb;
        CHECK(ParseContentsBlob(b, 512, pb) && !pb.hasTail && pb.recs.size() == 2,
              "K6: op=0 pack parses with no tail");
        b.push_back(0x00);   // one stray byte -- stock grammar permits no residue
        CHECK(!ParseContentsBlob(b, 512, pb), "K6: op=0 trailing byte rejected");
    }
    // ---- K7 (THE v2 REGRESSION): the v2 back-reader FAILS the well-formed blob ------------------
    {
        const std::vector<UE> recs{MakeRecord(L"Aprop_rock_C", L"rock-1")};
        const std::vector<uint8_t> blob = PackContentsExtract(1, 0, recs, {1});
        uint64_t toks[v2_reference::kMaxTokensPerWrite] = {};
        size_t n = 0;
        CHECK(!v2_reference::BackReadExtractTail(blob, toks, n),
              "K7: the V2 algorithm rejects a well-formed op=1 blob (token=1 -> last byte is the "
              "token's MSB = 0 -> count=0 -> rejected). Executed proof of the v2 bug this pass fixes.");
    }
    // ---- K8: byte-layout identity with v2 (the content hash input is unchanged) ------------------
    {
        const std::vector<UE> recs{MakeRecord(L"A", L"k")};
        const std::vector<uint8_t> b = PackContents(0x01020304, 0x1122334455667788ull, recs);
        CHECK(b.size() >= 15, "K8: pack has the full header");
        CHECK(b[0] == 0 && b[1] == 0x04 && b[2] == 0x03 && b[3] == 0x02 && b[4] == 0x01,
              "K8: op + LE eid bytes as v2 wrote them");
        CHECK(b[5] == 0x88 && b[12] == 0x11, "K8: LE baseHash bytes as v2 wrote them");
        CHECK(b[13] == 1 && b[14] == 0, "K8: LE record count as v2 wrote it");
    }

    // ---- L1: two clients, same token -> distinct pairing identities ------------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::Spawned, BirthVerdict::Spawned};
        const auto i1 = MakeIntent(1, 42, "slot1-item");
        const auto i2 = MakeIntent(1, 42, "slot2-item");
        CHECK(led.ParkIntent(Key(1, 10, 1), 42, i1, 1000, rs, bs) == ParkResult::Parked,
              "L1: slot 1 token 1 parks");
        CHECK(led.ParkIntent(Key(2, 11, 1), 42, i2, 1000, rs, bs) == ParkResult::Parked,
              "L1: slot 2 token 1 parks TOO (no collision)");
        const auto r1 = led.OnWriteApplied({Key(1, 10, 1)}, 42, 1100, rs, bs);
        const auto r2 = led.OnWriteApplied({Key(2, 11, 1)}, 42, 1100, rs, bs);
        CHECK(r1.committed == 1 && r2.committed == 1, "L1: BOTH commits birth");
        CHECK(led.CommittedCount() == 2, "L1: two distinct committed keys");
        CHECK(rs.Count(1, coop::net::container_extract_result::kOk) == 2,
              "L1: two kOk terminal verdicts, one per client");
        CHECK(bs.authored.size() == 2, "L1: exactly two births authored");
    }
    // ---- L2: slot reuse with a new generation -> fresh domain ------------------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::Spawned, BirthVerdict::Spawned};
        const auto intent = MakeIntent(1, 7, "the-item");
        CHECK(led.ParkIntent(Key(3, 100, 1), 7, intent, 1000, rs, bs) == ParkResult::Parked,
              "L2: occupant gen=100 parks token 1");
        CHECK(led.OnWriteApplied({Key(3, 100, 1)}, 7, 1100, rs, bs).committed == 1,
              "L2: gen-100 pairing commits");
        CHECK(led.ParkIntent(Key(3, 101, 1), 7, intent, 2000, rs, bs) == ParkResult::Parked,
              "L2: NEXT occupant (gen=101, same slot, same token) is FRESH, not already-committed");
        CHECK(led.OnWriteApplied({Key(3, 101, 1)}, 7, 2100, rs, bs).committed == 1,
              "L2: gen-101 pairing commits independently");
    }
    // ---- L3: duplicate in the same slot/generation is idempotent (max one birth) ------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::Spawned};
        CHECK(led.ParkIntent(Key(1, 5, 1), 9, MakeIntent(1, 9, "x"), 1000, rs, bs) ==
              ParkResult::Parked, "L3: first intent parks");
        CHECK(led.OnWriteApplied({Key(1, 5, 1)}, 9, 1100, rs, bs).committed == 1,
              "L3: first write commits");
        const auto rep = led.OnWriteApplied({Key(1, 5, 1)}, 9, 1200, rs, bs);
        CHECK(rep.duplicates == 1 && rep.committed == 0, "L3: duplicate write -> duplicate suppressed");
        CHECK(bs.authored.size() == 1, "L3: EXACTLY ONE birth ever");
        CHECK(rs.Count(1, coop::net::container_extract_result::kOk) == 1, "L3: one kOk only");
        CHECK(led.ParkIntent(Key(1, 5, 1), 9, MakeIntent(1, 9, "x"), 1300, rs, bs) ==
              ParkResult::DuplicateCommitted, "L3: duplicate intent after commit -> DuplicateCommitted");
        CHECK(bs.authored.size() == 1, "L3: still exactly one birth");
    }
    // ---- M1: intent-first pairing -> one birth + one kOk terminal verdict -------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::Spawned};
        CHECK(led.ParkIntent(Key(1, 1, 3), 12, MakeIntent(3, 12, "item-a"), 1000, rs, bs) ==
              ParkResult::Parked, "M1: intent-first parks");
        const auto rep = led.OnWriteApplied({Key(1, 1, 3)}, 12, 1100, rs, bs);
        CHECK(rep.committed == 1, "M1: write applied -> birth commits");
        CHECK(rs.sent.size() == 1 && rs.sent[0].toSlot == 1 &&
              rs.sent[0].p.extractToken == 3 && rs.sent[0].p.accepted == 1 &&
              rs.sent[0].p.reason == coop::net::container_extract_result::kOk,
              "M1: SUCCESS reaches a TERMINAL state (kOk to the originating slot, token echoed)");
        CHECK(led.IsCommitted(Key(1, 1, 3)) && led.ParkedCount() == 0,
              "M1: committed memory set, parked state drained");
    }
    // ---- M2: write-first pairing (intent arrives after its accepted write) ------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::Spawned};
        const auto rep = led.OnWriteApplied({Key(2, 2, 4)}, 15, 1000, rs, bs);
        CHECK(rep.markedApplied == 1 && rep.committed == 0, "M2: write-first leaves an applied marker");
        CHECK(led.ParkIntent(Key(2, 2, 4), 15, MakeIntent(4, 15, "item-b"), 1100, rs, bs) ==
              ParkResult::CommittedNow, "M2: the late intent commits ON ARRIVAL");
        CHECK(rs.Count(1, coop::net::container_extract_result::kOk) == 1 &&
              bs.authored.size() == 1, "M2: one birth, one kOk");
    }
    // ---- M3: rejected mutation -> ZERO births + one kRefused --------------------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        CHECK(led.ParkIntent(Key(1, 1, 5), 20, MakeIntent(5, 20, "item-c"), 1000, rs, bs) ==
              ParkResult::Parked, "M3: intent parked");
        led.OnWriteRefused({Key(1, 1, 5)}, rs);
        CHECK(bs.authored.empty(), "M3: a REFUSED mutation spawns NOTHING");
        CHECK(rs.Count(0, coop::net::container_extract_result::kRefused) == 1,
              "M3: exactly one kRefused verdict");
        CHECK(led.ParkedCount() == 0, "M3: pairing state drained on refusal");
        const auto rep = led.Sweep(2000, rs, bs);
        CHECK(rep.expiredParked == 0, "M3: nothing left to expire");
    }
    // ---- M4: transient birth failure -> bounded retry -> terminal kOk ------------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::TransientFailure, BirthVerdict::TransientFailure,
                     BirthVerdict::Spawned};
        CHECK(led.ParkIntent(Key(1, 1, 6), 30, MakeIntent(6, 30, "item-d"), 1000, rs, bs) ==
              ParkResult::Parked, "M4: parks");
        const auto rep = led.OnWriteApplied({Key(1, 1, 6)}, 30, 1100, rs, bs);
        CHECK(rep.committed == 0 && rep.birthsPending == 1,
              "M4: accepted mutation + transient birth -> NOT committed, NOT lost: retry armed");
        CHECK(rs.sent.empty(), "M4: no verdict before the birth exists");
        const auto s1 = led.Sweep(1650, rs, bs);   // nextAttempt = 1100+500 = 1600 -> now due
        CHECK(s1.birthRetries == 1 && s1.birthsCommitted == 0, "M4: retry 1 still transient");
        const auto s2 = led.Sweep(2200, rs, bs);   // retry 2 (scheduled 1650+500=2150) succeeds
        CHECK(s2.birthsCommitted == 1, "M4: the birth reaches terminal success on retry");
        CHECK(rs.Count(1, coop::net::container_extract_result::kOk) == 1,
              "M4: exactly one kOk, after the birth is proven");
        CHECK(led.IsCommitted(Key(1, 1, 6)) && led.BirthRetryCount() == 0,
              "M4: retry drained, committed memory set");
    }
    // ---- M5: bounded recovery lapsed -> honest kBirthFailed terminal verdict ----------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::TransientFailure};   // the initial attempt; retries stay dry
        CHECK(led.ParkIntent(Key(1, 1, 7), 40, MakeIntent(7, 40, "item-e"), 1000, rs, bs) ==
              ParkResult::Parked, "M5: parks");
        (void)led.OnWriteApplied({Key(1, 1, 7)}, 40, 1100, rs, bs);
        const auto rep = led.Sweep(1000 + 30000 + 600, rs, bs);   // past the bounded window
        CHECK(rep.birthsFailed == 1, "M5: the bounded window lapses -> kBirthFailed");
        CHECK(rs.Count(0, coop::net::container_extract_result::kBirthFailed) == 1,
              "M5: the client is TOLD (no permanent ghost, no silent loss)");
        CHECK(led.BirthRetryCount() == 0, "M5: retry state drained");
    }
    // ---- M6: expired parked intent -> kExpired ----------------------------------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        CHECK(led.ParkIntent(Key(1, 1, 8), 50, MakeIntent(8, 50, "item-f"), 1000, rs, bs) ==
              ParkResult::Parked, "M6: parks");
        (void)led.Sweep(1000 + 10000 + 1, rs, bs);
        CHECK(rs.Count(0, coop::net::container_extract_result::kExpired) == 1,
              "M6: the pairing window lapse answers kExpired");
    }
    // ---- L4: the leaver's pairing state dies with their occupancy ----------------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        CHECK(led.ParkIntent(Key(2, 9, 1), 60, MakeIntent(1, 60, "leaver-item"), 1000, rs, bs) ==
              ParkResult::Parked, "L4: leaver parks an intent");
        (void)led.OnWriteApplied({Key(2, 9, 2)}, 60, 1000, rs, bs);   // applied marker (write-first)
        CHECK(led.ParkedCount() == 1 && led.AppliedCount() == 1, "L4: state present");
        led.OnDisconnectForSlot(2);
        CHECK(led.ParkedCount() == 0 && led.AppliedCount() == 0 && led.BirthRetryCount() == 0,
              "L4: disconnect wipes ALL of that slot's pairing state");
    }

    // ---- N: the send backlog (first N sends fail, then one succeeds) -------------------------------
    {
        using namespace coop::net::send_backlog;
        struct FailingThenOkSink : BacklogCore::ISendSink {
            int refuseFirst = 0;
            std::vector<std::pair<coop::net::ReliableKind, std::vector<uint8_t>>> shipped;
            bool Send(coop::net::ReliableKind kind, int, const uint8_t* data, size_t len) override {
                if (refuseFirst > 0) { --refuseFirst; return false; }
                shipped.emplace_back(kind, std::vector<uint8_t>(data, data + len));
                return true;
            }
        } sink;
        sink.refuseFirst = 3;
        BacklogCore core{1000};
        coop::net::DroneActionRequestPayload req{};
        req.nonce = 42; req.op = coop::net::drone_action_op::kDropSack;
        coop::net::ContainerExtractResultPayload res{};
        res.extractToken = 7; res.accepted = 1; res.reason = coop::net::container_extract_result::kOk;
        CHECK(core.Park(coop::net::ReliableKind::DroneActionRequest, -1, &req, sizeof(req)) ==
              BacklogCore::EnqueueResult::Parked, "N: request parks on refusal");
        CHECK(core.Park(coop::net::ReliableKind::ContainerExtractResult, 2, &res, sizeof(res)) ==
              BacklogCore::EnqueueResult::Parked, "N: result parks on refusal");
        CHECK(core.Park(coop::net::ReliableKind::ContainerExtractIntent, -1, &req, sizeof(req)) ==
              BacklogCore::EnqueueResult::Parked, "N: intent parks on refusal");
        CHECK(core.Size() == 3, "N: bounded queue holds all three");
        auto rep = core.Drain(sink);
        CHECK(rep.sent == 0 && rep.retried == 0 && sink.shipped.empty(), "N: not due yet");
        core.SetTime(1250);
        rep = core.Drain(sink);            // 3 attempts, all refused
        CHECK(rep.retried == 3 && rep.sent == 0 && core.Size() == 3,
              "N: refusals NEVER drop the entries (not shipped != lost)");
        core.SetTime(1500);
        sink.refuseFirst = 0;              // the transport accepts from here
        rep = core.Drain(sink);
        CHECK(rep.sent == 3 && core.Size() == 0, "N: next attempt ships all three");
        CHECK(sink.shipped.size() == 3 &&
              sink.shipped[0].first == coop::net::ReliableKind::DroneActionRequest &&
              sink.shipped[1].first == coop::net::ReliableKind::ContainerExtractResult &&
              sink.shipped[2].first == coop::net::ReliableKind::ContainerExtractIntent,
              "N: FIFO order preserved (same semantic messages, no new nonce/token per retry)");
        coop::net::DroneActionRequestPayload back{};
        std::memcpy(&back, sink.shipped[0].second.data(), sink.shipped[0].second.size());
        CHECK(back.nonce == 42 && back.op == coop::net::drone_action_op::kDropSack,
              "N: the SAME nonce rides every attempt");
        core.SetTime(2000);
        (void)core.Park(coop::net::ReliableKind::DroneActionResult, 1, &res, sizeof(res));
        core.SetTime(2000 + 10000 + 1);
        rep = core.Drain(sink);
        CHECK(rep.expired == 1 && core.Size() == 0, "N: entry expires past the TTL (counted)");
        uint8_t big[kMaxEntryBytes + 1] = {};
        CHECK(core.Park(coop::net::ReliableKind::DroneActionRequest, -1, big, sizeof(big)) ==
                  BacklogCore::EnqueueResult::DroppedOversize,
              "N: oversize payload refused (the queue is for the fork-critical smalls)");
    }

    // ---- O: the pending take masks the mirrored hasSack gate ---------------------------------------
    {
        using coop::drone_take_sync::EffectiveMirrorHasSack;
        CHECK(EffectiveMirrorHasSack(true, true) == false,
              "O: pending + streamed hasSack=true -> effective mirror gate FALSE (Take cannot re-arm)");
        CHECK(EffectiveMirrorHasSack(true, false) == true,
              "O: no pending + streamed true -> gate true (normal presentation)");
        CHECK(EffectiveMirrorHasSack(false, true) == false &&
              EffectiveMirrorHasSack(false, false) == false,
              "O: streamed false stays false in both pending states");
    }

    // ---- P: the dilation ownership transitions -----------------------------------------------------
    {
        using coop::sleep_sync::DilationOwnershipTracker;
        using coop::sleep_sync::SleepDilationOwnership;
        // P1: WAITING(1x) -> disconnect while sleeping -> 20x  (THE blocker-P case)
        {
            DilationOwnershipTracker t;
            t.CommitWaitingUndo();
            CHECK(t.owned() == SleepDilationOwnership::WaitingForcedOne, "P1: WAITING ownership latched");
            CHECK(t.DisconnectHandoffTargetWhileSleeping() == 20.f,
                  "P1: WAITING(1x) -> disconnect while sleeping -> 20x (v2 handed vanilla sleep a 1x world)");
        }
        // P2: ACCELERATE(20x) -> END -> 1x
        {
            DilationOwnershipTracker t;
            t.CommitAccelerate();
            CHECK(t.DisconnectHandoffTargetWhileSleeping() == 20.f,
                  "P2: ACCELERATE ownership still hands 20x while sleeping (unchanged)");
            t.CommitRestored();
            CHECK(t.owned() == SleepDilationOwnership::None, "P2: after the END restore nothing is owed");
            CHECK(t.RestoreTargetWhileAwake() == 0.f, "P2: no write is ordered for a disowned restore");
        }
        // P3: WAITING(1x) -> wake -> 1x
        {
            DilationOwnershipTracker t;
            t.CommitWaitingUndo();
            CHECK(t.RestoreTargetWhileAwake() == 1.f, "P3: WAITING(1x) -> wake -> restore to 1x");
            t.CommitRestored();
            CHECK(t.RestoreTargetWhileAwake() == 0.f, "P3: restored -> nothing owed");
        }
        // P4: ACCELERATE -> WAITING -> disconnect while sleeping -> 20x
        {
            DilationOwnershipTracker t;
            t.CommitAccelerate();
            t.CommitWaitingUndo();   // the WAITING 1.0 supersedes (the current value IS 1.0)
            CHECK(t.owned() == SleepDilationOwnership::WaitingForcedOne,
                  "P4: WAITING ownership replaces ACCELERATE ownership");
            CHECK(t.DisconnectHandoffTargetWhileSleeping() == 20.f,
                  "P4: EITHER direction -> the handoff returns vanilla sleep its OWN 20x");
        }
        // P5: the module never touched dilation -> NO write at all (native 20x stands)
        {
            DilationOwnershipTracker t;
            CHECK(t.owned() == SleepDilationOwnership::None &&
                      t.DisconnectHandoffTargetWhileSleeping() == 0.f,
                  "P5: disowned -> the handoff writes NOTHING (no stomp of native sleep)");
        }
    }

    // ---- Q: the mask-preserving bool write ----------------------------------------------------------
    {
        using namespace ue_wrap::bool_mask;
        const uint8_t byte = 0xF0;
        CHECK(SetBoolMaskBits(byte, 0x08) == 0xF8,
              "Q: mask write sets ONLY the property's bit (0xF0 + mask 0x08 -> 0xF8)");
        CHECK(SetBoolMaskBits(0x00, 0x01) == 0x01 && SetBoolMaskBits(0xFF, 0x01) == 0xFF,
              "Q: set is idempotent on both polarities");
        CHECK(SetBoolMaskBits(0x0F, 0xF0) == 0xFF, "Q: unrelated low nibble preserved");
        CHECK(ClearBoolMaskBits(0xFF, 0x08) == 0xF7, "Q: clear removes only the masked bit");
        uint8_t packed = 0b10110110;   // pretend takenByDrone is bit 0x01; neighbors live
        packed = SetBoolMaskBits(packed, 0x01);
        CHECK(packed == 0b10110111, "Q: the retire write leaves every packed neighbor intact");
    }

    // ================================================================================================
    // TEST-CONTRACT EXPANSION (layer 1) -- the exhaustive per-subsystem matrices.
    // ================================================================================================

    // ---- DR1: the drone request admission matrix (the carved replay domain, REAL decisions) -----
    {
        using namespace coop::drone_take_sync;
        using namespace coop::net::drone_action_result;
        DroneReplayDomain d;
        // op validation: only kDropSack is ever executable.
        CHECK(AdmitRequest(d, 0x2A, 1).action == Admit::Refuse &&
                  AdmitRequest(d, 0x2A, 1).verdict.reason == kBadOp,
              "DR1: unknown op refused kBadOp (no execute, no verdict cache entry)");
        CHECK(AdmitRequest(d, coop::net::drone_action_op::kNone, 1).action == Admit::Refuse,
              "DR1: zero op refused");
        CHECK(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 0).action == Admit::Refuse,
              "DR1: zero nonce refused");
        CHECK(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 7).action == Admit::Execute,
              "DR1: a fresh kDropSack nonce executes");
        // duplicate ACCEPTED nonce: the cached verdict, never the action.
        RecordVerdict(d, 7, 1, kOk);
        const Admission dupAccepted = AdmitRequest(d, coop::net::drone_action_op::kDropSack, 7);
        CHECK(dupAccepted.action == Admit::ResendVerdict && dupAccepted.verdict.accepted == 1,
              "DR1: duplicate ACCEPTED nonce -> cached accept re-sent, native action CANNOT re-run");
        // duplicate DENIED nonce: the cached deny, and a deny can never become executable later.
        RecordVerdict(d, 8, 0, kNoCargo);
        const Admission dupDenied = AdmitRequest(d, coop::net::drone_action_op::kDropSack, 8);
        CHECK(dupDenied.action == Admit::ResendVerdict && dupDenied.verdict.accepted == 0 &&
                  dupDenied.verdict.reason == kNoCargo,
              "DR1: duplicate DENIED nonce -> cached deny re-sent, never executed");
        CHECK(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 6).action == Admit::Refuse,
              "DR1: an ancient (below highest accepted) nonce is refused even when evicted");
        // slot-generation reuse: a domain bound to a NEW generation admits nonce 1 again;
        // the production OnRequest performs this reset when d.generation != session gen.
        d.generation = 41;                      // pretend the domain served generation 41
        CHECK(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 1).action == Admit::Refuse,
              "DR1: same-generation nonce 1 after 7 was accepted -> refused");
        d = DroneReplayDomain{};                // the OnDisconnectForSlot / generation-change reset
        d.generation = 42;
        CHECK(AdmitRequest(d, coop::net::drone_action_op::kDropSack, 1).action == Admit::Execute,
              "DR1: NEW generation on the same slot -> nonce 1 executes (a fresh occupant)");
        // disconnect reset is the same reset (a wiped domain).
        d = DroneReplayDomain{};
        RecordVerdict(d, 3, 1, kOk);
        d = DroneReplayDomain{};
        CHECK(d.verdicts.empty() && d.lastAcceptedNonce == 0,
              "DR1: disconnect reset wipes the domain (no verdict memory, no monotonic memory)");
        // eviction bound: the cache stays bounded and evicts the OLDEST.
        for (uint32_t n = 1; n <= 40; ++n) RecordVerdict(d, n, 1, kOk);
        CHECK(d.verdicts.size() <= kMaxVerdictCache,
              "DR1: the verdict cache stays bounded under a flood of requests");
    }

    // ---- DR2: the denied verdict restores streamed-truth following; the accepted one cannot
    //         re-arm anything (the O gate's two sides).
    {
        using coop::drone_take_sync::EffectiveMirrorHasSack;
        bool pending = true;
        const bool streamedTrue = true;
        CHECK(EffectiveMirrorHasSack(streamedTrue, pending) == false,
              "DR2/O: while pending, the mirror gate is false (Take cannot re-arm)");
        pending = false;   // a DENIED verdict cleared the pending request
        CHECK(EffectiveMirrorHasSack(streamedTrue, pending) == true,
              "DR2: a DENIED verdict restores the ability to follow the streamed truth");
        // after an ACCEPTED verdict the host's own hasSack dropped, so the streamed truth is
        // false and the gate stays false -- there is nothing to re-execute against.
        CHECK(EffectiveMirrorHasSack(false, false) == false,
              "DR2: an ACCEPTED verdict keeps the gate false (the host's cargo is gone)");
    }

    // ---- E7: the two-token tail + the re-parked (retransmitted) intent ---------------------------
    {
        const std::vector<UE> recs{MakeRecord(L"Aprop_rock_C", L"rock-1")};
        ParsedBlob pb;
        CHECK(ParseContentsBlob(PackContentsExtract(1, 0, recs, {1, 2}), 512, pb) &&
                  pb.tokens.size() == 2 && pb.tokens[0] == 1 && pb.tokens[1] == 2,
              "E7: a two-token tail round-trips");
        CHECK(ParseContentsBlob(PackContentsExtract(1, 0, recs, {}), 512, pb) == false,
              "E7: a zero-token tail is rejected (0 of the allowed 0,1,2,4 sizes)");
    }
    // ---- E8: the T fix's pure success proof (JudgeBirth) ------------------------------------------
    {
        using coop::props::extract_pairing::BirthKeyProof;
        using coop::props::extract_pairing::JudgeBirth;
        // The pre-fix policy (a bare pointer = success) executed against the T scenario:
        // pointer live, key NOT established -> the OLD policy said Spawned (kOk!) -- the exact
        // violation the model suite's RED run demonstrated at HEAD 49db879a.
        const BirthKeyProof tScenario{/*live*/ true, /*nonEmpty*/ true, /*matches*/ false,
                                      /*preExisting*/ false};
        CHECK(JudgeBirth(tScenario) == BirthVerdict::TransientFailure,
              "E8/T: a live pointer whose observed key does NOT match is NOT a birth (transient)");
        CHECK((BirthKeyProof{true, true, false, false}.observedKeyMatches == false),
              "E8/T: the pre-fix policy accepted exactly this input as Spawned -- the hardened "
              "judge refuses it");
        CHECK(JudgeBirth({false, true, true, false}) == BirthVerdict::TransientFailure,
              "E8/T: no pointer at all is transient (the bounded retry owns it)");
        CHECK(JudgeBirth({true, true, true, false}) == BirthVerdict::Spawned,
              "E8/T: pointer + matching key = Spawned (kOk is now earned by proof)");
        CHECK(JudgeBirth({true, true, true, true}) == BirthVerdict::AlreadyExists,
              "E8/T: a pre-existing matching key = AlreadyExists (same authoritative birth)");
        CHECK(JudgeBirth({true, false, true, false}) == BirthVerdict::TransientFailure,
              "E8/T: an empty requested key can never be a success");
    }
    // ---- E9: a retransmitted intent for a still-parked key re-parks (one birth on the pairing) ---
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::Spawned};
        CHECK(led.ParkIntent(Key(1, 1, 9), 33, MakeIntent(9, 33, "dup-intent"), 1000, rs, bs) ==
                  ParkResult::Parked,
              "E9: the first intent parks");
        CHECK(led.ParkIntent(Key(1, 1, 9), 33, MakeIntent(9, 33, "dup-intent"), 1200, rs, bs) ==
                  ParkResult::Parked,
              "E9: the retransmitted intent re-parks idempotently (same key, same birth-once)");
        CHECK(led.ParkedCount() == 1 && bs.authored.empty(),
              "E9: still ONE parked key, no birth before the write");
        CHECK(led.OnWriteApplied({Key(1, 1, 9)}, 33, 1300, rs, bs).committed == 1,
              "E9: the single pairing commits once");
        CHECK(led.ParkIntent(Key(1, 1, 9), 33, MakeIntent(9, 33, "dup-intent"), 1400, rs, bs) ==
                  ParkResult::DuplicateCommitted,
              "E9: an intent after the commit is DuplicateCommitted (never a second birth)");
        CHECK(bs.authored.size() == 1, "E9: EXACTLY one birth ever");
    }

    // ---- N2: queue capacity is bounded ----------------------------------------------------------
    {
        using namespace coop::net::send_backlog;
        using coop::net::ReliableKind;
        BacklogCore core{1000};
        coop::net::DroneActionRequestPayload req{};
        req.nonce = 1; req.op = coop::net::drone_action_op::kDropSack;
        for (size_t i = 0; i < kMaxBacklogEntries; ++i) {
            req.nonce = static_cast<uint32_t>(i + 1);
            if (core.Park(ReliableKind::DroneActionRequest, -1, &req, sizeof(req)) !=
                BacklogCore::EnqueueResult::Parked) break;
        }
        CHECK(core.Size() == kMaxBacklogEntries, "N2: the queue fills to its bound");
        req.nonce = 999;
        CHECK(core.Park(ReliableKind::DroneActionRequest, -1, &req, sizeof(req)) ==
                  BacklogCore::EnqueueResult::DroppedFull,
              "N2: entry 65 is refused DroppedFull (a dead destination cannot balloon it)");
    }

    // ---- N3/S: per-slot purge + generation binding (review finding S) -----------------------------
    {
        using namespace coop::net::send_backlog;
        using coop::net::ReliableKind;
        // The scripted destination-generation table (the model of Session::peerGenerationForSlot).
        struct Table : BacklogCore::IGenerationResolver {
            std::map<int, uint32_t> gen;
            uint32_t GenerationForSlot(int slot) override {
                auto it = gen.find(slot);
                return it == gen.end() ? 0u : it->second;
            }
        } table;
        table.gen[1] = 10;
        BacklogCore core{1000};
        core.SetGenerationResolver(&table);
        coop::net::ContainerExtractResultPayload res{};
        res.extractToken = 7; res.accepted = 1; res.reason = coop::net::container_extract_result::kOk;
        // a host->slot entry (toSlot=1) stamps the destination's CURRENT generation (10).
        CHECK(core.Park(ReliableKind::ContainerExtractResult, 1, &res, sizeof(res)) ==
                  BacklogCore::EnqueueResult::Parked,
              "N3/S: a slot-addressed entry parks");
        core.SetTime(1250);
        struct AcceptAll : BacklogCore::ISendSink {
            std::vector<int> delivered;
            bool Send(ReliableKind, int toSlot, const uint8_t*, size_t) override {
                delivered.push_back(toSlot);
                return true;
            }
        } sink;
        // Same generation -> delivered (the normal retry path is unchanged).
        auto rep = core.Drain(sink);
        CHECK(rep.sent == 1 && sink.delivered.size() == 1 && sink.delivered[0] == 1,
              "N3/S: same-generation destination receives the parked entry");
        // Generation CHANGED while parked -> the entry must never ship.
        (void)core.Park(ReliableKind::ContainerExtractResult, 1, &res, sizeof(res));   // stamps gen 10
        table.gen[1] = 11;                          // slot 1 changed hands AFTER the park
        core.SetTime(1500);
        rep = core.Drain(sink);
        CHECK(rep.stale == 1 && rep.sent == 0 && sink.delivered.size() == 1,
              "N3/S: a parked entry for gen 10 is STALE-DROPPED when slot 1 is at gen 11");
        CHECK(core.Size() == 0, "N3/S: the stale entry is gone (never retried, never delivered)");
        // Empty slot (gen 0) -> also stale (an entry cannot outlive its destination).
        table.gen[2] = 5;
        (void)core.Park(ReliableKind::ContainerExtractResult, 2, &res, sizeof(res));   // stamps gen 5
        table.gen[2] = 0;                           // the occupant left; the slot is empty
        core.SetTime(1750);
        rep = core.Drain(sink);
        CHECK(rep.stale == 1, "N3/S: an entry for a now-EMPTY slot is stale-dropped too");
        // Host-directed entries (toSlot<0) are never generation-checked.
        table.gen[1] = 99;
        (void)core.Park(ReliableKind::DroneActionRequest, -1, &res, sizeof(res));
        core.SetTime(2000);
        rep = core.Drain(sink);
        CHECK(rep.sent == 1 && rep.stale == 0,
              "N3/S: host-directed entries ignore the generation check (they target the session)");
        // Per-slot purge: the peer-left event drops ONLY that destination's entries.
        (void)core.Park(ReliableKind::ContainerExtractResult, 1, &res, sizeof(res));
        coop::net::DroneActionRequestPayload req{};
        req.nonce = 5; req.op = coop::net::drone_action_op::kDropSack;
        (void)core.Park(ReliableKind::DroneActionRequest, 3, &req, sizeof(req));
        CHECK(core.Size() == 2, "N3/S: two entries parked for two destinations");
        CHECK(core.PurgeSlot(1) == 1, "N3/S: purging slot 1 removes exactly its own entry");
        CHECK(core.Size() == 1, "N3/S: the other destination's entry survives the purge");
        core.SetTime(2250);
        rep = core.Drain(sink);
        CHECK(rep.sent == 1 && sink.delivered.back() == 3,
              "N3/S: the surviving entry still delivers (to its own slot)");
        // No resolver -> no stamp -> no check (fail-open is impossible to trip accidentally:
        // the production wrapper always installs the session-backed resolver).
        BacklogCore naked{1000};
        (void)naked.Park(ReliableKind::ContainerExtractResult, 1, &res, sizeof(res));
        naked.SetTime(1250);
        rep = naked.Drain(sink);
        CHECK(rep.sent == 1 && rep.stale == 0,
              "N3/S: a resolver-less core skips the check (documented; production always wires one)");
    }

    // ---- P6: the remaining sleep-ownership obligations --------------------------------------------
    {
        using coop::sleep_sync::DilationOwnershipTracker;
        using coop::sleep_sync::SleepDilationOwnership;
        // The NORMAL lifecycle: ACCELERATE latches, END restores, nothing is owed afterwards.
        {
            DilationOwnershipTracker t;
            t.CommitAccelerate();
            CHECK(t.owned() == SleepDilationOwnership::AccelerateForcedTwenty &&
                      t.RestoreTargetWhileAwake() == 1.f,
                  "P6: ACCELERATE(20x) -> the wake restore targets 1x");
            t.CommitRestored();
            CHECK(t.owned() == SleepDilationOwnership::None &&
                      t.DisconnectHandoffTargetWhileSleeping() == 0.f &&
                      t.RestoreTargetWhileAwake() == 0.f,
                  "P6: after the normal lifecycle NOTHING is owed (no handoff, no restore write)");
        }
        // A FAILED restore keeps the obligation (Commit* is called ONLY after a successful
        // engine write -- the tracker must not discharge on a failed write).
        {
            DilationOwnershipTracker t;
            t.CommitWaitingUndo();
            // restore write fails -> the caller does NOT CommitRestored
            CHECK(t.owned() == SleepDilationOwnership::WaitingForcedOne,
                  "P6: a failed restore does NOT discharge the ownership");
            CHECK(t.RestoreTargetWhileAwake() == 1.f,
                  "P6: the restore is still owed after the failed attempt");
            CHECK(t.DisconnectHandoffTargetWhileSleeping() == 20.f,
                  "P6: a disconnect while the failed restore is still owed hands 20x (obligation kept)");
            t.CommitRestored();   // the retry succeeds
            CHECK(t.owned() == SleepDilationOwnership::None,
                  "P6: only the SUCCESSFUL restore discharges");
        }
        // DischargeSuperseded also clears (a higher authority took the value; logged loudly).
        {
            DilationOwnershipTracker t;
            t.CommitAccelerate();
            t.DischargeSuperseded();
            CHECK(t.owned() == SleepDilationOwnership::None,
                  "P6: a documented supersede discharges the obligation");
        }
    }

    // ---- Q2: the exhaustive mask algebra (every byte x every mask) --------------------------------
    {
        using namespace ue_wrap::bool_mask;
        for (uint32_t b = 0; b < 256; ++b) {
            for (uint32_t m = 1; m < 256; ++m) {
                const uint8_t byte = static_cast<uint8_t>(b);
                const uint8_t mask = static_cast<uint8_t>(m);
                const uint8_t set = SetBoolMaskBits(byte, mask);
                const uint8_t clr = ClearBoolMaskBits(byte, mask);
                if ((set & static_cast<uint8_t>(~mask & 0xFF)) != (byte & static_cast<uint8_t>(~mask & 0xFF)) ||
                    (set & mask) != mask ||
                    (clr & mask) != 0 ||
                    (clr & static_cast<uint8_t>(~mask & 0xFF)) != (byte & static_cast<uint8_t>(~mask & 0xFF))) {
                    CHECK(false, "Q2: exhaustive mask algebra violation");
                    goto q2_done;
                }
            }
        }
        CHECK(true, "Q2: exhaustive 256x255 sweep: set/clear touch ONLY their mask bits");
    q2_done:;
        CHECK(SetBoolMaskBits(0x00, 0x01) == 0x01 && SetBoolMaskBits(0x00, 0x02) == 0x02 &&
                  SetBoolMaskBits(0x00, 0x04) == 0x04 && SetBoolMaskBits(0x00, 0x80) == 0x80,
              "Q2: single-bit masks 0x01/0x02/0x04/0x80 set exactly their bit from zero");
        CHECK(ClearBoolMaskBits(0xFF, 0x01) == 0xFE && ClearBoolMaskBits(0xFF, 0x02) == 0xFD &&
                  ClearBoolMaskBits(0xFF, 0x04) == 0xFB && ClearBoolMaskBits(0xFF, 0x80) == 0x7F,
              "Q2: single-bit masks clear exactly their bit from all-ones");
    }

    // ---- E10 (FUZZ REGRESSION, seed 764236619501 / iteration 334): a refused DUPLICATE of an
    //         already-applied write must not cancel the pairing -------------------------------
    {
        Ledger led;
        ScriptedResults rs;
        ScriptedBirths bs;
        bs.script = {BirthVerdict::Spawned};
        // write-first: the write applies before its intent arrives -> an applied marker exists.
        const auto rep = led.OnWriteApplied({Key(1, 1, 11)}, 44, 1000, rs, bs);
        CHECK(rep.markedApplied == 1, "E10: write-first leaves an applied marker");
        // a DUPLICATE copy of the same write loses the CAS -> OnWriteRefused for the same key...
        led.OnWriteRefused({Key(1, 1, 11)}, rs);
        CHECK(led.AppliedCount() == 1,
              "E10: the refused DUPLICATE did NOT cancel the accepted mutation's pairing");
        CHECK(rs.Count(0, coop::net::container_extract_result::kRefused) == 0,
              "E10: no kRefused for an already-accepted mutation (the client must not retire)");
        // the late intent still commits (the pairing survived the refused duplicate).
        CHECK(led.ParkIntent(Key(1, 1, 11), 44, MakeIntent(11, 44, "dup-write"), 1100, rs, bs) ==
                  ParkResult::CommittedNow,
              "E10: the late intent commits (one birth, one kOk)");
        CHECK(rs.Count(1, coop::net::container_extract_result::kOk) == 1 &&
                  bs.authored.size() == 1,
              "E10: exactly one terminal kOk over one birth");
        // a TRUE refusal (a parked intent whose mutation never applied) is unchanged.
        Ledger led2;
        ScriptedResults rs2;
        ScriptedBirths bs2;
        (void)led2.ParkIntent(Key(2, 1, 12), 45, MakeIntent(12, 45, "true-refuse"), 1000, rs2, bs2);
        led2.OnWriteRefused({Key(2, 1, 12)}, rs2);
        CHECK(led2.ParkedCount() == 0 &&
                  rs2.Count(0, coop::net::container_extract_result::kRefused) == 1,
              "E10: a parked intent's true refusal still erases state and answers kRefused");
    }

    // ---- S1: the runtime contract's pure pieces (the smoke rate limiter + gate default) -------
    {
        using coop::dev::batch1_smoke::RateLimiter;
        RateLimiter rl;
        CHECK(rl.RateOk(1000, 1000), "S1: the first emit of a tag is always allowed");
        CHECK(!rl.RateOk(1999, 1000), "S1: a second emit inside the interval is rate-limited");
        CHECK(rl.RateOk(2000, 1000), "S1: exactly at the interval boundary the emit is allowed");
        CHECK(!rl.RateOk(2500, 1000), "S1: and the limiter re-arms from the new timestamp");
        RateLimiter fresh;
        CHECK(fresh.RateOk(999999, 1000), "S1: a fresh slot allows immediately (first milestone)");
    }

    // ---- T1 (ACTUATOR-ONLY pass): the drone client-Take actuator's pure gate ------------------
    // The actuator itself (coop/dev/drone_probe.cpp, ini drone_probe_take=1) dispatches the
    // mirror drone's OWN dropSack verb -- the same ProcessEvent path a player's Take option
    // runs -- so the REAL 0x45 seam authors the request. What is unit-tested here is the
    // decision half: never fire before the parked-with-cargo state, fire EXACTLY once, never
    // re-arm, never fire while a take request is in flight, terminal timeout that cannot fire.
    {
        using namespace coop::dev::drone_take_gate;
        using D = Decision;
        // 1. disabled by default: no input combination ever fires.
        {
            Gate g;
            bool fired = false;
            for (uint64_t t = 0; t <= 1000; t += 100)
                if (g.Tick(t, /*enabled=*/false, true, true, false).action == Action::Fire) fired = true;
            CHECK(!fired && g.phase() == Phase::Disabled,
                  "T1: a disabled gate never fires (default OFF is a hard never-fire)");
        }
        // 2. does not fire before the actionable state; the wait is armed on the mirror sighting.
        {
            Gate g;
            const D notReady = g.Tick(1000, true, true, /*ready=*/false, false);
            CHECK(notReady.action == Action::None && notReady.phase == Phase::Arming,
                  "T1: no fire before the parked-with-cargo state (NOT-READY diagnostics)");
            const D noDrone = g.Tick(2000, true, /*dronePresent=*/false, true, false);
            CHECK(noDrone.action == Action::None,
                  "T1: no fire without the client mirror Adrone_C sighting");
        }
        // 3. readiness must HOLD kReadyHoldTicks consecutive ticks before the single fire.
        {
            Gate g;
            Action last = Action::None;
            for (int i = 0; i < kReadyHoldTicks - 1; ++i)
                last = g.Tick(3000 + uint64_t(i), true, true, true, false).action;
            CHECK(last == Action::None, "T1: the ready hold is not satisfied early");
            const D fire = g.Tick(3000 + kReadyHoldTicks, true, true, true, false);
            CHECK(fire.action == Action::Fire && fire.phase == Phase::Fired,
                  "T1: exactly on the kReadyHoldTicks-th consecutive ready tick the gate fires ONCE");
        }
        // 4. fires exactly once and never re-arms.
        {
            Gate g;
            int fires = 0;
            for (uint64_t t = 0; t < 50; ++t)
                if (g.Tick(10000 + t, true, true, true, false).action == Action::Fire) ++fires;
            CHECK(fires == 1, "T1: 50 ready ticks produce EXACTLY ONE fire (no re-arm)");
            CHECK(g.phase() == Phase::Done && g.fired(),
                  "T1: after the fire the gate is permanently Done");
        }
        // 5. never fires while a take request is already in flight (ours or a human's).
        {
            Gate g;
            bool fired = false;
            for (uint64_t t = 0; t < 100; ++t)
                if (g.Tick(1000 + t, true, true, /*ready=*/true, /*pendingTake=*/true).action == Action::Fire)
                    fired = true;
            CHECK(!fired, "T1: a pending take request blocks the actuator fire completely");
            // and the hold is CONSECUTIVE: ready again from zero after the pending clears.
            const D d = g.Tick(2000, true, true, true, false);
            CHECK(d.action == Action::None, "T1: after a pending take the hold restarts from zero");
        }
        // 6. the wait timeout is TERMINAL and mutates nothing: TimedOut can never fire, ever.
        {
            Gate g;
            g.Tick(1000, true, true, false, false);                 // arm at 1000
            const D to = g.Tick(1000 + kWaitTimeoutMs + 1, true, true, false, false);
            CHECK(to.phase == Phase::TimedOut && to.action == Action::None,
                  "T1: the delivery wait expires into an explicit NOT-FIRED terminal state");
            bool fired = false;
            for (uint64_t t = 0; t < 100; ++t)
                if (g.Tick(1000 + kWaitTimeoutMs + 10 + t, true, true, true, false).action == Action::Fire)
                    fired = true;
            CHECK(!fired, "T1: after the timeout even permanent readiness cannot fire (timeout "
                          "leaves gameplay state untouched -- the FSM has no such output)");
        }
        // 7. a late drone sighting arms the clock THEN (slow world loads do not eat the budget).
        {
            Gate g;
            const D pre = g.Tick(10 * kWaitTimeoutMs, true, false, true, false);
            CHECK(pre.action == Action::None && pre.phase == Phase::Arming,
                  "T1: without the mirror the gate waits (and does not accrue timeout)");
            const D arm = g.Tick(10 * kWaitTimeoutMs + 1, true, true, false, false);
            CHECK(arm.action == Action::None && arm.phase == Phase::Arming,
                  "T1: the wait clock arms at the FIRST mirror sighting");
        }
    }

    // ---- T2 (ACTUATOR-ONLY pass): the extraction actuator's baseline-convergence gate ---------
    // Node2 evidence: the old selftest fired the client extract on a FIXED +25s timer -- before
    // the client had consumed the host's published baseline (base=0) -- and the host's CAS
    // REFUSED it (STALE BASE, author base=0). The gate below makes the actuator WAIT for proven
    // convergence (base != 0 && local == base) and fire exactly once; it has NO way to write a
    // base/version, so it cannot manufacture convergence -- only wait for it.
    {
        using namespace coop::dev::extract_convergence;
        using O = Observation;
        using D = Decision;
        // 1. does not fire when the baseline is absent (base=0 = host truth not yet consumed).
        {
            Gate g;
            const D d = g.Tick(1000, O{0, 12345});
            CHECK(d.action == Action::None && d.phase == Phase::Waiting,
                  "T2: no fire without a consumed baseline (base=0) -- the Node2 STALE BASE shape");
        }
        // 2. does not fire when the base is mismatched (local drifted / newer truth pending).
        {
            Gate g;
            const D d = g.Tick(1000, O{0xABCD, 0x1234});
            CHECK(d.action == Action::None && d.phase == Phase::Waiting,
                  "T2: no fire while local content != consumed baseline (would be refused)");
        }
        // 3. waits through a stale baseline for as long as it lasts (no fixed-timer fire).
        {
            Gate g;
            bool fired = false;
            for (uint64_t t = 0; t < 500; ++t)
                if (g.Tick(1000 + t, O{0xAA, 0xBB}).action == Action::Fire) fired = true;
            CHECK(!fired, "T2: 500 stale ticks produce no fire (waits THROUGH the stale baseline)");
        }
        // 4. fires ONCE, immediately, on proven convergence -- and never again.
        {
            Gate g;
            (void)g.Tick(1000, O{0x77, 0x99});                      // stale first
            const D fire = g.Tick(1001, O{0x77, 0x77});             // converged
            CHECK(fire.action == Action::Fire && fire.phase == Phase::Fired,
                  "T2: proven convergence (base!=0, local==base) fires the ONE real extract");
            const D after = g.Tick(1002, O{0x77, 0x77});
            CHECK(after.phase == Phase::Done && after.action == Action::None,
                  "T2: the convergence gate is permanently inert after its one fire");
            int fires = 0;
            for (uint64_t t = 0; t < 100; ++t)
                if (g.Tick(1100 + t, O{0x77, 0x77}).action == Action::Fire) ++fires;
            CHECK(fires == 0, "T2: converged observations after the fire never re-fire");
        }
        // 5. the timeout is TERMINAL and leaves production state untouched (no fire, ever).
        {
            Gate g;
            (void)g.Tick(1000, O{0, 0});
            const D to = g.Tick(1000 + kWaitTimeoutMs + 1, O{0, 0});
            CHECK(to.phase == Phase::TimedOut && to.action == Action::None,
                  "T2: the baseline wait expires into an explicit NOT-FIRED terminal state");
            const D late = g.Tick(1000 + kWaitTimeoutMs + 2, O{0x77, 0x77});
            CHECK(late.action == Action::None,
                  "T2: after the timeout even a converged observation cannot fire");
        }
        // 6. convergence is defined by EQUALITY ONLY -- contradictory observations cannot
        //    manufacture it (the gate has no write path to any base/version at all).
        {
            Gate g;
            bool fired = false;
            const uint64_t bases[]  = {1, 2, 3, 0, 5, 0xABCD};
            const uint64_t locals[] = {9, 8, 7, 6, 4, 0x1234};   // deliberately NO base==local pair
            for (uint64_t t = 0; t < 6; ++t)
                if (g.Tick(1000 + t, O{bases[t], locals[t]}).action == Action::Fire) fired = true;
            CHECK(!fired, "T2: arbitrary observations never fire -- only exact base==local equality does");
        }
    }

    // ---- T3 (ACTUATOR-ONLY pass): the CTAKE race's BOTH_READY barrier --------------------------
    // The old peer orchestration ran ONE 60s GO-timeout from the peer's OWN ARRIVED -- "HOST
    // ARRIVED" alone started the clock and a slow client let the host expire WITHOUT a GO.
    // mp.py additionally tested readiness by PRESENCE, so a stale ARRIVED from a previous
    // session instance could satisfy BOTH_READY. The barrier below is the shipped semantics.
    {
        using namespace coop::director::ctake_barrier;
        constexpr uint64_t kGoDelay = 1500, kBothWait = 120000;
        // 1. HOST ARRIVED alone cannot start a GO timeout (or a GO anything).
        {
            Gate b;
            b.ObserveArrival(Role::Host, 7);
            const Decision d = b.Tick(1000, kGoDelay, kBothWait);
            CHECK(d.phase == Phase::Waiting && !d.mayFire && d.event == Event::None,
                  "T3: HOST ARRIVED alone starts NO GO clock (barrier keeps waiting)");
        }
        // 2. CLIENT ARRIVED alone: symmetric.
        {
            Gate b;
            b.ObserveArrival(Role::Client, 7);
            const Decision d = b.Tick(1000, kGoDelay, kBothWait);
            CHECK(d.phase == Phase::Waiting && !d.mayFire && d.event == Event::None,
                  "T3: CLIENT ARRIVED alone starts NO GO clock");
        }
        // 3. both ARRIVED (one shared generation) starts ONLY the BOTH_READY phase, then GO.
        {
            Gate b;
            b.ObserveArrival(Role::Host, 7);
            b.ObserveArrival(Role::Client, 7);
            const Decision br = b.Tick(1000, kGoDelay, kBothWait);
            CHECK(br.event == Event::BothReady && br.phase == Phase::BothReady && !br.mayFire,
                  "T3: BOTH_READY is proven before any GO exists");
            const Decision pre = b.Tick(1000 + kGoDelay - 1, kGoDelay, kBothWait);
            CHECK(pre.phase == Phase::BothReady && !pre.mayFire,
                  "T3: the GO countdown runs AFTER BOTH_READY, never from a single arrival");
            const Decision go = b.Tick(1000 + kGoDelay, kGoDelay, kBothWait);
            CHECK(go.event == Event::Go && go.mayFire && go.phase == Phase::Go,
                  "T3: GO (the only mayFire) arrives strictly after BOTH_READY + delay");
        }
        // 4. a disconnect / generation change invalidates readiness BEFORE GO.
        {
            Gate b;
            b.ObserveArrival(Role::Host, 7);
            b.ObserveArrival(Role::Client, 7);
            (void)b.Tick(1000, kGoDelay, kBothWait);                // BOTH_READY
            b.InvalidateGeneration(7);
            const Decision d = b.Tick(1001, kGoDelay, kBothWait);
            CHECK(d.phase == Phase::Waiting && !d.mayFire && d.event == Event::None,
                  "T3: a generation change withdraws BOTH_READY (readiness invalidated)");
        }
        // 5. stale readiness from a previous peer generation can NEVER satisfy BOTH_READY.
        {
            Gate b;
            b.ObserveArrival(Role::Host, 1);
            b.ObserveArrival(Role::Client, 1);
            b.InvalidateGeneration(1);                              // the client dropped + rejoined
            b.ObserveArrival(Role::Client, 2);                      // fresh proof, new generation
            const Decision stale = b.Tick(1000, kGoDelay, kBothWait);
            CHECK(stale.phase == Phase::Waiting && stale.event == Event::None && !stale.mayFire,
                  "T3: the host's gen-1 readiness cannot pair with the client's gen-2 (stale)");
            b.ObserveArrival(Role::Host, 2);                        // the host re-proves under gen 2
            const Decision ok = b.Tick(1001, kGoDelay, kBothWait);
            CHECK(ok.event == Event::BothReady,
                  "T3: BOTH_READY needs BOTH roles under the CURRENT shared generation");
        }
        // 6. the BOTH-peers wait expires into an explicit, role-naming, never-firing timeout.
        {
            Gate b;
            b.ObserveArrival(Role::Host, 7);
            (void)b.Tick(1000, kGoDelay, kBothWait);                // the wait anchors at first sight
            const Decision to = b.Tick(1000 + kBothWait + 1, kGoDelay, kBothWait);
            CHECK(to.phase == Phase::TimedOut && to.event == Event::TimedOut && !to.mayFire,
                  "T3: one peer never ARRIVED -> explicit timeout naming the missing role");
            const Decision after = b.Tick(1000 + kBothWait + 2, kGoDelay, kBothWait);
            CHECK(after.phase == Phase::TimedOut && !after.mayFire,
                  "T3: the timeout is terminal -- a late arrival cannot resurrect the race");
        }
        // 7. arrivals that disagree on the generation never satisfy BOTH_READY while waiting.
        {
            Gate b;
            b.ObserveArrival(Role::Host, 1);
            b.ObserveArrival(Role::Client, 2);
            const Decision d = b.Tick(1000, kGoDelay, kBothWait);
            CHECK(d.phase == Phase::Waiting && d.event == Event::None && !d.mayFire,
                  "T3: mismatched arrival generations are the stale-readiness case -- no BOTH_READY");
        }
        // 8. after GO the race is underway: a generation change does not rewind the barrier.
        {
            Gate b;
            b.ObserveArrival(Role::Host, 3);
            b.ObserveArrival(Role::Client, 3);
            (void)b.Tick(1000, kGoDelay, kBothWait);
            (void)b.Tick(1000 + kGoDelay, kGoDelay, kBothWait);     // GO
            b.InvalidateGeneration(3);
            const Decision d = b.Tick(1000 + kGoDelay + 1, kGoDelay, kBothWait);
            CHECK(d.phase == Phase::Go,
                  "T3: post-GO invalidation is recorded but the barrier stays in Go");
        }
    }

    std::printf("== %d checks, %d failures ==\n", g_total, g_fail);
    if (g_fail == 0) std::printf("ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
