# BATCH-1 implementation worklog — 2026-08-28 (krofne fork)

Branch: krofne/b133-playability-batch1
Base: tag v0.9.0n-b133-dev = 2f32d1affb9bdf10447d9145c166b4832d1e449f (mod protocol 133)
Scope: BATCH-1A drone take/departure+dup, BATCH-1B sleep camera softlock, BATCH-1C client extraction birth + rederive.
Explicitly OUT of scope: perf, scan hub, physics budget, trash cone/mesh, settings panel, upstream proto 134-143.

## Phase 0 — baseline

- Worktree: repo-src/.worktrees/batch1 from tag (exact SHA verified 2f32d1affb9bdf10447d9145c166b4832d1e449f).
- Environment diagnosis (build):
  - Linux container; toolchain present: g++ only. No MSVC, no Windows SDK, no cmake, no vcpkg.
  - BUILDING.md requires Windows 10/11 + VS BuildTools (tested VS18/MSVC 14.50) + CMake 3.20 + vcpkg manifest (protobuf 3.21.12 x64-windows-static) + 4 submodules; GNS needs nested webrtc (~263 MB).
  - Submodules: minhook/imgui/opus checked out in worktree; GameNetworkingSockets NOT initialized (gitlink present, nested webrtc not fetched) - configure would fail at GNS add_subdirectory.
  - VERDICT: local DLL build BLOCKED by toolchain (documented, not worked around; no unrelated source touched to force a build). The repo's documented no-local-toolchain path = GitHub Actions lane (workflow_dispatch build-core.yml) - instructions will ship in the report; artifact naming multivoid-0.9.0n-<build>.dll is load-bearing (build number parsed from kProtocolVersion).
  - Baseline build: NOT RUN (environment). Baseline unit/harness tests: the repo has no host-runnable test binary on Linux (harness is in-game); static test code will be added in-source where the repo already keeps such self-tests (see per-batch).
- Reading phase begins: all reads are at the worktree (b133) unless marked [donor:main].

## BATCH-1A (drone take) — implementation notes

Read at b133: protocol.h (ReliableKind enum tail=120, kProtocolVersion=133@708, ParseHeader version check @5055),
session_lanes.h (LaneForKind default Normal, IsClientRelayableReliableKind default false = NOT relayed),
event_dispatch_intent.cpp (family contract: role+slot validation per case), event_dispatch_state.cpp
(DroneState trust-gated slot 0), vm_dispatch.h (Bracket carries ctx ONLY - no argument values;
CurrentThreadVerb published for downstream Func seams), drone.cpp/.h (gate offsets, SuppressTick,
WriteGateFields), drone_probe.cpp (verb census: dropSack paramless; cargo classes orderbox/giftbox/inventoryContainer_drone),
prop_drop_intent.cpp (HostSpawnPlacedProp, park flow, ReelEjectIntent whitelist), prop_destroy_seam.cpp
(DestroyLocalProp = MarkIncomingDestroy + K2_DestroyActor = retire WITHOUT broadcast), prop_echo_suppress.h,
remote_prop_spawn.cpp (Phase 5S0 key-dedup: existing key -> converge, not duplicate - the adoption path),
donor [donor:main] coingun_collect.cpp (forward-and-reconcile: ParamFrame + player/action params +
balance post-verify + consumption guard + exact-class gate).

Key design decisions:
1. Fork protocol = 2133 (uint16; upstream band is 1xx sequential; ParseHeader rejects mismatch -> stock b133
   fails CLEANLY; DLL becomes multivoid-0.9.0n-2133.dll via the build-number parse; loader highest-build pick wins).
2. Observation verb = `actionOptionIndex` (the E-press option dispatch; dropSack itself is called
   INSIDE the drone BP body = same-object dispatch, not 0x45-visible). Exact-class ctx gate (IsDrone =
   ClassOf == drone_C UClass). Option index = constant 7 (bytecode probe 2026-06-08) because the vm_dispatch
   Bracket carries NO argument values (substrate design); fails CLOSED at the host (only 7 accepted).
3. Host dispatch = drone's OWN native `dropSack` UFunction via ParamFrame/Call (NOT a hand-written
   hasSack=false), post-verified by re-reading hasSack (coin-donor discipline).
4. Idempotency: client nonce (monotonic per session) + host per-slot last-ACCEPTED-nonce; replay denied
   kReplay with no result resend (the first verdict already closed the client request).
5. Phantom reconcile: FinishSpawningActor post-hook (3rd on the UFunction), capture ONLY fresh untracked
   non-echo non-hand Aprop_C births that are (a) INSIDE the actionOptionIndex bracket with ctx==pending
   drone [exact], or (b) fallback: <=1 capture, <=2 s window, <=600 cm of the drone [latent-spawn case].
   Real host sack is MarkIncomingSpawn'd before Finish -> NEVER captured in either order. Retire =
   prop_lifecycle::DestroyLocalProp (destroy-seam suppressed; no PropDestroy for a phantom the host never knew).
   TTL 10 s / drone-gone: clear WITHOUT destroying (degraded = old behavior for that one action).
6. OnDisconnect: pending dropped, phantoms deliberately KEPT (post-disconnect SP world; the drone's restored
   native tick owns the FSM; destroying would eat the player's delivery).
7. SelfTest (pure, runs once at Install): payload sizes, serialization round-trip, validator matrix
   (bad action/replay/older/zero/fresh) - 7 checks, PASS/FAIL logged at boot.

Wire: DroneActionRequest=121 (client->host, 8B, Lane::High, host-terminal), DroneActionResult=122
(host->one client, 8B, Lane::High). ContainerExtractIntent=123 enum reserved in commit 1 (handler = commit 3;
unknown kinds are ignored by the dispatch per the enum's own forward-compat contract).

Verification: g++ -fsyntax-only -std=c++20 PASS on drone_take_sync.cpp, drone.cpp,
event_dispatch_intent.cpp, event_dispatch_state.cpp, subsystems.cpp (with a __debugbreak shim for the
MSVC-only intrinsic in hot_path_guard.h; the TUs are otherwise portable). Full MSVC build = CI lane (toolchain
blockage documented in Phase 0).

## BATCH-1B (sleep camera) — implementation notes

Read at b133: sleep_sync.cpp (both camera writes :71/:224, conditional restore :91, falling edge
report-only, gamemode reset clears-without-restore, OnDisconnect deliberate no-restore), sleep.h
(SetSleepViewTarget via the sleeping pawn's controller, CallWakeup idempotent, dilation), freecam.cpp
(the SetViewTargetWithBlend(pc, player, 0.25) restore shape + Registry::Get().Local() +
engine::GetController resolution), engine.h (GetController @234, SetViewTargetWithBlend @266),
players_registry.h (Local() revalidated each query).

Changes (all in sleep_sync.cpp; wire-neutral):
- `g_ownsSleepView` ownership latch, set ONLY after a SUCCESSFUL mod-owned view write (ACCELERATE
  sleepCam handoff; WAITING bed hold). Never inferred from isSleep.
- `RestoreLocalViewIfOwned(reason)` -- the ONE idempotent restore helper: no-op without the latch;
  dilation->1.0 iff g_accelerate (our 20x); clears the hold latch; resolves local mainPlayer
  (Registry::Get().Local() + IsLive) -> GetController -> SetViewTargetWithBlend(pc, player, 0.25f)
  (freecam Disable shape); one diagnostic line with the reason. NOT per-frame.
- Restore edges wired (all structural exits):
  1. ApplyEndLocal -- after the conditional native CallWakeup (covers the !IsSleeping() strand:
     native wakeup is a no-op there and restores nothing; when wakeup DID run, re-setting the same
     target is a harmless blend).
  2. Tick isSleep FALLING edge -- the only edge that fires if isSleep was cleared without a wakeup
     (latent-chain severance / mod reset path).
  3. Fresh-gamemode reset -- restore-or-clear BEFORE the phase state is wiped (the old sleepCam died
     with the old world).
  4. OnDisconnect -- if NOT natively in bed: restore (WAITING-hold strand); if natively in bed: drop
     OUR claim and let native SP sleep own the camera (stock behavior preserved; no fighting latches).
- Native actual bed-sleep behavior preserved: no restore fires without the ownership latch; no
  premature restore while a valid synchronized phase owns the view; no per-frame reset.

Verification: g++ -fsyntax-only -std=c++20 PASS on sleep_sync.cpp. Runtime transitions
(NORMAL->sleepCam->END->mainPlayer; WAITING hold->disconnect->mainPlayer; ACCELERATE->gm reset;
double-restore harmlessness) = RUNTIME NOT YET VERIFIED (checklist in the report).

## BATCH-1C (extraction birth) — implementation notes

Read at b133: container_contents_sync.cpp full (0x45 OnVerbEntry -- ctx = propInventory component,
OwnerOf -> container actor, eid = Registry; kVerbDirty shared by addObject/takeObj; BOUNDARY 1 =
IsWorldContainerInventory Player@0x00F9 fail-closed; RederiveManagedState + ResolveRederiveFns
[updateVolumesAndMass declared ONLY on prop_container_C - declaring-class resolve; recalculateNames
on propInventory_C]; LivePropActor; GT::Post Task=std::function), prop_drop_intent.cpp (pending drain,
park flow, freshBirth whitelist, HostSpawnPlacedProp setKey-before-Finish), prop_echo_suppress.h,
ue_wrap/prop.h WalksToBase(cls, base), event_dispatch_intent PropDropIntent case (validation shape).

Changes:
1. container_contents_sync: verbs split into kVerbAdd=1 (dirty-mark only) / kVerbTake=2 (dirty-mark +
   extraction causality + re-derive). NEW export IsClientTakeObjExtractionActive(): GT + client role +
   connected + CurrentThreadVerb()==kVerbTake + IsInventoryComponent(ctx) + IsWorldContainerInventory(ctx)
   [BOUNDARY 1 fail-closed] + owner eid tracked. Called by prop_drop_intent DURING the verb body --
   zero wall-clock heuristics; the bracket is the window.
2. prop_drop_intent: PendingPlace.fromExtract recorded at ENQUEUE (the bracket has unwound by drain
   time); drain: extractBirth = fromExtract && !parked -> ContainerExtractIntent kind (123) with LIVE
   physFlags (no forced kSleep -- a world birth, unlike the held-prop reel/module convention) + full
   metadata (className/key/propName/transform/scale/savedScalar-if-present). Drive classes get the
   same NoteLocalDriveBirth adoption note. Parked-place + whitelist paths byte-identical semantics.
3. C3: the silent `continue` at the b133 gate (was :278) now emits a rate-limited WARN (1/5 s +
   suppressed-count): class, prop name, key-restored state, extract flag. No per-frame spam.
4. Host: OnContainerExtractIntent = class-must-walk-to-Aprop_C gate + the SAME dup-guard +
   HostSpawnPlacedProp author (setKey-before-Finish + SpParity identity + scale + savedScalar) ->
   host FinishSpawn watcher broadcasts the real PropSpawn -> extractor adopts its own local copy by
   key via the existing Phase 5S0 converge (remote_prop_spawn dedup-by-key). No new wire kind was
   avoidable: ReelEjectIntent's host side is class-whitelisted by design and forcing extraction
   classes through it would either break the whitelist's meaning or need a flag byte = a parse
   change anyway. A distinct kind IS the causal flag; document in protocol.h.
5. C4 re-derive: on the client's takeObj edge, GT::Post a per-eid debounced (250 ms) task ->
   LivePropActor(eid) + IsContainerActor + BOUNDARY-1 re-check -> RederiveManagedState(actor, inv)
   (the SAME proven helper the apply side uses; no duplicate implementation). Recursion-safe:
   updateVolumesAndMass is measured to call only Get Volume. Debounce map cleared in OnDisconnect.

Over-author protection audit (why this is NOT a blanket client-birth allow):
- the flag is armed ONLY while the current thread is inside THIS module's takeObj bracket on a
  SYNCED (tracked-eid) WORLD container's inventory (personal inventory and untracked containers
  return false at the query, fail-closed);
- the FinishSpawn enqueue path still applies every pre-existing filter (IsLive, PeekIncomingSpawn
  mirror exclusion, Aprop_C descent, tracked-eid, hand-axis at drain, kMaxPending cap, key-tries);
- the host still dup-guards by key and re-validates the class descent;
- unrelated client fresh births (morph intermediates, transient husks, spawner churn) still hit the
  (now loud) drop gate.

Verification: g++ -fsyntax-only -std=c++20 PASS on container_contents_sync.cpp, prop_drop_intent.cpp,
event_dispatch_intent.cpp (with the __debugbreak shim). Runtime verification = checklist.

## Corrective pass — commit: observe native dropSack + exact capture + self-heal-safe retire

Reviewer blockers A/B/C (+ FIX J drone part) applied:

- A: the 0x45 observation verb is now `dropSack` (the causal verb), NOT `actionOptionIndex`. The
  multiplexing option dispatcher can no longer author anything: a Use/Open (option 4 ->
  openPropInv) never dispatches dropSack. The request payload's `action` byte became `op`
  (drone_action_op: kNone=0 invalid, kDropSack=1); kDroneTakeActionIndex removed; deny reason
  kBadAction renamed kBadOp. The local canTakeOff/hasSack authoring filter was REMOVED -- the
  verb firing is the causal fact and the host re-validates its own gates; a stale mirror gate
  can no longer suppress a legit take. Tripwire added (NoteStreamedGates from drone_sync's
  client apply): local hasSack diverging from the streamed truth with no pending request =
  rate-limited WARN naming the old-bug signature (covers a 0x46-finalized dropSack cook, the
  documented contingency).
- B: radius/time fallback capture DELETED (kFallbackMs/kFallbackRadius/kMaxFallbackCaptures
  gone). Capture = in-OUR-dropSack-bracket on the pending drone + EXACT prop_dronesack_C class
  (resolved by name per delivery_census_probe RE) + mirror/tracked/hand exclusions + exactly one
  capture per request; a second in-bracket sack spawn = fail-closed WARN. Zero-capture verdict =
  loud WARN (ghost report request), nothing destroyed.
- C: retire is now self-heal-safe: SetSackTakenByDrone (reflected takenByDrone property, exact
  class-gated write) BEFORE DestroyLocalProp; any failure (class mismatch / property unresolved)
  REFUSES the destroy -- a leftover ghost is recoverable, a self-healed replacement is a new
  phantom minted by the cleanup.
- J (drone): DroneActionRequest/Result dispatch gates are now EXACT-size (payloadLen != sizeof
  drops), not short-only.
- SelfTest updated: 10 checks (op enum + validator matrix + round-trip).
- Syntax: all 5 touched TUs pass g++ -fsyntax-only -std=c++20 (debugbreak shim).

## Corrective pass — commit: connection-generation-safe replay state

Reviewer blocker D applied:

- The process-lifetime `static uint32_t lastNonce[kMaxPeers]` is GONE. Each slot now owns a
  DroneReplayDomain {occupancy generation, highest ACCEPTED nonce, bounded nonce->verdict cache}.
- Generation binding: OnRequest re-binds the domain whenever session.peerGenerationForSlot(slot)
  moved -- a new occupant of a reused slot starts a FRESH domain (nonce 1 admitted).
- Belt-and-braces: drone_take_sync::OnDisconnectForSlot(slot) wipes the leaver's domain; wired
  into the existing subsystems per-slot fanout. Full OnDisconnect wipes all domains.
- Verdict cache (16/slot): a duplicate/retransmitted request re-sends the CACHED verdict (accept
  OR deny) and never re-executes; a DENIED nonce stays denied even after eviction (evicted
  ancient nonces refuse kReplay without executing).
- Client consistency documented: g_nonce keeps counting across in-process reconnects; either
  policy is safe against a reset domain.
- SelfTest extended to 20 checks: D1 duplicate-accept, D2 duplicate-deny, D3 evicted-deny stays
  denied, D4 slot-reset admits nonce 1, D5 zero-nonce/unknown-op refuse.
- Syntax: drone_take_sync.cpp + subsystems.cpp PASS.

## Corrective pass — commit: extraction birth coupled to accepted container mutation

Reviewer blocker E (+ FIX F, FIX J props half) applied. The authority model is rebuilt:

- WIRE: ContainerExtractIntentPayload is now 188 B = the 172 B PropDropIntent birth metadata +
  u64 extractToken + u32 containerEid. NEW fork kind ContainerExtractResult=124 (16 B, host->one
  client, Lane::High) carries the pairing verdict. The ContainerContents blob grammar gains
  op=1: identical layout + trailing [u8 count][count x u64 token] tail (max 4/write); the
  content hash still hashes the op=0 pack, so tokens are never part of content identity; a
  malformed tail fails the whole blob closed.
- CLIENT: the takeObj edge MINTS a session-monotonic token (client only, synced world container
  only), queues it for that eid, and exposes it via CurrentExtraction() to prop_drop_intent's
  FinishSpawn hook (the bracket-live query), which binds PendingPlace{token,eid}. The dirty
  drain attaches up to 4 queued tokens to the client's next write for that eid (op=1) and pops
  them only on a successful send. The birth intent carries the same token; the local actor is
  registered as an ExtractGhost.
- HOST: OnContainerExtractIntent class-validates (Aprop_C descent) and PARKS via
  container_contents_sync::ParkExtractionBirth -- arriving as this kind is NOT causal proof and
  never spawns. ParseAndApply reads the tail BEFORE the CAS: refuse -> kRefused result to the
  author + parked birth dropped (the item stays in the host container; no duplication);
  Applied -> CommitExtractedBirth (exactly-once committed-token set, bounded 64) ->
  prop_drop_intent::SpawnExtractionBirth -> the ordinary OnPropDropIntent author (dup-guard +
  HostSpawnPlacedProp). Write-first order is covered by an applied-marker map with the same
  slot/eid matching; both directions TTL (10 s) with kExpired rejects so the client can clean
  its ghost.
- CLIENT verdict handling: accept = nothing to destroy (the host's PropSpawn adopts the local
  copy by key); reject/expired = RetireLocalGhost (self-heal-safe: a prop_dronesack_C ghost gets
  takenByDrone set first, refuse-on-failure).
- FIX F: the re-derive debounce (250 ms skip-window that could drop the trailing state in a
  burst) is replaced by a marked-set + posted coalesced drain: every take edge marks its eid,
  one drain per pump re-derives everything marked, so the FINAL mutation always gets its run.
  Recursion-safe (updateVolumesAndMass -> Get Volume only).
- FIX J (props): ContainerExtractIntent/Result dispatch gates are EXACT-size fail-closed.
- Relay note: the host relays the original blob bytes, so op=1 reaches other clients; their
  parser handles op=1 (tail read + ignored on non-hosts). Slot-0 sweeps never see tokens.
- Syntax: container_contents_sync.cpp, prop_drop_intent.cpp, both dispatchers PASS.

## Corrective pass — commit: sleep view/dilation ownership made complete

Reviewer FIX G/H/I applied:

- G: dilation ownership is now its OWN latch (g_ownsSleepDilation), set only when the module
  successfully writes the 20x (ACCELERATE), discharged when the module itself restores 1.0
  (WAITING undo) or the restore helper succeeds. It is deliberately independent of g_accelerate
  (which ApplyEndLocal clears BEFORE the restore runs -- the old stranded-20x hole). The view
  latch is unchanged in spirit: set only on successful view writes.
- I: RestoreLocallyOwnedSleepState clears an obligation ONLY on success; failures KEEP the
  obligation and arm a bounded retry (500 ms cadence, 10 s bound, pumped from Tick's head before
  the connected gate). Expiry = loud superseded discharge (a world/possession rebuild owns the
  camera), never a silent forget and never a per-frame fight.
- H: disconnect while natively in bed is now a REAL vanilla handoff: view we own ->
  SLP::SetSleepViewTarget(SleepCam()) (the native sleep presentation); dilation we own ->
  20.0f (the native timelapse value the module's own ACCELERATE mirrors). The player is NOT
  woken as a cleanup shortcut. Not-asleep disconnect -> full restore (mainPlayer + 1.0). All
  handoff failures log SUPERSEDED (SP level rebuild owns presentation).
- All structural exits now route through the one helper: sleep-end, isSleep falling edge,
  gamemode reset, disconnect (+ bounded retry pump). Syntax PASS.
