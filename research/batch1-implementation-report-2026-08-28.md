# BATCH-1 implementation report — 2026-08-28

Fork: **krofne/b133-playability-batch1** (from `v0.9.0n-b133-dev` = `2f32d1af`).
Primary design basis: `research/user-session-bug-triage-2026-08-28.md` (RCA phase, closed).
Full command/decision trail: `research/batch1-implementation-WORKLOG-2026-08-28.md`.

# Executive result

| Subsystem | Status | Scope |
|---|---|---|
| Drone (BUG 1 + BUG 3) | **IMPLEMENTED** | commit `304c9ac6` |
| Camera (BUG 4) | **IMPLEMENTED** | commit `5577e56b` |
| Extraction birth (BUG 5 + generic BUG 2) | **IMPLEMENTED** | commit `f83c04bf` |
| DLL build | **BLOCKED (environment)** | Linux container; Windows/MSVC-only build; repo CI lane is the documented path (see Build result) |

Verification honesty: **STATIC/UNIT PASS** = every changed TU passes a full `g++ -fsyntax-only -std=c++20` semantic check (with a one-line shim for MSVC's `__debugbreak` only), payload sizes are `static_assert`ed, the drone lane's serialization + validation matrix runs a boot-time self-test, and every new request is host-validated. **RUNTIME NOT YET VERIFIED** — nothing here was reproduced in a running game (no Windows toolchain); the 4-step friend checklist at the bottom is the verification.

# Base and fork identity

- Base tag/SHA: `v0.9.0n-b133-dev` = `2f32d1affb9bdf10447d9145c166b4832d1e449f` (upstream mod protocol 133)
- Fork branch: `krofne/b133-playability-batch1` (worktree `repo-src/.worktrees/batch1`)
- **FORK_PROTOCOL = 2133** (`kProtocolVersion`, `include/coop/net/protocol.h`). Chosen in the 2xxx band, which upstream's sequential 1xx numbering cannot collide with; it is a parse change, so a stock-b133 peer FAILS the handshake cleanly (ParseHeader rejects `h.version != kProtocolVersion`) instead of silently mis-parsing fork wire shapes; the build-number parse renames the payload `multivoid-0.9.0n-2133.dll`, so the proxy's highest-build scan always prefers the fork over stock 133. Compatibility: stock b133 <-> fork intentionally incompatible (clean reject); fork <-> fork compatible; game build remains Alpha 0.9.0n.
- New wire kinds (fork-local only, no upstream 134–143 feature pulled): `DroneActionRequest=121`, `DroneActionResult=122`, `ContainerExtractIntent=123`. None are client-relayable (host-terminal, per the existing `IsClientRelayableReliableKind` default-false), none pre-world-sendable.

# Commits

| SHA | Purpose |
|---|---|
| `304c9ac6` | feat(coop): host-authoritative drone take request (BUG 1+3) — protocol 2133, kinds 121/122, native host dispatch, ghost reconcile, self-test |
| `5577e56b` | fix(sleep): restore locally-owned sleep camera on all exits (BUG 4) — ownership latch + single idempotent restore helper, wire-neutral |
| `f83c04bf` | fix(props): author client container-extraction births (BUG 5 + generic BUG 2) — live takeObj causal bracket, kind 123, extractor re-derive, dropped-birth diagnostics |

# Drone implementation

## Exact old failure
The b133 mirror wrote `canTakeOff`/`hasSack` gate fields onto the client's parked drone every DroneState packet (`drone_sync.cpp:164`) but had no client→host lane for the take verb. A client take ran client-locally (local ghost sack + local `hasSack=false`), the next packet (≤50 ms) re-asserted host truth, the host's departure transition never ran (drone never leaves), and the option re-armed forever (infinite sack duplicates). One root, both symptoms.

## New flow
CLIENT observes the native take (0x45 `actionOptionIndex` bracket, ctx = the drone) → sends `DroneActionRequest{nonce, action=7}` → HOST validates ITS OWN drone (live, `canTakeOff`, `hasSack`, action constant, per-slot nonce monotonicity) → HOST dispatches the drone's OWN native `dropSack` UFunction (ParamFrame/Call; the vanilla BP body owns hasSack + departure FSM + the real cargo spawn — no hand-emulated state) → normal host replication distributes the outcome (DroneState gates + the real sack's ordinary PropSpawn via host_spawn_watcher) → the client retires its captured local phantom(s) on the verdict. Host-local takes never forward (client-only authoring), so the host path is 100 % native with no double dispatch.

## Request validation
Trust boundary in `event_dispatch_intent.cpp` (role==Host, senderSlot 1..kMaxPeers-1, payload ≥ 8 B) then, host-side: pure validator (`ValidateRequestPure`: `action == kDroneTakeActionIndex(7)` else `kBadAction`; `nonce > lastAccepted[slot]` else `kReplay` — replays get NO second result) and engine validation against the host's own drone (`D::Find()` live; `ReadGateFields` → `kNotArrived` / `kNoCargo`; `kDroneGone`). All deny reasons are answered to the sender so its phantom can retire. The 7 is the 2026-06-08 bytecode probe value and fails CLOSED (a wrong constant would deny every take loudly, never mis-dispatch) because the vm_dispatch Bracket carries no argument values by design — documented at `kDroneTakeActionIndex`.

## Native host dispatch
`ue_wrap::drone::DispatchDropSack` — resolves `dropSack` on the resolved `Adrone_C` class (exact-owner FindFunction), paramless call via ParamFrame/Call, then post-verifies by re-reading `hasSack` (the coin-donor discipline): a dispatch that ran without consuming cargo logs a loud WARN instead of a silent success.

## Ghost reconciliation
A third FinishSpawningActor post-hook (client-only, armed only while a request is pending) captures a fresh, untracked, non-echo, non-hand `Aprop_C` descent birth ONLY when (a) it fires INSIDE the `actionOptionIndex` bracket with ctx == the pending request's drone (exact identity — any cargo class the BP spawns), or (b) fallback for a latent spawn: ≤ 1 capture, ≤ 2 s window, ≤ 600 cm of the drone. Retire = `prop_lifecycle::DestroyLocalProp` (marks destroy-suppression first, so no PropDestroy ever crosses for a phantom the host never knew). Ordering safety: the REAL host sack is `MarkIncomingSpawn`ed before its Finish, so the capture predicates exclude it in BOTH arrival orders (result-before-PropSpawn and PropSpawn-before-result); the retire is pointer+GUObjectArray-index identity, never key/class. Verdict-lost expiry (10 s TTL / drone gone) clears state WITHOUT destroying (degraded = old behaviour for that one action, logged).

## Replay/idempotency handling
Client nonce is per-session monotonic; the host stores the highest ACCEPTED nonce per slot — a retransmit/replay is denied `kReplay` without touching the drone, and a second press after an accepted take fails the host's `hasSack` gate natively. N client presses ⇒ at most ONE accepted take per cargo state.

## Changed files
`include/coop/net/protocol.h` (fork version + kinds 121/122 + payloads + reason enum + static_asserts), `src/coop/net/session_lanes.h` (Lane::High), `include/ue_wrap/devices/drone.h` + `src/ue_wrap/devices/drone.cpp` (`IsDrone` exact-class gate, `ReadGateFields`, `DispatchDropSack`), NEW `include/coop/interactables/drone_take_sync.h` + `src/coop/interactables/drone_take_sync.cpp`, `event_dispatch_intent.cpp` (request case), `event_dispatch_state.cpp` (result case, trust-gated slot 0), `subsystems.cpp` (Install/Tick/OnDisconnect), `CMakeLists.txt`.

## Tests
Boot-time pure self-test (runs once at Install, logs PASS/FAIL): payload sizes, memcpy serialization round-trip, validator matrix (bad action → kBadAction; replayed/older/zero nonce → kReplay; fresh → kOk) — 7 checks. Static: exact-class ctx gate verified in code; wrong-role rejection at the router AND in OnRequest. RUNTIME NOT YET VERIFIED (checklist below).

## Remaining runtime risks
The take option index constant (7) is probe-derived, not read off the wire (substrate limitation) — wrong value fails closed (denials in the log, drone unaffected). If the drone BP spawns cargo LATENTLY (> 2 s after the option dispatch), no phantom is captured (in-bracket + 2 s fallback missed) — the accepted take would leave one client-local ghost until the next session; diagnosable via the `[DRONE-TAKE]` log lines (no capture line = the fallback window missed). The vanilla departure cadence after an accepted take is native and unverified here.

# Camera implementation

## Ownership latch
`g_ownsSleepView` in `sleep_sync.cpp` — set ONLY after a SUCCESSFUL mod-owned view write (ACCELERATE → `SetSleepViewTarget(SleepCam())` succeeded; WAITING → `SetSleepViewTarget(SleepingPawn())` held). Never inferred from `isSleep`.

## Restore edges
ONE idempotent helper `RestoreLocalViewIfOwned(reason)`: no-op without the latch; dilation → 1.0 iff OUR accelerate phase; clears the hold latch; view → local mainPlayer via `Registry::Get().Local()` + `GetController` + `SetViewTargetWithBlend(pc, player, 0.25f)` (the proven freecam-Disable shape); one diagnostic line. Wired on every structural exit: (1) sleep END — including the already-`!isSleep` branch where native `wakeup()` is an idempotent no-op and restores nothing (the faint → stuck-camera hole); (2) the isSleep FALLING edge — the only edge that fires if isSleep was cleared without a wakeup; (3) the fresh-gamemode reset — before the phase state is wiped; (4) OnDisconnect — restore if not natively in bed; if natively in bed, drop OUR claim and let native SP sleep own the camera (stock timelapse behaviour preserved, latches cannot fight the native wake). No per-frame reset; a repeated restore re-sets the same target (harmless blend).

## Changed files
`src/coop/player/sleep_sync.cpp` only (+75/−1). Wire-neutral.

## Tests
Static transition audit in code comments (NORMAL→sleepCam→END→mainPlayer; WAITING hold→disconnect→mainPlayer; ACCELERATE→gamemode-reset; repeated-restore harmlessness). RUNTIME NOT YET VERIFIED.

# Extraction implementation

## takeObj causal bracket
`container_contents_sync` split its two 0x45 verb ids: `addObject` = dirty-mark only; `takeObj` = the EXTRACTION edge. New export `IsClientTakeObjExtractionActive()`: true iff, on the current game thread, the innermost vm_dispatch verb is THIS module's takeObj edge on a synced WORLD container's inventory component (client role + connected + `IsInventoryComponent(ctx)` + BOUNDARY-1 `IsWorldContainerInventory` fail-closed + owner eid tracked). Called from prop_drop_intent's FinishSpawningActor hook DURING the verb body — zero wall-clock heuristics; the bracket is the window.

## Birth authorization
`PendingPlace.fromExtract` is recorded at ENQUEUE (the bracket has unwound by drain time). The drain authors `ContainerExtractIntent` (kind 123) for `fromExtract && !parked` entries with FULL metadata (className/key/propName/transform/scale/savedScalar-if-present) and LIVE physics flags (no forced kSleep — an extraction is a world birth, unlike the held-prop reel/module convention; drive classes additionally get the existing `NoteLocalDriveBirth` adoption note). Parked-place and reel/module/drive whitelist paths keep byte-identical semantics. Host side: `OnContainerExtractIntent` = class-must-walk-to-Aprop_C gate + the SAME dup-guard-by-key + the proven `HostSpawnPlacedProp` author (setKey-before-Finish + SpParity identity + scale + savedScalar), then the host's FinishSpawn watcher broadcasts the real PropSpawn to every peer and the extractor adopts its own local copy by key via the existing Phase 5S0 converge (no duplicate — the dedup-by-key converge is the shipped path).

## Metadata path
Unchanged payload shape (`PropDropIntentPayload`, 172 B): className 64, key 32, propName 32, loc/rot/scale, physFlags, savedScalar (reel-gated as before). Identity: the client's NewGuid key rides the intent; the host spawns under the SAME key; the client's existing actor converges onto it. No new identity rules.

## Managed-state rederive
The extractor's own takeObj edge now schedules the SAME `RederiveManagedState` the apply side always used (`GT::Post` to the next game-thread pump — after the mutation — per-eid 250 ms debounce, re-resolving `LivePropActor(eid)` and re-checking the container + BOUNDARY-1 gates at execution time). Recursion-safe: `updateVolumesAndMass` is measured to call only `Get Volume` (never takeObj). Debounce state cleared in OnDisconnect.

## Changed files
`include/coop/props/container_contents_sync.h` + `src/coop/props/container_contents_sync.cpp` (+68), `include/coop/props/prop_drop_intent.h` + `src/coop/props/prop_drop_intent.cpp` (+97/−10), `event_dispatch_intent.cpp`, `include/coop/net/protocol.h` (kind 123), `session_lanes.h` (Lane::Bulk).

## Tests
Static: every dropped-birth path now logs (rate-limited WARN with class/prop-name/key-state/extraction-flag/suppressed-count) — the previously invisible failure is diagnosable. Payload reuse = no new serialization to test (static_asserts carry). RUNTIME NOT YET VERIFIED (checklist).

## Over-author protection
The door opens ONLY for births flagged by the live takeObj bracket on a synced world container. Everything else still returns false at the query: addObject, personal inventory (BOUNDARY 1), untracked containers, other verbs/classes. All pre-existing filters still run (IsLive, mirror exclusion via `PeekIncomingSpawn`, Aprop_C descent, tracked-eid, hand-axis drain re-check, pending cap, key-tries). The host re-validates the class descent and dup-guards by key. Unrelated client fresh births (morph intermediates, husks, spawner churn) still hit the now-loud drop gate. Reconcile-window / InEpisode safeguards untouched.

# Build result

- Exact command (the repo's documented one; NOT runnable here): `cmake -S src/votv-coop -B build/votv-coop -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=ON` then `cmake --build build/votv-coop --config Release`.
- Compiler required: MSVC (tested reference: VS 18 BuildTools / MSVC 14.50) + vcpkg manifest + 4 compiled submodules. Result: **NOT RUN** — this is a Linux container (g++ only; no MSVC/Windows SDK/vcpkg; GameNetworkingSockets' nested webrtc submodule not fetched). Diagnosed, not worked around; no unrelated source was touched to force a compile.
- What WAS verified locally: full `g++ -fsyntax-only -std=c++20` semantic pass on all 8 changed/new TUs (with a one-line `__debugbreak` shim for the MSVC-only intrinsic in `hot_path_guard.h`).
- DLL path (to be produced): `dist/krofne-multivoid-0.9.0n-b133-batch1/multivoid-0.9.0n-2133.dll` — via the repo's own CI lane (fork → Actions → build → Run workflow on this branch → artifact). Instructions in `dist/krofne-multivoid-0.9.0n-b133-batch1/README.md`. Hash: to be recorded at CI build time.

# Runtime test checklist for user

One 5–10 minute session (host + 1 client, both on the fork DLL; if something fails, grab the mod log then):

1. Client orders a drone delivery (laptop shop).
2. When the drone arrives, the CLIENT presses Take once.
   Expected: exactly ONE sack appears; Take does not re-arm; the drone eventually departs (vanilla cadence).
3. Client extracts one normal item from any storage container.
   Expected: the other peer sees the same world item; nothing disappears/ghosts.
4. Enter/exit a bed once, then force one faint (let food/sleep run out on a stairs run or similar).
   Expected: the camera always returns to the player.

# Known not-fixed-yet

- FPS/performance (~20 FPS woken-physics floor + b133 scan/reseed amplifiers) — **Batch 2**.
- The giant-bottle/trash incident's exact trigger — not chased per scope; the generic client-birth drop (one ranked hypothesis family) is fixed here, and the new rate-limited drop WARN will name any remaining dropped-birth path if the symptom reappears.
- Trash-proxy mesh fallback (`PILE-FALLBACK`) and the use-intercept cone — untouched per scope.
- Survival settings panel (food/sleep multipliers) — untouched per scope.

---

# ADDENDUM — corrective pass (2026-08-28, post independent review)

The original report above documents the FIRST implementation pass (commits 304c9ac6 / 5577e56b /
f83c04bf). An independent review found correctness blockers A–E and fixes F–J; all were verified
against the source + RE corpus and FIXED in follow-up commits (see
`research/batch1-corrective-review.md` for the full blocker-by-blocker account):

| Follow-up commit | Contents |
|---|---|
| `ac82f8e8` | fix(drone): observe native dropSack (not actionOptionIndex) + exact phantom capture + self-heal-safe retire |
| `105949d9` | fix(drone): connection-generation-safe per-slot replay domains + 20-check selftest |
| `3fbcce9f` | fix(props): extraction birth coupled to an ACCEPTED container mutation (token pairing, kind 124, trailing re-derive) |
| `f755dc09` | fix(sleep): independent view/dilation ownership + vanilla disconnect handoff + bounded restore obligation |

## STATUS CORRECTION (read before using this report)

**BUG 5 (item extracted from container is a client-only ghost / "item from the wall") is
PARTIAL — not fully fixed.**

What IS fixed and proven by construction: the extracted actor is now HOST-AUTHORITATIVE — a
client's container extraction becomes a real, replicated world prop for every peer (the
client-only ghost and the standalone-unvalidated-birth problems are gone).

What is NOT claimed: the WALL-ORIGIN symptom. The corrective networking preserves the native
actor transform. If native takeObj births the extracted item at a wall-mounted container's
origin, the corrected pipeline will now consistently replicate the item INSIDE the wall for
everyone. No origin correction has been invented (no arbitrary +Z/+forward offset was added, by
design); proving or fixing the origin requires runtime measurement or further RE and is NOT part
of this pass.

## Wire revision note

The corrective pass revised the fork wire before any release existed (the first-pass commits
never shipped; CI never ran). Protocol stays 2133. Layouts now: DroneActionRequest/Result 8 B
(op enum, not an action index), ContainerExtractIntent 188 B (birth metadata + token + eid),
ContainerExtractResult 124 = 16 B, ContainerContents blob op=1 with a bounded token tail.
