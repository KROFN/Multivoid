# BATCH-1 corrective review — 2026-08-28 (post independent-review blockers A–E, fixes F–J)

Scope: corrective implementation pass on `krofne/b133-playability-batch1`. No Batch 2, no
performance work, no refactors beyond the blockers. Every reviewer finding was verified against
the actual source and the b133 RE corpus BEFORE implementing; the verification evidence is cited
per item. Follow-up commits only — the original review history is untouched.

| Item | Verdict on review claim | Commit |
|---|---|---|
| BLOCKER A — drone observes the wrong verb | **CONFIRMED** | `ac82f8e8` |
| BLOCKER B — phantom capture fallback unsafe | **CONFIRMED** | `ac82f8e8` |
| BLOCKER C — dronesack self-heal | **CONFIRMED** (mechanism present in RE corpus) | `ac82f8e8` |
| BLOCKER D — replay state not generation-safe | **CONFIRMED** | `105949d9` |
| BLOCKER E — extraction birth not coupled to accepted mutation | **CONFIRMED** | `3fbcce9f` |
| FIX F — extractor re-derive must be trailing | **CONFIRMED** | `3fbcce9f` |
| FIX G — dilation owned separately from view | **CONFIRMED** | `f755dc09` |
| FIX H — disconnect handoff while natively sleeping | **CONFIRMED** | `f755dc09` |
| FIX I — failed resolve must not lose restore obligation | **CONFIRMED** | `f755dc09` |
| FIX J — fork packets must fail closed on size | **CONFIRMED** | `ac82f8e8` + `3fbcce9f` |
| STATUS — BUG 5 wall origin | **ACCEPTED: BUG 5 downgraded to PARTIAL** (see the report addendum) | `e-newer docs commit` |

## BLOCKER A — observe the causal verb, not the option dispatcher

Verification: `vm_dispatch.h` Bracket = `{ctx, verbId, depth}` — no argument values (the header
states it and the struct proves it). The 2026-06-08 bytecode probe already established
`actionOptionIndex`: option 4 → `openPropInv(container)`, option 7 → `dropSack()`. The v1 code's
`if (canTakeOff && hasSack) p.action = 7` therefore could not distinguish a client Use/Open from
a take — a legitimate option-4 dispatch authored a host dropSack.

Fix (`ac82f8e8`):
- The 0x45 observation verb is now `dropSack` itself, ctx-gated to the exact `Adrone_C` class.
  A Use/Open dispatches `openPropInv` and can NEVER author a request — the ambiguity is removed
  by construction, not by filtering.
- The request no longer pretends to echo an observed argument: `action` (with
  `kDroneTakeActionIndex=7`) is replaced by an explicit fork operation enum
  (`drone_action_op::kDropSack = 1`; `kNone = 0` invalid). The request represents exactly ONE
  operation: "perform your native dropSack". Unknown ops deny `kBadOp` (fail closed).
- The client-side `canTakeOff/hasSack` authoring pre-filter was REMOVED: the verb firing IS the
  causal fact, and a stale mirror gate could suppress a legitimate take. The host re-validates
  its own gates (unchanged).
- Host-local takes still never forward (client-only authoring) — no double dispatch.
- Comments/docs/tests no longer claim Bracket exposes option values. The SelfTest checks the op
  enum and the validator matrix.
- **Observability tripwire** (the one honest residual risk): the shipped vm_dispatch watches
  opcode 0x45 only; 0x46 (EX_LocalFinalFunction) has no exposed registration and no measured
  customer. If a future cook finalizes `dropSack` onto the 0x46 path, the verb would go silent.
  `drone_sync`'s client apply now feeds the streamed gates into `NoteStreamedGates`, and the
  lane's Tick raises a rate-limited TRIPWIRE WARN when the mirror's local hasSack falls below the
  streamed truth with no pending request — the exact old-bug signature, named in the log.

## BLOCKER B — exact phantom capture

Verification: the v1 fallback (≤2 s + 600 cm + one arbitrary untracked Aprop_C) existed as
described. Native `dropSack` spawns the sack synchronously inside its verb body (the same RE the
capture window relies on), so the fallback could only ever be a liability.

Fix (`ac82f8e8`): the radius/time fallback is DELETED. Capture requires ALL of:
live `dropSack` bracket (`CurrentThreadVerb`) with `ctx ==` the pending drone (pointer identity);
EXACT `prop_dronesack_C` class (resolved by name, the `delivery_census_probe` pattern);
`PeekIncomingSpawn` exclusion first (the host's real sack is marked before Finish, so neither
arrival order can capture it); untracked; not the hand-axis husk. Exactly ONE capture per
request; a second in-bracket sack spawn is a logged fail-closed violation; a verdict with zero
captures logs loudly (a possible ghost — nothing is destroyed).

## BLOCKER C — self-heal-safe retire

Verification: `prop_dronesack_C` + its `takenByDrone` property are runtime-resolvable via
reflection (the repo's own `delivery_census_probe.cpp:181–190` reads exactly that class and
property by name→offset), consistent with the reviewer's native `ReceiveDestroyed` self-heal RE
(`putSackOn` sets `takenByDrone=true`, THEN destroys — the flag is the engine's own "do not
self-heal" bit).

Fix (`ac82f8e8`): `ue_wrap::drone::SetSackTakenByDrone()` sets the reflected property (exact
class-gated write; resolve failures latch a WARN and fail every caller). The retire path sets
the flag BEFORE `DestroyLocalProp` and REFUSES the destroy on any failure — a leftover ghost is
recoverable; a destroy with the flag unset mints a replacement phantom. The same guarded helper
is reused by the extraction reject path (BLOCKER E) so a `dronesack` extracted from a container
cannot self-heal either. Normal sack behavior is untouched: only the phantom/ghost retire writes
the flag.

## BLOCKER D — connection-generation-safe replay state

Verification: `static uint32_t s_lastNonce[kMaxPeers]` at function scope, never reset on peer
leave; only ACCEPTED nonces remembered. The subsystems per-slot disconnect fanout exists
(`subsystems.cpp`) and the session exposes a host-minted occupancy generation
(`peerGenerationForSlot`) — both levers the reviewer asked for are available.

Fix (`105949d9`):
- Per-slot `DroneReplayDomain {generation, lastAcceptedNonce, nonce→verdict cache (16)}`,
  module-owned (not function-static).
- Generation binding: every request re-binds the domain if the slot's occupancy generation
  moved → a rejoining client on a reused slot starts a FRESH replay domain (nonce 1 admitted).
- `drone_take_sync::OnDisconnectForSlot(slot)` wipes the leaver's domain — wired into the
  existing per-slot fanout — and `OnDisconnect` wipes all.
- Duplicate/retransmitted requests are answered from the verdict cache WITHOUT executing
  (both accepts and denies are cached). A denied nonce can never become executable later, even
  after cache eviction (evicted ancient nonces refuse `kReplay` without executing).
- Client consistency documented: the client's nonce keeps counting across in-process reconnects;
  either policy is safe against a reset domain.
- SelfTest extended to 20 checks: duplicate-accept, duplicate-deny, evicted-deny-stays-denied,
  slot-reset-admits-nonce-1, zero-nonce/unknown-op.

## BLOCKER E — extraction birth coupled to an ACCEPTED mutation

Verification: confirmed end-to-end. The v1 intent went straight to `OnPropDropIntent` (class
gate only). The contents lane has a real CAS (`HostAcceptsClientWrite` on
`baseHash == g_publishedHash` + conflict window; refused writes are answered by a host
re-publish), and the two kinds ride different lanes (Bulk vs Normal), so cross-lane ordering is
unassumed. The v1 race could genuinely produce item-in-container + world-prop = duplication.

Fix (`3fbcce9f`) — the reviewer's preferred architecture, on the existing arbitration:
- **Token**: minted on the client INSIDE the takeObj edge (client role + synced world container
  only, session-monotonic). It rides BOTH the birth intent (new 188 B
  `ContainerExtractIntentPayload` = 172 B birth metadata + u64 token + u32 eid) AND the client's
  next contents write for that eid — a fork-local blob op=1 with a back-read
  `[u8 count][count × u64]` token tail (≤4 per write; content hash still hashes the op=0 pack,
  so tokens are never part of content identity; malformed tail drops the whole blob fail-closed).
- **HOST**: `OnContainerExtractIntent` class-validates (Aprop_C descent) and PARKS via
  `container_contents_sync::ParkExtractionBirth`. Arriving as the kind is NOT causal proof and
  NEVER spawns. In `ParseAndApply`, the tail is read BEFORE the CAS:
  - write REFUSED → `ContainerExtractResult{kRefused}` per token + parked birth dropped → the
    item stays in the host container, the client retires its local ghost → **no duplication**;
  - write APPLIED → `CommitExtractedBirth` (exactly-once committed-token set, bounded 64) →
    `SpawnExtractionBirth` → the ordinary author (dup-guard by key + HostSpawnPlacedProp) → the
    host's PropSpawn broadcast adopts the extractor's local copy by key.
- **Ordering**: intent-first parks; write-first records an applied-marker (same slot+eid match)
  and commits when the intent arrives. Both directions TTL (10 s) with `kExpired` rejects; a
  stale client token that never ships expires too, so it can never ride a LATER write.
- **Idempotency**: a committed token can never birth again (duplicate intents are logged and
  ignored).
- **Result ack**: `ContainerExtractResult = 124` (16 B, host→one client, Lane::High) cleans the
  client-local ghost on reject/expiry. Accept needs no destroy — the host's PropSpawn adopts the
  local copy by key (the existing converge).
- **Reuse**: the entire pairing hangs off the EXISTING CAS and re-publish path; no second
  arbitration was built. The host's relay forwards the original blob bytes, so other clients
  parse op=1 and ignore the tail (non-hosts never arbitrate).

## FIX F — trailing re-derive

Verification: the 250 ms per-eid skip-window could drop the trailing re-derive of a burst.

Fix (`3fbcce9f`): the take edge now MARKS the eid in a pending set; a posted, coalesced drain
(one per game-thread pump) re-derives everything marked after the native mutations land — the
FINAL mutation in a burst always gets its run. Recursion-safe (updateVolumesAndMass → Get Volume
only, never takeObj). Debounce state removed; cleared on disconnect.

## FIX G — dilation owned separately

Verification: the restore helper keyed the dilation undo off `g_accelerate`, which
`ApplyAccelerateLocal(false)` clears BEFORE the restore runs in `ApplyEndLocal` — a stranded
path could restore the camera and leave the world at 20x.

Fix (`f755dc09`): `g_ownsSleepDilation` is its own latch — set on the SUCCESSFUL 20x write,
discharged on the mod's own successful 1.0 restore (WAITING undo) or by the restore helper's
success. The helper handles each obligation independently and idempotently.

## FIX H — vanilla handoff while natively sleeping

Verification: the v1 disconnect path just dropped the latches ("native sleep owns the view")
while WAITING may have left dilation at the mod's 1.0 and the view on the mod's bed hold — not a
native presentation.

Fix (`f755dc09`): disconnect while natively in bed now hands back to vanilla EXPLICITLY —
`SetSleepViewTarget(SleepCam())` for a view we own (the native sleep presentation), `20.0f` for
a dilation we own (the native timelapse value this module's ACCELERATE mirrors). The player is
NEVER woken as a cleanup shortcut. Not-asleep disconnect restores mainPlayer + 1.0. Every
handoff failure logs `SUPERSEDED` (the SP level rebuild owns the presentation wholesale).

## FIX I — failed resolve keeps the obligation

Verification: the v1 helper cleared the latch before proving player/controller/blend — a failed
resolution during a transition permanently forgot the restore.

Fix (`f755dc09`): obligations clear ONLY on success; a failure arms a bounded, low-cadence retry
(500 ms interval, 10 s bound) pumped from Tick's head (before the connected gate). Deadline
expiry = loud superseded discharge (a world/possession rebuild necessarily supersedes), never a
per-frame fight and never a silent forget.

## FIX J — exact-size fork packets

Verification: all fork handlers used `< sizeof` (short-only) checks.

Fix (`ac82f8e8` drone, `3fbcce9f` extraction): `DroneActionRequest`, `DroneActionResult`,
`ContainerExtractIntent`, `ContainerExtractResult` dispatch gates are `!= sizeof` (exact) — an
oversized malformed payload drops with a log, matching the payload `static_assert`s (8/8/188/16
B). Existing stock kinds are untouched.

## Re-verification proofs (by construction + code)

- **Use/Open produces ZERO requests**: the only authoring path is the `dropSack` 0x45 callback;
  `openPropInv` never matches that verb name. The actionOptionIndex verb is no longer registered
  anywhere.
- **Only native dropSack can author**: one verb registration, one callback, gated on exact
  Adrone_C ctx + client role.
- **One accepted take = one host native dropSack**: the host dispatches `dropSack` exactly once
  per Execute admission; duplicates hit the verdict cache and never execute.
- **Phantom cleanup cannot self-heal**: `takenByDrone` is set (and exact-class proven) before
  every controlled retire; a failed flag write refuses the destroy.
- **No radius/proximity heuristic remains**: the fallback constants and branch are gone; capture
  is bracket + exact class only.
- **Reconnect/slot reuse cannot poison nonce state**: generation binding + per-slot disconnect
  reset (SelfTest D4).
- **Extraction: no accepted mutation → no birth**: `SpawnExtractionBirth` is reachable ONLY from
  `CommitExtractedBirth`, which is reachable ONLY from the `Ingest::Applied` branch of
  `ParseAndApply` (or the applied-marker path, which is written only from that same branch).
- **Refused stale mutation → zero host birth**: the refuse branch answers kRefused and erases
  the parked intents.
- **Reorder works**: both arrival orders commit (parked-intent path + applied-marker path).
- **Duplicate token → max one birth**: committed-token set checked at park and at commit.
- **View/dilation separately owned and restored**: two latches, one helper, independent
  discharge.
- **isSleep=false strand → mainPlayer + 1.0**: `RestoreLocallyOwnedSleepState` discharges both
  obligations (dilation only if owned).
- **Disconnect while native sleep → vanilla handoff**: SleepCam + native 20x, no wake.
- **Failed target resolution does not discard responsibility**: obligation kept + bounded retry
  + superseded-with-log.

## Residual honest risks (documented, not hidden)

1. ~~`dropSack` visibility at the 0x45 seam~~ **RECLASSIFIED v3: VERIFIED for VotV 0.9.0n b133.**
   The b133 RE corpus explicitly records `drone::dropSack` as EX_LocalVirtualFunction / opcode
   0x45 `[V]` for THIS build — the seam is proven, not presumed, and is NOT an open question for
   this target. The tripwire stays wired purely as a DIAGNOSTIC: it would still name the failure
   if some FUTURE cook finalized the verb onto the 0x46 path (contingency: enable the 0x46
   wrapper with dropSack as its first measured customer). No current-build risk remains here.
2. The extraction pairing assumes the takeObj edge's container write happens within the 10 s
   pairing windows; longer stalls reject cleanly (kExpired) and the host truth stands.
3. BUG 5's wall-origin symptom is NOT claimed fixed — see the status correction in the
   implementation report (PARTIAL): the corrective networking makes the extracted item
   host-authoritative and consistently replicated, wherever native takeObj births it — including
   inside a wall-mounted container's origin. No arbitrary offset was invented; an origin
   correction needs runtime/RE proof and is NOT part of this pass.
