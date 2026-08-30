// coop/props/prop_drop_intent.h -- the CLIENT-place -> HOST-authoritative keyed-prop DROP INTENT lane.
//
// ONE concept: a CLIENT placing a keyed world prop it had PICKED UP (hold-R place). Prop sync is
// host-authoritative: a client's own fresh Aprop_C spawn is skipped at prop_lifecycle:210 (client
// keyed spawns are host-auth), so a client-placed rock is INVISIBLE to the host (F2). The proper fix
// (NOT a seam-catch crutch -- see the /qf-15 convergence in
// research/findings/join-identity/votv-keyed-prop-grabdrop-intent-lane-DESIGN-2026-07-09.md) is the pattern
// chipPiles already use: the CLIENT sends an INTENT, the HOST is the sole authority.
//
// THE FLOW (Increment 1 = the DROP half; the grab half already works -- the client's hold-R pickup
// DESTROY crosses the bidirectional destroy-seam and the host destroys its copy):
//   1. PICKUP: the client's hold-R pickup destroys the world rock -> DestroySeamBody broadcasts
//      DESTROY(key) (host destroys ITS copy) and calls NoteClientKeyedDestroy(key) -> the key is
//      PARKED. The park is the SAFETY INVARIANT: it means "the host has no copy of this key now",
//      so a later re-spawn of it authors exactly ONE host prop -> no host dup.
//   2. PLACE: the client's hold-R place (simulateDrop) spawns a FRESH Aprop_C; our FinishSpawn
//      post-hook enqueues it; one tick later (Key restored by loadData) Tick() reads the Key and, iff
//      it is in the park set, sends PropDropIntent{className,key,propName,transform,scale,physFlags}
//      to the host and unparks it.
//   3. HOST: OnPropDropIntent spawns the authoritative Aprop by Key at the transform (NOT
//      echo-suppressed) -> the host's own FinishSpawn watcher (host_spawn_watcher) expresses it and
//      broadcasts PropSpawn to ALL peers. The placing client adopts its own untracked local rock by
//      Key (ResolveLiveActorByKey scan-fallback) -> no dup.
//
// WHY the +1-tick send is load-bearing: the place ALSO destroys the in-hand display husk. If that
// husk-destroy crosses as DESTROY(key), the reliable channel delivers it BEFORE the +1-tick
// PropDropIntent -> the host processes it against a rock it no longer has (no-op) THEN spawns from
// the intent -> the spawn survives. (v2 authored the spawn same-tick and the husk-destroy killed it.)
//
// Client-side state (park set + pending) is game-thread-only. The host handler is game-thread-only.
// [[feedback-folder-per-domain-concept-rule]] [[lesson-client-keyed-prop-move-two-wire-halves]]
// [[lesson-reuse-proven-author-not-raw-reimpl]] [[feedback-map-all-wire-events-before-fixing-missing-sync]]

#pragma once

#include <string>

#include "coop/net/protocol.h"
#include "coop/props/extract_pairing.h"   // BirthVerdict (the v3 blocker-M author outcome)

namespace coop::net { class Session; }

namespace coop::prop_drop_intent {

// Install the CLIENT FinishSpawn post-hook (chains after host_spawn_watcher's on the same
// FinishSpawningActor UFunction). Idempotent + retry-throttled while the UFunction is unresolved.
// Game thread. Safe to call every subsystem-install tick.
void Install(coop::net::Session* session);

// Per-net-pump-tick drain (CLIENT): for each pending place spawn whose Key is now restored AND parked,
// author a PropDropIntent to the host and unpark. Game thread. No-op on the host / empty pending.
void Tick(coop::net::Session* session);

// Called from prop_lifecycle::DestroySeamBody right AFTER a CLIENT broadcasts a keyed-prop DESTROY:
// park the key so a later same-key place authors a host-authoritative drop intent (and only then --
// the host has already destroyed its copy => no dup). Game thread. Bounded FIFO set.
void NoteClientKeyedDestroy(const std::wstring& key);

// HOST handler for a received PropDropIntent: spawn the authoritative Aprop_C by Key at the transform
// (the host's FinishSpawn watcher broadcasts it). Dup-guarded (skips if the host already has the Key
// live). Returns TRUE when the authoritative prop EXISTS after the call -- spawned now, or the exact
// key was already live (the dup-guard) -- FALSE only when the birth could not be established (v3
// blocker M uses this to distinguish success from a transient failure).
bool OnPropDropIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                      uint8_t senderSlot);

// v114 (L7): HOST handler for ReliableKind::ReelEjectIntent -- a CLIENT's caddy/reelbox eject
// birthed a reel prop in its hands (a client Aprop_C spawn never broadcasts), so the host authors
// it via the SAME HostSpawnPlacedProp path. CLASS-WHITELISTED to the Aprop_reel_C lineage (not a
// general client-spawn door). The payload's savedScalar carries the reel's Progress; the kSleep
// flag makes the host copy spawn inert until the client's held-prop pose stream drives it.
// Design: research/findings/computers-devices/votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md.
void OnReelEjectIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                       uint8_t senderSlot);

// KROFNE FORK (batch-1C, REVISED by the corrective pass / blocker E): HOST handler for
// ReliableKind::ContainerExtractIntent -- a CLIENT's native takeObj on a synced WORLD container
// birthed an Aprop_C locally. ARRIVING AS THIS KIND IS NOT CAUSAL PROOF: the handler only
// class-validates the payload and PARKS it (container_contents_sync::ParkExtractionBirth). The
// birth is authored ONLY when the container mutation carrying the SAME extraction token returns
// Applied from the existing baseHash CAS -- a refused (stale) mutation spawns NOTHING, so the
// item cannot end up both in the host container and in the world.
void OnContainerExtractIntent(coop::net::Session& session,
                              const coop::net::ContainerExtractIntentPayload& p,
                              uint8_t senderSlot);

// KROFNE FORK (blocker E; REVISED v3 / blocker M): HOST-side commit, called by the pairing ledger
// (coop::props::extract_pairing) when the paired contents mutation returned Applied. Runs the
// SAME author as every other intent (dup-guard by key + HostSpawnPlacedProp), then the host
// FinishSpawn watcher broadcasts the real PropSpawn and the extractor adopts its own local copy
// by key. Returns the author OUTCOME so the ledger can reach a terminal state:
//   Spawned          -- the host spawned the keyed actor (kOk is ordered by the ledger)
//   AlreadyExists    -- the exact key already exists as the same authoritative birth (kOk)
//   TransientFailure -- the birth could not be established (the ledger parks + retries bounded,
//                       then answers kBirthFailed; the mutation was ALREADY accepted, so the item
//                       is never silently lost)
coop::props::extract_pairing::BirthVerdict SpawnExtractionBirth(
    coop::net::Session& session, const coop::net::ContainerExtractIntentPayload& p,
    uint8_t senderSlot);

// KROFNE FORK (blocker E): CLIENT handler for ContainerExtractResult -- accept = terminal success,
// the tracking entry is erased (the host's own PropSpawn adopts our local copy by key); ANY
// accepted=0 (kRefused / kExpired / v3's kBirthFailed) = retire the local extracted ghost
// (self-heal-safe: a prop_dronesack_C ghost gets takenByDrone set first).
void OnContainerExtractResult(const coop::net::ContainerExtractResultPayload& p);

// Session teardown -- clear the park set + pending. Game thread.
void Reset();

}  // namespace coop::prop_drop_intent
