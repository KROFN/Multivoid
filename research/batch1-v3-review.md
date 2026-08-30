# Batch-1 v3 Corrective Review — krofne/b133-playability-batch1

Date: 2026-08-29 · Base: v2 HEAD `8674580b` · This pass: BLOCKERS K–Q + HARDENING Q + 0x45 reclassification
Scope guard honored: no RCA restart, no Batch 2, no performance work, no Windows CI run (still auth-blocked anyway), follow-up commits only.

## 0. What triggered this pass

An independent review of the ACTUAL v2 source found concrete correctness bugs that the v2
corrective pass had introduced or failed to fully close. Every finding was re-verified against
the source at `8674580b` BEFORE coding; each is listed below with its disposition.

## 1. Blocker dispositions (K–Q)

### BLOCKER K — extraction tail serialization/parser — FIXED
v2 `BackReadExtractTail` read the token count from the blob's LAST BYTE — which is the
most-significant byte of the last little-endian token (0 for every normal session token), so
**every** well-formed op=1 blob was deterministically rejected as malformed.
Fix (`include/coop/props/container_extract_wire.h`, new pure header):
- `ParseContentsBlob`: structural FORWARD parse — header → exactly `nRecords` → the tail at the
  CURRENT offset → exact final offset required for BOTH ops (stock op=0 permits no trailing
  bytes; the packer is the only producer).
- The FULL blob is validated before ANY caller side effect (host CAS, pairing, applies).
- `PackContents`/`PackContentsExtract` moved into the same header byte-identically (the content
  hash input is unchanged — verified by the K8 layout test).
- The selftest embeds the v2 parser VERBATIM and EXECUTES it against a well-formed blob to prove
  the v2 failure mode (K7), plus round-trips (token=1/2/0x1122334455667788, four-token write)
  and rejects (truncated token, count=0, count>max, trailing garbage, zero token).

### BLOCKER L — extraction tokens not globally unique on host — FIXED
v2 keyed the host pairing maps by the raw token alone; client slot 1's token 1 and slot 2's
token 1 collided (3–4 player use case broken by construction), and slot reuse collided with the
previous occupant.
Fix (`include/coop/props/extract_pairing.h`, new pure header; `container_contents_sync.cpp` adapter):
- `ExtractKey{senderSlot, generation, token}` — generation = `Session::peerGenerationForSlot`
  captured at arrival. ALL state (parked / applied / committed / birth-retry) keys on it.
- `container_contents_sync::OnDisconnectForSlot(slot)` (wired in subsystems) wipes the leaver's
  pending pairing state; a reused slot starts a fresh identity domain.
- Client wire tokens are UNCHANGED (per-client counters; the host already knows slot+generation).
- The v2 deferral hole is also closed: parked blobs now remember their ORIGINAL author slot, so
  a deferred client op=1 write replays under its own identity instead of slot 0.

### BLOCKER M — success result never sent — FIXED
v2's successful commit (`CommitExtractedBirth → SpawnExtractionBirth → return`) sent NO
`ContainerExtractResult{kOk}`; client ghosts leaked forever and `kExtractGhostTtlMs` was dead.
Fix:
- `SpawnExtractionBirth` returns the author outcome (`BirthVerdict`): `Spawned` /
  `AlreadyExists` (the exact key already IS the authoritative birth — the dup-guard proof) /
  `TransientFailure`. `OnPropDropIntent` returns bool accordingly.
- The ledger orders `kOk` to the originating slot ONLY after a successful verdict; the client
  erases its ghost (terminal) and keeps the actor — host PropSpawn remains the converge path.
- A transient failure AFTER the mutation was accepted parks the birth in a bounded retry
  (500 ms cadence, 30 s window) — never silently committed-and-lost. If the window lapses the
  ledger answers the honest terminal `kBirthFailed` (new reason code in protocol.h) and the
  client retires its ghost loudly.
- `kExtractGhostTtlMs` is now ENFORCED (prop_drop_intent Tick, before the pending-drain early
  return): a ghost with no verdict after 15 s is retired (self-heal-safe) and logged.

### BLOCKER N — critical fork packets ignore send failure — FIXED
`SendReliable`/`SendReliableToSlot` returning false (GNS refuses to queue) was ignored for all
four fork kinds — the authoritative-item-loss sequence (intent refused → contents write applies →
item gone from container → no birth intent exists).
Fix (`include/coop/net/send_backlog.h` pure core + `src/coop/net/send_backlog.cpp` wrapper):
- `SendCritical`: ONE real send attempt; a refusal parks the EXACT bytes (same nonce/token) for
  bounded retry (250 ms cadence, 10 s TTL, 64-entry cap, expiry counted + logged). FIFO order
  preserved; a false return is never treated as shipped.
- All four kinds route through it: DroneActionRequest (client), DroneActionResult (all 3 host
  verdict sites), ContainerExtractIntent (client drain), ContainerExtractResult (ledger sinks:
  kOk / kRefused / kExpired / kBirthFailed).
- `Tick` wired in the subsystems fan-out; `OnDisconnect` clears the backlog with the session.
- Host idempotency makes retransmit safe (replay domains; once-only ledger commits).

### BLOCKER O — pending drone request could locally dupe — FIXED
While a take was pending, DroneState packets kept re-asserting streamed `hasSack=true` on the
mirror, re-arming the native Take option; a second press ran the native body AGAIN (vm_dispatch
observation is not cancellation) — a second local sack the exactly-one capture could not absorb.
Fix (`drone_take_sync.h/.cpp`, `drone_sync.cpp`):
- `drone_take_sync::HasPendingTake()` + pure `EffectiveMirrorHasSack(streamed, pending)`.
- `drone_sync::OnReliable` writes the MASKED gate: `effective = streamed && !pending`. The REAL
  streamed truth is still recorded via `NoteStreamedGates` (diagnostics unchanged); host truth
  is never altered. Verdict closes pending → next packet restores host truth (denied) or stays
  false (accepted).

### BLOCKER P — WAITING sleep dilation handoff missed 20x — FIXED
v2 latched dilation ownership ONLY on the ACCELERATE 20x write; the WAITING forced-1.0 (the
module overwriting a native value in the OTHER direction) left no trace, so WAITING →
disconnect-while-sleeping handed vanilla sleep a world at 1x instead of its native 20x.
Fix (`include/coop/player/sleep_dilation_ownership.h` pure tracker; `sleep_sync.cpp`):
- `SleepDilationOwnership{None, WaitingForcedOne, AccelerateForcedTwenty}` +
  `DilationOwnershipTracker`. Every successful dilation write commits the state; every restore
  discharges it ONLY on success; supersede paths discharge loudly.
- Disconnect handoff while natively sleeping: `DisconnectHandoffTargetWhileSleeping()` returns
  20x for EITHER direction of override (and 0 = no write when the module never touched it —
  native sleep's own 20x is never stomped). Structural exits while awake restore 1x.
- All four transition cases are selftested (see §3).

### HARDENING Q — mask-preserving takenByDrone — DONE
`SetSackTakenByDrone` wrote the whole byte (`= 1`); UE packs several `uint8 flag : 1` bitfields
into shared bytes.
Fix (`src/ue_wrap/devices/drone.cpp`, `include/ue_wrap/core/bool_mask.h`):
- Resolve `takenByDrone` via `reflection::FindBoolProperty` (ByteOffset + ByteMask, cached once);
  write `byte = (byte & ~mask) | mask` — unrelated bits preserved. Fail closed (no destroy) if
  the property/mask cannot be resolved.

### 0x45 seam — RECLASSIFIED VERIFIED (not unresolved)
The b133 RE corpus explicitly records `drone::dropSack` as EX_LocalVirtualFunction / opcode 0x45
`[V]` for THIS target. The seam is proven; it is NOT an open question for VotV 0.9.0n.
The mirror-divergence tripwire remains wired as a DIAGNOSTIC only (it would catch a future cook
finalizing the verb onto 0x46). protocol.h, drone_take_sync.h and the v2 review's residual-risk
#1 now carry the classification.

## 2. Incidental v2 defect closed in passing
`SweepParked` replayed parked blobs as `senderSlot=0`. A client-authored op=1 blob parked during
a birth-skew would apply on replay but its tokens could never pair (kExpired → item unbacked).
Parked blobs now carry their original author slot; replay pairs under the correct ExtractKey.

## 3. Tests (tests/batch1_corrective_selftest.cpp — 93 checks, ALL PASS)

Headless/pure: compiles the REAL production headers (extract wire, pairing ledger, backlog core,
gate, dilation tracker, bool mask) + the real record codec; NOT grep-based.

| # | Required property | Test IDs |
|---|---|---|
| 1 | op=1 serialize/parse roundtrip (token=1, 2, 0x1122334455667788, 4-token) | K1–K4 |
| 2 | malformed tail rejection (truncated, count=0, count>max, trailing garbage, token 0) | K5 |
| 3 | op=0 exact consumption + trailing byte rejected | K6 |
| 4 | v2 parser demonstrably fails the well-formed blob (executed, verbatim copy) | K7 |
| 5 | v2 byte-layout identity (content hash input unchanged) | K8 |
| 6 | two clients token=1 simultaneously | L1 |
| 7 | slot reuse with new generation is fresh | L2 |
| 8 | duplicate in same slot/generation is idempotent (max one birth) | L3 |
| 9 | intent-first AND write-first pairing | M1, M2 |
| 10 | rejected mutation → zero birth + kRefused | M3 |
| 11 | successful mutation → one birth + one kOk terminal verdict | M1/M4 |
| 12 | transient birth failure → bounded retry → kOk; lapse → kBirthFailed | M4, M5 |
| 13 | expired parked intent → kExpired | M6 |
| 14 | disconnect wipes the slot's pairing state | L4 |
| 15 | first N sends fail, next succeeds; same bytes/nonce; FIFO; refusal ≠ loss; TTL expiry; oversize refused | N |
| 16 | pending drone masks the client hasSack gate | O |
| 17 | WAITING disconnect → 20x; ACCELERATE END → 1x; WAITING wake → 1x; ACCEL→WAITING disconnect → 20x; disowned → no write | P1–P5 |
| 18 | bool mask write preserves unrelated bits | Q |

Build/run: `sh src/votv-coop/tests/run_batch1_selftest.sh` (g++ -std=c++20, no engine).
Result: **93 checks, 0 failures — ALL PASS.** The in-game boot selftest in drone_take_sync also
gained the O-matrix (23 checks).

## 4. Verification performed
- All 10 touched TUs pass `g++ -std=c++20 -fsyntax-only` on Linux (MSVC shim for `__debugbreak`).
- Anti-pattern sweep: no `BackReadExtractTail`/`kDroneTakeActionIndex` remnants; the drone lane
  never references `actionOptionIndex` (the only remaining hit is the unrelated lightswitch
  probe, pre-existing); no raw `SendReliable*` calls for kinds 121–124 remain; raw-token pairing
  maps are gone; `kMaxTokensPerWrite` references all go through the pure header.
- Protocol: still 2133; kinds unchanged (121/122/123/124); payload sizes unchanged
  (8/8/188/16 B) — only the reason-code namespace gained `kBirthFailed=3`.
- Windows CI: NOT RUN (per directive; also still no push auth in this environment).

## 5. Honest residuals
1. The DLL is still built only by Windows CI (`multivoid-0.9.0n-2133.dll`); the headless selftest
   gates logic, not the MSVC link.
2. `kBirthFailed` is a genuine loss report (item in neither place). It is loud and terminal, but
   it is not an item resurrection — a true recovery would need a container re-insert author, out
   of Batch-1 scope.
3. The backlog is per-process GT state; a client whose session drops mid-retry loses parked
   sends with the session (bounded, logged) — the pairing/TTL paths own recovery after that.
