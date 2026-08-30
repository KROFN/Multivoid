# Batch-1 Test Contract (KROFNE fork, protocol 2133)

The contract: **the defined invariants are executable and green** -- not "reviewed and
believed correct". Five layers; every layer runs in CI (`.github/workflows/batch1-test-contract.yml`).

Process rule for every correctness bug from now on:
1. a test that FAILS on the buggy revision; 2. prove it fails for the expected reason;
3. the smallest fix; 4. prove the test passes; 5. run the complete suite.
No fix without a regression test unless the behavior is impossible to exercise outside
the actual UE runtime.

## Layers

| Layer | File(s) | Runs | What it proves |
|---|---|---|---|
| 1 -- pure protocol/logic | `tests/batch1_corrective_selftest.cpp` (+ `run_batch1_selftest.sh`) | headless, g++/cl | the exhaustive per-subsystem matrices over the REAL shipped headers: wire codec (K), pairing ledger identity/pairing/verdicts (L/M), drone admission domain (D), mirror gate (O), sleep ownership (P), bool-mask algebra (Q), backlog retry/purge/generation-binding (N/S), birth success proof (T), smoke rate limiter |
| 2 -- model / state machines | `tests/batch1_model_selftest.cpp` + `tests/batch1_model_harness.h` | headless | SYSTEM behavior: FakeTransport/Host/Clients/Drone/Container/World around the real pure cores. Invariants D1-D6 (drone), the authoritative-copies invariant (extraction), T (kOk only over a proven key), S (backlog never crosses generations), the surfaced EXPECTED-FAIL policy losses |
| 3 -- integration gate | `tools/batch1/integration_gate.py` | pre-build | the build graph compiles the tested code into the DLL: every Batch-1 TU exists + is listed in CMake (no glob, no duplicates), fork kind ids unique, payload sizes compile-verified against the declared static_asserts, kinds 121-124 never bypass SendCritical |
| 4 -- Windows CI | `.github/workflows/batch1-test-contract.yml` | windows-latest | LOGIC + MODEL + FUZZ + gate (all under MSVC), then CMake configure + MSVC Release compile + DLL link + artifact upload. Green = LOGIC/MODEL/FUZZ/BUILD-GRAPH/MSVC COMPILE/LINK PASS |
| 5 -- fuzz/property | `tests/batch1_fuzz_selftest.cpp` | headless | seeded (deterministic) PRNG; 10,000 scenarios of random slot/generation/token/nonce traffic with duplication, lane-stall delay, send refusal, disconnect, reconnect (slot reuse + new generation), timeouts; invariants verified after EVERY event; failure = seed + numbered event sequence + state dump + replay command |

## The invariants (executable)

Drone:
- D1 one valid client Take causes AT MOST ONE host native dropSack
- D2 while Take is pending the local mirror cannot expose another Take
- D3 a denied request creates ZERO authoritative sacks
- D4 an accepted request creates EXACTLY ONE authoritative sack
- D5 duplicate/reordered request/result packets cannot increase the sack count
- D6 a result for an old slot generation can never affect a new occupant

Extraction (for one logical item identity):
- `authoritativeCopies = (inContainer ? 1 : 0) + (inWorld ? 1 : 0) == 1` at every terminal
  state -- never 0, never 2, EXCEPT the two DOCUMENTED policy losses, which are SURFACED
  (EXPECTED-FAIL), never called lossless:
  - loss #1: the bounded 30s birth-retry window lapses -> kBirthFailed (the client is told)
  - loss #2: the write applied but the intent never paired (marker expired / extractor left)
    -- nobody can even be told (found by the fuzz; surfaced by `Ledger::WasUnbacked`)

Stale generation: old-generation deferred traffic never mutates new-generation state
(backlog generation binding + PurgeSlot; stale-generation write/intent guards).

## Running locally (Linux, no engine)

```sh
sh src/votv-coop/tests/run_batch1_selftest.sh                       # layer 1
g++ -std=c++20 -I src/votv-coop/include \
    src/votv-coop/tests/batch1_model_selftest.cpp \
    src/votv-coop/src/coop/items/save_record_wire.cpp \
    src/votv-coop/src/coop/interactables/signal_wire.cpp -o model && ./model   # layer 2
./model-equivalent for batch1_fuzz_selftest.cpp, then ./fuzz [iters] [seed]    # layer 5
python3 tools/batch1/integration_gate.py                            # layer 3
```

## Runtime contract (manual, one real session)

Set `[dev] batch1_smoke=1` in multivoid.ini, play normally (drone take, container
extraction, sleep), and require these milestone lines in the log (rate-limited 1/tag/s,
log-only): `B1_DRONE_VERB`, `B1_DRONE_REQ`, `B1_DRONE_HOST_ACCEPT`,
`B1_DRONE_NATIVE_DROPSACK`, `B1_DRONE_PHANTOM_CAPTURE`, `B1_DRONE_RESULT`,
`B1_DRONE_REAL_SACK`, `B1_EXTRACT_EDGE`, `B1_EXTRACT_MUTATION_APPLIED`,
`B1_EXTRACT_BIRTH_COMMIT`, `B1_EXTRACT_HOST_KEY_PROVEN`, `B1_EXTRACT_RESULT`,
`B1_SLEEP_VIEW_OWN`, `B1_SLEEP_DILATION_OWN`, `B1_SLEEP_RESTORE reason=`. A missing
milestone names the dead causal hop precisely.
