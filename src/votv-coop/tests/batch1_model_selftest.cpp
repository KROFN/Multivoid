// tests/batch1_model_selftest.cpp -- KROFNE FORK (2133), Batch-1 TEST CONTRACT layer 2.
//
// SYSTEM-BEHAVIOR tests: pure fakes (transport / host / clients / drone / container / world)
// composed around the REAL pure cores (see batch1_model_harness.h), asserting the fork's
// INVARIANTS after every event:
//
//   D1  one valid client Take can cause AT MOST ONE host native dropSack
//   D2  while a Take is pending, the local mirror cannot expose another Take
//   D3  a denied request creates ZERO authoritative sacks
//   D4  an accepted request creates EXACTLY ONE authoritative sack
//   D5  duplicate/reordered request/result packets cannot increase the authoritative sack count
//   D6  a result for an old slot generation can never affect a new occupant
//   E   for one logical item identity, authoritativeCopies == 1 at every terminal state
//       (the ONE documented exception: the bounded birth-retry window lapsing -- surfaced
//        EXPECTED-FAIL below, never reported as lossless)
//   T   a kOk verdict may only leave the host over a PROVEN key (the S review finding)
//   S   parked critical traffic never reaches a slot's successor generation (the S review
//       finding: the backlog was the one queue that survived a disconnect)
//
// Build (Linux, no engine):
//   g++ -std=c++20 -I include tests/batch1_model_selftest.cpp \
//       src/coop/items/save_record_wire.cpp src/coop/interactables/signal_wire.cpp \
//       -o batch1_model_selftest
// Exit 0 = all invariants held (expected-fails surfaced separately).

#include "batch1_model_harness.h"

#include <cstdio>
#include <string>

static int g_pass = 0, g_fail = 0, g_expectedFails = 0, g_unexpectedGreens = 0;
static std::vector<std::string> g_expectedFailLog, g_unexpectedGreenLog;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (cond) { ++g_pass; }                                               \
        else {                                                                \
            ++g_fail;                                                         \
            std::printf("FAIL: %s\n", what);                                  \
        }                                                                     \
    } while (0)

// A test that is EXPECTED to violate the invariant under the CURRENT policy (surfaced, not
// hidden -- the suite stays green only while the violation is the documented one).
#define EXPECTED_FAIL(cond, label)                                            \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_unexpectedGreens;                                             \
            g_unexpectedGreenLog.push_back(label);                            \
            std::printf("NOTE: expected-fail case now PASSES -- retire the label: %s\n", label); \
        } else {                                                              \
            ++g_expectedFails;                                                \
            g_expectedFailLog.push_back(label);                               \
            std::printf("EXPECTED-FAIL (surfaced policy violation): %s\n", label); \
        }                                                                     \
    } while (0)

// the always-on invariant sweep
static bool Sweep(b1model::Model& m) {
    const auto rep = b1model::CheckSystemInvariants(m);
    if (!rep.ok)
        std::printf("INVARIANT VIOLATION [%s]: %s\n", rep.code, rep.what.c_str());
    return rep.ok;
}

using b1model::ExtractKey;
using b1model::kModelSlots;

// ================================================================================================
int main() {
    std::printf("== Batch-1 model selftest (fakes + REAL pure cores; invariants after every event) ==\n");

    // ============================================================================================
    // SECTION A -- DRONE invariants (D1-D6)
    // ============================================================================================

    // A1: the happy take -- exactly one native dropSack, one sack, one verdict.
    {
        b1model::Model m;
        m.Connect(1, 10);
        CHECK(Sweep(m), "A1: invariants at rest");
        CHECK(m.clients[1].PressTake(), "A1: the take press is presented (mirror hasSack true)");
        m.DrainWire();
        CHECK(Sweep(m), "A1: invariants after the take");
        CHECK(m.host.dropSackExecutions == 1 && m.host.sacksSpawned == 1 && m.host.acceptedRequests == 1,
              "A1/D1/D4: exactly ONE native dropSack body ran, ONE authoritative sack exists");
        CHECK(m.clients[1].pendingTake == false && m.clients[1].localPhantoms == 0,
              "A1: the verdict closed the pending and retired the exact phantom");
        // D5: a duplicate/reordered request cannot add a sack (resend the SAME nonce).
        coop::net::DroneActionRequestPayload dup{};
        dup.nonce = 1; dup.op = coop::net::drone_action_op::kDropSack;
        (void)m.transport.Enqueue(1, -1, static_cast<uint8_t>(coop::net::ReliableKind::DroneActionRequest),
                                  &dup, sizeof(dup));
        (void)m.transport.Enqueue(1, -1, static_cast<uint8_t>(coop::net::ReliableKind::DroneActionRequest),
                                  &dup, sizeof(dup));
        m.DrainWire();
        CHECK(m.host.dropSackExecutions == 1 && m.host.sacksSpawned == 1,
              "A1/D5: duplicates answered from the verdict cache, sack count unchanged");
    }

    // A2: reordered duplicate -- the duplicate overtakes fresh traffic, still one execution.
    {
        b1model::Model m;
        m.Connect(1, 3);
        m.transport.delayNextEnqueues = 1;
        m.transport.dupNextEnqueues = 1;
        CHECK(m.clients[1].PressTake(), "A2: take pressed");
        m.DrainWire();
        CHECK(m.host.dropSackExecutions == 1 && m.host.sacksSpawned == 1,
              "A2/D5: reordered + duplicated request -> still exactly one execution");
        CHECK(Sweep(m), "A2: invariants hold");
    }

    // A3: ONE-send refusal parks the request (same nonce rides the retry); the pending take
    // masks the mirror meanwhile (D2) -- a second press is impossible.
    {
        b1model::Model m;
        m.Connect(1, 4);
        m.transport.refuseNextEnqueues = 1;              // the request enqueue is refused once
        CHECK(m.clients[1].PressTake(), "A3: take pressed (request parked on refusal)");
        CHECK(m.clients[1].outbox.Size() == 1, "A3: the exact request bytes parked in the outbox");
        CHECK(!m.clients[1].PressTake(),
              "A3/D2/O: while pending, the mirror gate hides hasSack -- the Take option cannot re-arm");
        CHECK(m.clients[1].localPhantoms == 1,
              "A3/D2: still exactly ONE local phantom (no second native body)");
        m.DrainWire();
        CHECK(m.host.dropSackExecutions == 1 && m.host.sacksSpawned == 1,
              "A3: the parked request delivered on retry -> one execution");
        CHECK(m.clients[1].pendingTake == false && m.clients[1].localPhantoms == 0,
              "A3: verdict closes the loop");
        CHECK(Sweep(m), "A3: invariants hold");
    }

    // A4: FIVE-send refusals -- the request survives (bounded queue, FIFO, same bytes).
    {
        b1model::Model m;
        m.Connect(1, 5);
        m.transport.refuseNextEnqueues = 5;
        CHECK(m.clients[1].PressTake(), "A4: take pressed");
        const bool delivered = m.AdvanceUntil(
            [&] { return m.host.acceptedRequests == 1; }, 15000);
        CHECK(delivered, "A4: after the transport recovers, the parked request ships");
        CHECK(m.host.dropSackExecutions == 1 && m.host.sacksSpawned == 1,
              "A4/D5: five refusals still exactly one execution (no re-request was ever minted)");
        CHECK(Sweep(m), "A4: invariants hold");
    }

    // A5: the DENIED take -- zero authoritative sacks; the verdict restores streamed truth.
    {
        b1model::Model m;
        m.Connect(1, 6, /*droneHasSack=*/false);
        CHECK(m.clients[1].streamedHasSack == false, "A5: the streamed truth says no cargo");
        CHECK(m.clients[1].MirrorHasSack() == false, "A5: the Take option is not presented");
        m.host.drone.hasSack = false;                   // (defensive: the deny must not depend on the mirror)
        CHECK(!m.clients[1].PressTake(), "A5: no take can be pressed with the gate false");
        // A stale request from a previous state must still be DENIED cleanly (D3).
        m.host.drone.canTakeOff = true; m.host.drone.hasSack = false;
        coop::net::DroneActionRequestPayload req{};
        req.nonce = 1; req.op = coop::net::drone_action_op::kDropSack;
        (void)m.transport.Enqueue(1, -1, static_cast<uint8_t>(coop::net::ReliableKind::DroneActionRequest),
                                  &req, sizeof(req));
        m.DrainWire();
        CHECK(m.host.deniedRequests == 1, "A5/D3: the host denied the request");
        CHECK(m.host.sacksSpawned == 0 && m.host.dropSackExecutions == 0,
              "A5/D3: a denied request creates ZERO authoritative sacks and ZERO native bodies");
        m.host.drone.hasSack = true;                    // the host regains cargo
        m.StreamDroneState(1);
        CHECK(m.clients[1].MirrorHasSack(),
              "A5: a denied verdict restores the ability to follow the streamed truth (gate re-arms)");
        CHECK(Sweep(m), "A5: invariants hold");
    }

    // A6/S1: disconnect before the result -- the parked verdict must NEVER reach the slot's
    // successor (THE S FINDING; the backlog is the one queue that survives a disconnect).
    {
        b1model::Model m;
        m.Connect(1, 10);
        CHECK(m.clients[1].PressTake(), "A6: take pressed");
        m.transport.refuseNextEnqueues = 99;            // refuse the RESULT enqueue AND its retries
        m.DrainWire();                                   // request delivered; host executed; result PARKED
        m.transport.refuseNextEnqueues = 0;
        CHECK(m.host.acceptedRequests == 1, "A6: the host executed the take");
        CHECK(m.host.outbox.Size() == 1, "A6: the verdict is parked in the host backlog");
        // The occupant leaves; a NEW peer takes the slot with a NEW generation.
        m.host.DisconnectSlot(1);
        m.Connect(1, 11);                                // same slot, new occupant (gen 11)
        const int before = m.clients[1].resultsReceived;
        m.DrainWire();
        CHECK(m.clients[1].resultsReceived == before,
              "A6/S/D6: a parked verdict for the OLD occupant must not be delivered to the successor");
        CHECK(b1model::CheckFreshOccupant(m, 1).ok, "A6/D6: the successor starts pristine");
        CHECK(Sweep(m), "A6: invariants hold");
    }

    // ============================================================================================
    // SECTION B -- EXTRACTION invariants (the authoritative-copies contract)
    // ============================================================================================

    // B1: intent-first (the common order) -- one birth, one kOk, terminal copies == 1.
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-1")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.clients[1].DoExtract(L"rock-1");
        m.itemKey[L"rock-1"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.DrainWire();
        CHECK(Sweep(m), "B1: invariants during pairing");
        m.AdvanceUntil([&] { return !m.host.ledger.IsPending(ExtractKey{1, 10, m.clients[1].nextToken}); },
                       5000);
        m.DrainWire();
        CHECK(Sweep(m), "B1: invariants at terminal");
        int loss = 0;
        const auto rep = b1model::CheckCopiesTerminal(m, L"rock-1", &loss);
        CHECK(rep.ok && !loss, "B1/E: terminal authoritativeCopies == 1 (the item left the container, the world birth exists)");
        CHECK(m.host.world->KeyProven(L"rock-1"), "B1: the authoritative birth carries the requested key");
        CHECK(m.clients[1].ghosts.empty(), "B1: the client ghost reached a terminal state (no leak)");
        CHECK(m.host.kOkProof.size() == 1, "B1/M: exactly one kOk terminal verdict");
    }

    // B2: contents-first (the write overtakes the intent) -- the applied marker commits the
    // late intent on arrival.
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-2")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.transport.delayNextEnqueues = 1;               // the INTENT is delayed behind the write
        m.clients[1].DoExtract(L"rock-2");
        m.itemKey[L"rock-2"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.DrainWire();
        m.AdvanceUntil([&] { return !m.host.ledger.IsPending(ExtractKey{1, 10, m.clients[1].nextToken}); },
                       5000);
        m.DrainWire();
        CHECK(Sweep(m), "B2: invariants hold");
        int loss = 0;
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-2", &loss).ok && !loss,
              "B2/E: contents-first also reaches terminal copies == 1");
    }

    // B3/B4: duplicate intents + duplicate writes -- still one birth.
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-3")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.transport.dupNextEnqueues = 2;                 // duplicate the intent AND the write
        m.clients[1].DoExtract(L"rock-3");
        m.itemKey[L"rock-3"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.DrainWire();
        m.AdvanceUntil([&] { return !m.host.ledger.IsPending(ExtractKey{1, 10, m.clients[1].nextToken}); },
                       5000);
        m.DrainWire();
        CHECK(m.host.world->KeyProvenCount(L"rock-3") == 1, "B3/B4: EXACTLY one authoritative birth");
        CHECK(m.host.kOkProof.size() == 1, "B3/B4: exactly one kOk (the duplicate never re-births)");
        int loss = 0;
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-3", &loss).ok && !loss,
              "B3/B4/E: terminal copies == 1");
    }

    // B5: the mutation is REFUSED (stale baseHash) -- no birth, the item stays in the container.
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-4")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.transport.delayNextEnqueues = 1;               // the intent is delayed
        m.clients[1].DoExtract(L"rock-4");
        m.itemKey[L"rock-4"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        // The container changes underneath the in-flight write (another peer's write won the CAS era).
        m.host.containerItems.push_back(b1model::MakeRecord(L"Aprop_box_C", L"someone-elses"));
        m.DrainWire();
        // the refused write's intent still parks -> it expires through the pairing window
        m.AdvanceUntil([&] {
            return m.clients[1].ghosts.empty() &&
                   !m.host.ledger.IsPending(ExtractKey{1, 10, m.clients[1].nextToken});
        }, 20000);
        m.DrainWire();
        CHECK(m.host.world->KeyProven(L"rock-4") == false, "B5: a refused mutation spawns NOTHING");
        int loss = 0;
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-4", &loss).ok && !loss,
              "B5/E: the item stays exactly in the container (copies == 1)");
        CHECK(Sweep(m), "B5: invariants hold");
    }

    // B6: transient birth failures then success -- the bounded retry lands the birth (M4).
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.world.spawnFails = 2;                          // the first two author attempts fail outright
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-5")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.clients[1].DoExtract(L"rock-5");
        m.itemKey[L"rock-5"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.DrainWire();
        const bool landed = m.AdvanceUntil(
            [&] { return m.host.world->KeyProven(L"rock-5"); }, 15000);
        CHECK(landed, "B6: the bounded birth retry lands the authoritative birth");
        m.DrainWire();
        CHECK(m.host.kOkProof.size() == 1, "B6/M: one kOk AFTER the birth exists (never before)");
        int loss = 0;
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-5", &loss).ok && !loss,
              "B6/E: terminal copies == 1 (the interim unbacked window was bounded and recovered)");
    }

    // B7: the write is LOST on the stock lane -- the pairing window expires, the ghost retires,
    // the item never left the container.
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-6")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.clients[1].transport->refuseNextEnqueues = 0;
        // lose ONLY the write: enqueue the intent, then drop the stock-lane write by delaying
        // past nothing -- model the loss directly: DoExtract then remove the queued contents msg.
        m.clients[1].DoExtract(L"rock-6");
        m.itemKey[L"rock-6"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        for (auto it = m.transport.wire.begin(); it != m.transport.wire.end();)
            it = (it->kind == b1model::kKindContents) ? m.transport.wire.erase(it) : it + 1;
        m.DrainWire();
        const bool expired = m.AdvanceUntil(
            [&] { return m.clients[1].ghosts.empty(); }, 15000);
        CHECK(expired, "B7: the pairing window expired and the ghost retired");
        int loss = 0;
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-6", &loss).ok && !loss,
              "B7/E: the item stays in the container (copies == 1)");
    }

    // B8: client disconnect mid-pairing -- the leaver's pairing state dies; the slot's next
    // occupant (new generation, SAME token value 1) is FRESH (L at system level).
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-7")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.transport.refuseNextEnqueues = 1;              // the intent parks in the client outbox
        m.clients[1].DoExtract(L"rock-7");
        m.itemKey[L"rock-7"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.host.DisconnectSlot(1);                        // the leaver's pairing state dies host-side
        m.clients[1].outbox.Clear();                     // (the leaver's own outbox dies with the session)
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-7").ok,
              "B8/E: the item never left the container (no orphan pairing state births it)");
        // the successor: same slot, new generation, extracts the same item with its OWN token 1.
        m.Connect(1, 11);
        m.clients[1].DoExtract(L"rock-7");
        m.itemKey[L"rock-7"].push_back(ExtractKey{1, 11, m.clients[1].nextToken});
        m.DrainWire();
        m.AdvanceUntil([&] { return !m.host.ledger.IsPending(ExtractKey{1, 11, m.clients[1].nextToken}); },
                       5000);
        m.DrainWire();
        int loss = 0;
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-7", &loss).ok && !loss,
              "B8/L/E: the successor's extraction completes independently (terminal copies == 1)");
        CHECK(Sweep(m), "B8: invariants hold");
    }

    // B9: TWO peers extracting from the SAME container CONCURRENTLY -- the CAS serializes:
    // exactly ONE write wins (one birth + one kOk); the loser is cleanly refused (its item
    // stays in the container, its ghost retires, nothing is lost, nothing is duplicated).
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.Connect(2, 20);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-8"),
                                 b1model::MakeRecord(L"Aprop_rock_C", L"rock-9")};
        for (int s = 1; s <= 2; ++s) {
            m.clients[s].viewItems = m.host.containerItems;
            m.clients[s].viewHash = m.host.CurrentHash();
        }
        m.clients[1].DoExtract(L"rock-8");               // both clients' token counters are at 1
        m.clients[2].DoExtract(L"rock-9");               // (the SAME token value, different peers)
        m.itemKey[L"rock-8"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.itemKey[L"rock-9"].push_back(ExtractKey{2, 20, m.clients[2].nextToken});
        m.DrainWire();
        m.AdvanceUntil([&] {
            return !m.host.ledger.IsPending(ExtractKey{1, 10, 1}) &&
                   !m.host.ledger.IsPending(ExtractKey{2, 20, 1});
        }, 20000);
        m.DrainWire();
        CHECK(m.host.kOkProof.size() == 1,
              "B9: exactly one extraction wins the CAS (one birth, one kOk)");
        CHECK(m.host.rejectLog.size() == 1 && m.host.rejectLog[0].second ==
                  coop::net::container_extract_result::kRefused,
              "B9: the losing write is honestly refused (kRefused), never silently dropped");
        int loss = 0;
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-8", &loss).ok && !loss,
              "B9/E: the winner's item is exactly in the world (copies == 1)");
        CHECK(b1model::CheckCopiesTerminal(m, L"rock-9", &loss).ok && !loss,
              "B9/E: the loser's item stayed exactly in the container (copies == 1) -- no loss");
        CHECK(m.host.world->KeyProven(L"rock-9") == false,
              "B9: the refused mutation spawned NOTHING (no duplication)");
        CHECK(Sweep(m), "B9: invariants hold");
    }

    // ============================================================================================
    // SECTION C -- S: the backlog is the one queue that survives a disconnect (RED on HEAD)
    // ============================================================================================
    {
        b1model::Model m;
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-s")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.clients[1].DoExtract(L"rock-s");
        m.itemKey[L"rock-s"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.transport.refuseNextEnqueues = 99;             // refuse the kOk result AND its retries
        m.DrainWire();
        m.transport.refuseNextEnqueues = 0;
        CHECK(m.host.outbox.Size() == 1, "C1/S: the kOk verdict is parked in the host backlog");
        m.host.DisconnectSlot(1);                        // GNS drops in-flight; the BACKLOG SURVIVES
        m.Connect(1, 11);                                // a DIFFERENT peer now owns slot 1
        const int before = m.clients[1].extractResultsReceived;
        m.DrainWire();
        CHECK(m.clients[1].extractResultsReceived == before,
              "C1/S: a queued result for slot1/gen10 must NEVER reach slot1/gen11");
        CHECK(b1model::CheckFreshOccupant(m, 1).ok, "C1/D6: the successor is untouched by stale traffic");
    }

    // ============================================================================================
    // SECTION D -- T: kOk only over a PROVEN key (RED on HEAD: the policy says a pointer suffices)
    // ============================================================================================
    {
        b1model::Model m;
        m.world.keyStickFailures = 1;                    // the spawn succeeds but the KEY never stuck
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-t")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.clients[1].DoExtract(L"rock-t");
        m.itemKey[L"rock-t"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.DrainWire();
        m.AdvanceUntil([&] { return !m.host.ledger.IsPending(ExtractKey{1, 10, m.clients[1].nextToken}); },
                       5000);
        m.DrainWire();
        // THE T INVARIANT: every kOk must ride a proven key. Under the current policy a bare
        // actor pointer was reported as the birth, so kOk went out with the key unestablished.
        const auto rep = b1model::CheckSystemInvariants(m);
        CHECK(rep.ok, "D1/T: kOk only over a PROVEN key (a spawn pointer alone is not a birth)");
        CHECK(m.world.KeyProven(L"rock-t"), "D1/T: the requested key is actually established on the host");
    }

    // ============================================================================================
    // SECTION E -- the DOCUMENTED policy loss (EXPECTED-FAIL, surfaced, never called lossless)
    // ============================================================================================
    {
        b1model::Model m;
        m.world.spawnFails = 1000000;                    // the birth can NEVER be established
        m.Connect(1, 10);
        m.host.containerItems = {b1model::MakeRecord(L"Aprop_rock_C", L"rock-lost")};
        m.clients[1].viewItems = m.host.containerItems;
        m.clients[1].viewHash = m.host.CurrentHash();
        m.clients[1].DoExtract(L"rock-lost");
        m.itemKey[L"rock-lost"].push_back(ExtractKey{1, 10, m.clients[1].nextToken});
        m.DrainWire();
        const bool failed = m.AdvanceUntil(
            [&] {
                for (const auto& rj : m.host.rejectLog)
                    if (rj.second == coop::net::container_extract_result::kBirthFailed) return true;
                return false;
            }, 60000);
        CHECK(failed, "E1: the bounded window lapses into the honest kBirthFailed verdict");
        m.DrainWire();
        CHECK(m.clients[1].ghosts.empty(), "E1: the client retired its ghost on the terminal verdict");
        // The copies invariant is violated BY POLICY here (the item is in neither place):
        // surfaced EXPECTED-FAIL -- the suite must not call this system lossless.
        const int copies = b1model::AuthoritativeCopies(m.host, L"rock-lost");
        EXPECTED_FAIL(copies == 1,
                      "E-LOSS: authoritative extraction birth loss after the unrecoverable 30s "
                      "birth-failure window (kBirthFailed) -- the item is in NEITHER place");
    }

    // ---- summary --------------------------------------------------------------------------------
    std::printf("\n== model selftest: %d checks pass, %d failures ==\n", g_pass, g_fail);
    for (const auto& s : g_expectedFailLog)
        std::printf("SURFACED EXPECTED-FAIL: %s\n", s.c_str());
    for (const auto& s : g_unexpectedGreenLog)
        std::printf("UNEXPECTEDLY GREEN (retire the label): %s\n", s.c_str());
    std::printf("expected-fails surfaced: %d; unexpectedly green: %d\n", g_expectedFails, g_unexpectedGreens);
    if (g_fail == 0) std::printf("MODEL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
