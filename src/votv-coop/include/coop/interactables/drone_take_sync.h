// coop/drone_take_sync.h -- KROFNE FORK batch-1 (protocol 2133): the delivery-drone TAKE
// action lane -- the missing causal half of the b133 drone mirror.
//
// RCA (research/user-session-bug-triage-2026-08-28.md, BUG-1/BUG-3): the b133 drone is a
// host-authoritative mirror. The host streams pose + the canTakeOff/hasSack GATE fields
// (DroneState=38) and the client suppresses its drone's ReceiveTick -- which makes the client's
// PARKED mirror fully interactable, but no client->host lane exists for the verb those options
// exist to run (the cargo take: Adrone_C::dropSack). A client take therefore runs
// client-locally: it spawns a local ghost sack and flips LOCAL hasSack; the next DroneState
// packet (<=50 ms) re-asserts host truth; the host's native departure transition never executes.
// ONE root defect, TWO symptoms: the drone never departs (BUG 1) and the take option re-arms
// forever, minting a ghost sack per press (BUG 3).
//
// THE FIX (forward-and-reconcile; donor: main's v139 CoinCollect, ported without its protocol):
//
//   CLIENT observes ITS OWN NATIVE dropSack dispatch (the 0x45 vm_dispatch verb, ctx-gated to
//           the EXACT Adrone_C class). dropSack is the CAUSAL verb -- the multiplexing
//           actionOptionIndex (RE: option 4 = openPropInv, option 7 = dropSack) is NOT
//           observable as an argument: the vm_dispatch Bracket carries the Context but NO
//           argument values, so option identity cannot be read off the seam. Observing
//           dropSack itself removes the ambiguity by construction: a Use/Open (option 4)
//           dispatches openPropInv, never dropSack, and can therefore NEVER author a request.
//        -> client sends DroneActionRequest{nonce, op=kDropSack} (one operation: "perform your
//           native dropSack"). Authority stays with the host; the local mirror state is never
//           consulted for authoring.
//        -> HOST validates ITS OWN drone (live, canTakeOff, hasSack, op, per-slot replay
//           domain) and dispatches the drone's OWN native `dropSack`
//           (ue_wrap::drone::DispatchDropSack) -- the vanilla BP body flips hasSack + runs the
//           departure FSM + spawns the REAL cargo
//        -> the host's ordinary replication distributes the outcome (DroneState gates + the
//           real sack's PropSpawn via host_spawn_watcher)
//        -> the client retires the EXACT local phantom its own take produced, captured
//           causally at FinishSpawningActor time (see below). HOST-local takes never produce a
//           request, so the host path is 100% native with no double dispatch.
//
// PHANTOM CAPTURE (never "destroy by class", never a nearest/radius scan): native dropSack
// performs the sack world spawn SYNCHRONOUSLY inside its verb body, and vm_dispatch publishes
// CurrentThreadVerb() for exactly this downstream-seam case. A FinishSpawningActor post-hook
// captures a birth ONLY when ALL of these hold:
//   - it fires INSIDE our dropSack bracket with ctx == the pending request's drone (identity);
//   - the actor's EXACT class is prop_dronesack_C (the class dropSack spawns -- RE);
//   - it is not a wire/display mirror (PeekIncomingSpawn -- the host's REAL sack is marked
//     incoming BEFORE its Finish, so neither arrival order can capture it), not element-
//     tracked, not the hand-axis view husk.
// At most ONE capture per request; a second in-bracket sack spawn for the same pending is a
// FAIL-CLOSED invariant violation (logged, nothing captured). No time/radius fallback exists:
// an unproven actor is left alone, and a verdict with zero captures logs loudly.
//
// SELF-HEAL-SAFE RETIRE: native prop_dronesack_C::ReceiveDestroyed spawns a REPLACEMENT sack
// at the drone whenever takenByDrone is false at destroy time (the native putSackOn sets
// takenByDrone=true and only then K2_DestroyActor). The retire therefore sets
// takenByDrone=true on the phantom FIRST (ue_wrap::drone::SetSackTakenByDrone, reflected
// property) and refuses to destroy on any failure -- without the flag the cleanup itself
// would mint the next phantom.
//
// OBSERVABILITY TRIPWIRE: if the client's mirror hasSack diverges from the streamed host truth
// with no pending request (the old-bug signature), a rate-limited WARN names it -- the honest
// signal if dropSack ever fails to surface at the 0x45 seam (e.g. a cook that finalizes the
// function onto the 0x46 EX_LocalFinalFunction path, which this substrate does not watch yet;
// the contingency is documented in the corrective review).
//
// Idempotency: the request nonce is monotonic per client session; the host keeps a PER-SLOT
// replay domain (reset when the slot's peer disconnects -- see the .cpp), so a replayed or
// retransmitted request cannot execute the native take twice, a denied nonce cannot become
// executable later, and a rejoining client on a reused slot starts a fresh domain.
//
// Wire: DroneActionRequest=121 (client->host) / DroneActionResult=122 (host->one client), both
// 8 B, Lane::High, host-terminal (never relayed). Fork protocol 2133 (see protocol.h).

#pragma once

#include "coop/net/protocol.h"

#include <atomic>
#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::drone_take_sync {

// Store the session + run the pure-logic self test once (logs PASS/FAIL lines).
void Install(coop::net::Session* session);

// Game-thread tick: lazily registers the 0x45 dropSack observation verb (resolve is
// GT-deferred by the substrate), expires a stale pending request (verdict lost), and runs the
// mirror-divergence tripwire. Near-free when idle.
void Tick();

// HOST ingest (event_dispatch_intent): validate + native dispatch + verdict. Client no-op.
void OnRequest(coop::net::Session& session, const coop::net::DroneActionRequestPayload& p,
               uint8_t senderSlot);

// CLIENT ingest (event_dispatch_state): the verdict for OUR nonce -> retire the captured
// phantom, clear the request. Host no-op.
void OnResult(const coop::net::DroneActionResultPayload& p);

// Streamed-gate note wired from drone_sync's client mirror apply (every DroneState packet):
// the tripwire's reference for "what the host currently says the mirror holds".
void NoteStreamedGates(bool canTakeOff, bool hasSack);

// CLIENT: true while a DroneActionRequest of ours is awaiting its verdict (BLOCKER O).
// drone_sync reads this on EVERY DroneState packet and masks the mirrored hasSack gate with
// it: while a take is pending, the mirror must NOT re-arm the native Take option -- the local
// dropSack body already ran (one phantom captured, one request in flight), and a re-armed
// option would let a second press spawn a second local sack the capture cannot absorb. The
// mask is presentation-only: the STREAMED host truth is never altered, the verdict closes the
// pending (a denied take restores host truth on the next packet; an accepted one keeps the
// gate false because the host's own hasSack just dropped), and nothing is written back.
bool HasPendingTake();

// BLOCKER O (pure, headless-testable): the mirror hasSack the client should PRESENT for this
// packet. Host truth is recorded separately (NoteStreamedGates) for diagnostics -- this only
// decides what lands on the interaction gate. While a take is pending, hasSack is presented
// false so the native Take option cannot re-arm between the press and the verdict (the
// vm_dispatch observation is NOT a cancellation: the native body already ran, and a second
// native body is a second local sack).
inline bool EffectiveMirrorHasSack(bool streamedHasSack, bool takePending) {
    return streamedHasSack && !takePending;
}

// Session teardown: drop the pending request WITHOUT destroying captured phantoms (the session
// is gone; if the host never processed the take the phantom is the only cargo in what is now a
// local world -- destroying it would eat the player's delivery) and reset EVERY per-slot replay
// domain.
void OnDisconnect();

// HOST, one peer left (subsystems' per-slot fanout): wipe THAT slot's replay domain so the next
// occupant of the slot starts a fresh one (a rejoining client's nonce 1 is a first request).
// Belt and braces on top of the occupancy-generation binding in OnRequest.
void OnDisconnectForSlot(int slot);

}  // namespace coop::drone_take_sync
