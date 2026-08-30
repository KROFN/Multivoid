// tests/batch1_fuzz_selftest.cpp -- KROFNE FORK (2133), Batch-1 TEST CONTRACT layer 5.
//
// Deterministic fuzz/property runner over the MODEL harness (batch1_model_harness.h -- pure
// fakes around the REAL pure cores). No external fuzz framework: a seeded splitmix64 PRNG.
//
//   ./batch1_fuzz_selftest [iterations] [seed]
//   defaults: 10000 iterations, seed 0xB1F00D5EED -- deterministic across machines.
//   The seed parses with strtoull base 0: decimal AND 0x-hex literals both work (the CI
//   workflow passes the 0x form; a base-10 parse would silently truncate it to 0).
//
// Every iteration builds a fresh Model and runs a random event script. Events draw from the
// full fault alphabet: peer slot / occupancy generation / tokens+nonces (per-client counters),
// packet DUPLICATION, REORDERING, send REFUSAL, DELAYED delivery, DISCONNECT, RECONNECT
// (slot reuse with a new generation), TIMEOUT (clock advance), RETRY (backlog pump).
//
// After EVERY event the always-on invariants are verified (D1/D5, D2, D4, T, E no-2-copies;
// see batch1_model_harness.h); at the settle phase every registered item must reach a TERMINAL
// state with authoritativeCopies == 1 -- OR one of the TWO DOCUMENTED policy losses:
//   loss #1  kBirthFailed terminal (the bounded birth-retry window lapsed; the client is told);
//   loss #2  unbacked write (the write applied, the intent never paired: marker expired or the
//            extractor disconnected) -- nobody can even be told;
// both are SURFACED and counted, never silently accepted and never called "lossless".
//
// STALE GENERATION: old-generation deferred traffic can never mutate a new-generation client
// (the S binding) -- verified by the fresh-occupant check after every reconnect and by the
// stale-drop pure checks in the corrective selftest.
//
// ON FAILURE: prints the seed, the full numbered event sequence, a state dump, and the exact
// replay command. Exit 1.

#include "batch1_model_harness.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace b1model;

namespace {
uint32_t g_traceIter = 0xFFFFFFFFu;    // B1_FUZZ_TRACE=<iteration> -> dump state after each event

void Trace(uint32_t iteration, int ev, const Model& m) {
    if (iteration != g_traceIter) return;
    std::printf("  after event %d: now=%llu wire=%zu clientOut=%zu hostOut=%zu parked=%zu "
                "applied=%zu committed=%zu retry=%zu container=[",
                ev, (unsigned long long)m.nowMs, m.transport.InFlight(),
                m.clients[1].outbox.Size(), m.host.outbox.Size(), m.host.ledger.ParkedCount(),
                m.host.ledger.AppliedCount(), m.host.ledger.CommittedCount(),
                m.host.ledger.BirthRetryCount());
    for (const auto& r : m.host.containerItems)
        std::printf("%ls,", r.key.c_str());
    std::printf("] worldRockC=%d unbacked=%zu kOk=%zu itemKey:\n",
                (int)m.world.KeyProvenCount(L"rock-c"), m.host.ledger.UnbackedLostTotal(),
                m.host.kOkProof.size());
    const auto b = m.host.ledger.Buckets();
    auto dump = [](const char* name, const std::vector<ExtractKey>& v) {
        std::printf("    %s:", name);
        for (const auto& k : v)
            std::printf(" {s%u,g%u,t%llu}", k.senderSlot, k.generation,
                        (unsigned long long)k.token);
        std::printf("\n");
    };
    dump("parked  ", b.parked);
    dump("applied ", b.applied);
    dump("retry   ", b.retry);
    dump("committed", b.committed);
    for (const auto& kv : m.itemKey)
        for (const auto& k : kv.second)
            std::printf("    itemKey '%ls' -> {s%u,g%u,t%llu}\n", kv.first.c_str(),
                        k.senderSlot, k.generation, (unsigned long long)k.token);
    std::printf("    kOkProof:");
    for (const auto& kp : m.host.kOkProof) std::printf(" (t%llu,proven=%d)", (unsigned long long)kp.first, kp.second);
    std::printf("  rejects:");
    for (const auto& rj : m.host.rejectLog) std::printf(" (t%llu,r%u)", (unsigned long long)rj.first, rj.second);
    std::printf("  pendingTokens(s1)=%zu ghosts(s1)=%zu worldLive=%zu\n",
                m.clients[1].pendingTokens.size(), m.clients[1].ghosts.size(), m.world.actors.size());
}

struct FuzzFailure {
    uint64_t seed = 0;
    uint32_t iteration = 0;
    std::string invariant;
    std::string detail;
    std::vector<std::string> events;
    std::string state;
};

std::string StateDump(const Model& m) {
    char buf[512];
    std::string s;
    std::snprintf(buf, sizeof buf, "host: dropSackExec=%d sacks=%d accepted=%d denied=%d "
                  "outbox=%zu parked=%zu applied=%zu committed=%zu retry=%zu container=%zu",
                  m.host.dropSackExecutions, m.host.sacksSpawned, m.host.acceptedRequests,
                  m.host.deniedRequests, m.host.outbox.Size(), m.host.ledger.ParkedCount(),
                  m.host.ledger.AppliedCount(), m.host.ledger.CommittedCount(),
                  m.host.ledger.BirthRetryCount(), m.host.containerItems.size());
    s += buf;
    s += " | world: live=";
    std::snprintf(buf, sizeof buf, "%zu", m.world.actors.size());
    s += buf;
    for (const auto& a : m.world.actors) {
        s += " [";
        s += std::to_string(a.id);
        s += "='";
        s += std::string(a.observedKey.begin(), a.observedKey.end());
        s += "']";
    }
    for (int sl = 1; sl < kModelSlots; ++sl) {
        const FakeClient& c = m.clients[sl];
        if (!c.pendingTake && !c.localPhantoms && c.ghosts.empty() && m.host.gen[sl] == 0) continue;
        std::snprintf(buf, sizeof buf, " | slot%d: gen=%u pending=%d phantoms=%d ghosts=%zu "
                      "streamed=%d resRecv=%d exResRecv=%d",
                      sl, m.host.gen[sl], (int)c.pendingTake, c.localPhantoms, c.ghosts.size(),
                      (int)c.streamedHasSack, c.resultsReceived, c.extractResultsReceived);
        s += buf;
    }
    return s;
}

// One random scenario. Returns false and fills `failure` on an invariant violation.
// `loss1`/`loss2` tally the surfaced DOCUMENTED policy losses (kBirthFailed / unbacked write).
bool RunScenario(uint64_t seed, uint32_t iteration, FuzzFailure* failure,
                 uint64_t* loss1, uint64_t* loss2) {
    Rng rng(seed * 1000003ull + iteration);
    Model m;
    std::vector<std::string> log;
    g_ingestTrace = (iteration == g_traceIter);   // ingest trace only for the traced scenario

    // boot: one or two peers, a random generation each, a container with 1-3 items.
    const wchar_t *kItemNames[] = {L"rock-a", L"rock-b", L"rock-c", L"rock-d"};
    const int nItems = 1 + (int)rng.Below(3);
    for (int i = 0; i < nItems; ++i)
        m.host.containerItems.push_back(MakeRecord(L"Aprop_rock_C", kItemNames[i]));

    auto connect = [&](int slot) {
        const uint32_t gen = 10u + rng.Next() % 4000u;
        m.Connect(slot, gen, rng.Chance(70));
        char b[64];
        std::snprintf(b, sizeof b, "connect slot=%d gen=%u", slot, gen);
        log.push_back(b);
    };
    connect(1);
    if (rng.Chance(50)) connect(2);

    const int nEvents = 8 + (int)rng.Below(14);
    for (int ev = 0; ev < nEvents; ++ev) {
        char b[96];
        // an event only addresses CONNECTED slots (a disconnected client cannot act in production)
        const int slot = 1 + (int)rng.Below(2);
        const bool connected = m.host.gen[slot] != 0;
        switch (rng.Below(10)) {
        case 0: {   // a drone take press
            const bool ok = connected ? m.clients[slot].PressTake() : false;
            std::snprintf(b, sizeof b, "pressTake slot=%d connected=%d -> %d", slot,
                          (int)connected, (int)ok);
            log.push_back(b);
            break;
        }
        case 1: {   // an extraction edge
            const wchar_t* item = kItemNames[rng.Below((uint32_t)nItems)];
            const bool holds = connected && m.clients[slot].ContainerHolds(item);
            if (holds) {
                m.clients[slot].DoExtract(item);
                m.itemKey[item].push_back(ExtractKey{static_cast<uint8_t>(slot), m.host.gen[slot],
                                                     m.clients[slot].nextToken});
            }
            std::snprintf(b, sizeof b, "extract slot=%d item=%ls held=%d", slot, item, (int)holds);
            log.push_back(b);
            break;
        }
        case 2: {   // fault injection for the NEXT enqueues
            const uint32_t kind = rng.Below(4);
            if (kind == 0) { const int n = 1 + (int)rng.Below(3); m.transport.refuseNextEnqueues = n;
                             std::snprintf(b, sizeof b, "fault refuse x%d", n); }
            else if (kind == 1) { m.transport.dupNextEnqueues = 1; std::snprintf(b, sizeof b, "fault duplicate"); }
            else if (kind == 2) { m.transport.delayNextEnqueues = 1; std::snprintf(b, sizeof b, "fault delay"); }
            else { m.transport.delayNextEnqueues = 1 + (int)rng.Below(2); std::snprintf(b, sizeof b, "fault lane-stall"); }
            log.push_back(b);
            break;
        }
        case 3: {   // a short quiesce pump (retry / delivery)
            m.DrainWire();
            log.push_back("drain");
            break;
        }
        case 4: {   // a clock advance (timeouts: backlog TTL, pairing window, birth retry)
            const uint64_t dt = 250ull * (1 + rng.Next() % 8);
            m.Step(dt);
            std::snprintf(b, sizeof b, "advance +%llums", (unsigned long long)dt);
            log.push_back(b);
            break;
        }
        case 5: {   // a peer leaves
            m.host.DisconnectSlot(slot);
            m.clients[slot].outbox.Clear();
            std::snprintf(b, sizeof b, "disconnect slot=%d", slot);
            log.push_back(b);
            break;
        }
        case 6: {   // a peer (re)connects -- slot reuse with a NEW generation
            connect(slot);
            break;
        }
        case 7: {   // the host drone's gates move (denial paths + mirror truth)
            m.host.drone.hasSack = rng.Chance(60);
            m.host.drone.canTakeOff = rng.Chance(90);
            std::snprintf(b, sizeof b, "droneGates hasSack=%d canTake=%d",
                          (int)m.host.drone.hasSack, (int)m.host.drone.canTakeOff);
            log.push_back(b);
            break;
        }
        case 8: {   // the streamed DroneState (host truth -> mirrors)
            m.StreamDroneState(slot);
            std::snprintf(b, sizeof b, "streamDrone slot=%d hasSack=%d", slot,
                          (int)m.host.drone.hasSack);
            log.push_back(b);
            break;
        }
        default: {  // a bounded wait (the pairing/birth windows tick while traffic drains)
            m.Step(120);
            log.push_back("settle-step");
            break;
        }
        }
        // NOTE: there is deliberately NO "resync views from the host" event. The client's LOCAL
        // container view is its own machine state (a pending extraction has physically removed
        // the item there); only an in-wire HOST BROADCAST may rewrite it (the harness does that
        // on every refused/applied write). An out-of-band resync would re-add a locally-taken
        // item and mint impossible duplications -- an unphysical fault, not a test.
        // THE INVARIANT GATE: after EVERY event.
        Trace(iteration, ev, m);
        const InvariantReport rep = CheckSystemInvariants(m);
        if (!rep.ok) {
            failure->seed = seed; failure->iteration = iteration;
            failure->invariant = rep.code;
            failure->detail = rep.what;
            failure->events = log;
            failure->state = StateDump(m);
            return false;
        }
    }

    // THE SETTLE: advance past every bounded window, then judge every item's terminal state.
    for (int i = 0; i < 90; ++i) m.Step(500);   // 45 s: > pairing window + birth-retry window
    m.DrainWire();
    const InvariantReport rep = CheckSystemInvariants(m);
    if (!rep.ok) {
        failure->seed = seed; failure->iteration = iteration;
        failure->invariant = rep.code; failure->detail = rep.what;
        failure->events = log; failure->state = StateDump(m);
        return false;
    }
    for (const auto& item : m.itemKey) {
        int loss = 0;
        const InvariantReport crep = CheckCopiesTerminal(m, item.first, &loss);
        if (!crep.ok) {
            failure->seed = seed; failure->iteration = iteration;
            failure->invariant = crep.code; failure->detail = crep.what;
            failure->events = log; failure->state = StateDump(m);
            return false;
        }
        if (loss == 1) ++(*loss1);   // loss #1: the kBirthFailed terminal verdict
        if (loss == 2) ++(*loss2);   // loss #2: the unbacked write (surfaced, not hidden)
    }
    return true;
}

void PrintFailure(const FuzzFailure& f) {
    std::printf("\n==================== FUZZ FAILURE ====================\n");
    std::printf("seed       = %llu\n", (unsigned long long)f.seed);
    std::printf("iteration  = %u\n", f.iteration);
    std::printf("invariant  = %s\n%s\n", f.invariant.c_str(), f.detail.c_str());
    std::printf("replay     = ./batch1_fuzz_selftest 1 %llu   (scenario %u of that seed)\n",
                (unsigned long long)f.seed, f.iteration);
    std::printf("event sequence:\n");
    for (size_t i = 0; i < f.events.size(); ++i)
        std::printf("  %3zu: %s\n", i + 1, f.events[i].c_str());
    std::printf("state dump:\n  %s\n", f.state.c_str());
    std::printf("======================================================\n");
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 10000ull;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0xB1F00D5EEDull;
    if (const char* t = std::getenv("B1_FUZZ_TRACE"))
        g_traceIter = static_cast<uint32_t>(std::strtoul(t, nullptr, 10));
    if (std::getenv("B1_TRACE_INGEST")) b1model::g_ingestTrace = true;

    std::printf("== Batch-1 fuzz (model harness; invariants after every event) ==\n");
    std::printf("   iterations=%llu seed=%llu\n", (unsigned long long)iterations,
                (unsigned long long)seed);

    uint64_t loss1 = 0, loss2 = 0;   // the surfaced policy losses
    FuzzFailure failure;
    uint32_t done = 0;
    for (uint64_t it = 0; it < iterations; ++it) {
        if (!RunScenario(seed, static_cast<uint32_t>(it), &failure, &loss1, &loss2)) {
            PrintFailure(failure);
            std::printf("FUZZ FAIL\n");
            return 1;
        }
        ++done;
    }
    std::printf("   %u scenarios clean; invariants verified after every event\n", done);
    std::printf("   surfaced policy losses (documented, counted -- the system is NOT lossless):\n");
    std::printf("     loss #1 kBirthFailed terminal (bounded birth-retry window lapsed): %llu\n",
                (unsigned long long)loss1);
    std::printf("     loss #2 unbacked write (write applied, intent never paired):       %llu\n",
                (unsigned long long)loss2);
    std::printf("FUZZ PASS\n");
    return 0;
}
