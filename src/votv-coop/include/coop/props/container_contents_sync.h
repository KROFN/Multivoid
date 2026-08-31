// coop/props/container_contents_sync.h -- take-4 R11: a WORLD container's CONTENTS as
// host-authored state.
//
// RE of record: research/findings/inventory-items/votv-container-contents-gobjstack-RE-2026-07-22.md
//
// THE ROOT, stated as an invariant rather than a site list: a container's contents are NOT on the
// container. Every container in the game reads them from ONE global per-peer array
// `saveSlot.GObjStack : TArray<struct_mObject>`, addressed by `propInventory_C.Index`; the actor
// holds only that index and a cached volume. The client receives GObjStack once inside the join
// save-transfer blob and NEVER again, because every mutating verb (addObject / addLoot / takeObj)
// dispatches EX_LocalVirtualFunction -- BP-internal, invisible to both the ProcessEvent detour and
// the UFunction::Func patch. So the drone delivery landed full on the host and 0.0 on the client,
// ~19 minutes after the container itself had been adopted. R11 is one visible symptom of that one
// missing lane, not a drone bug.
//
// AUTHORITY was already correct and is NOT changed here: drone_sync parks the client's drone
// ReceiveTick, so the client never ran a delivery. This module adds only the missing LANE.
//
// SHAPE
//   edge   -- ue_wrap/core/vm_dispatch (the GNatives 0x45 swap) on `addObject` + `takeObj`.
//             The callback only MARKS THE COMPONENT DIRTY; it takes no action and reads no
//             arguments, which is why it is correct for every caller in the firing set
//             (drone::compileOrder x2, propInventory::addLoot x4, prop_container::
//             putObjectIn_overlap, the prop_container ubergraph, lib::findInventoryObject,
//             prop_container::{extract,getObject}, checkObjectsVolume).
//   send   -- v125 (R11b): THE PEER WHOSE VERB FIRED authors, on the sweep after the edge. There
//             is no intent/deny shape available -- takeObj is EX_LocalVirtualFunction, so the item
//             has ALREADY moved on the presser before any seam of ours exists; a request the host
//             could refuse cannot be built ([[lesson-presser-authored-state-not-intent-for-
//             invisible-verbs]]). The host ARBITRATES a client slice (baseHash compare-and-swap:
//             accept only if the author edited the truth the host last published and no host-side
//             change is in flight), applies it, and RELAYS to every other peer EXCLUDING the
//             author -- echoing a peer's own state back reverts its newer local value and primes
//             the baseline over it (the eaten-scroll race). A refused write is answered with the
//             host re-publishing its truth to that author, and counted: that counter is the
//             evidence that decides whether a rollback shape is ever needed. Before v125 the
//             callback returned at IsHost() and a client's every extraction was dropped -- the
//             client took 1 of 2 burgers, the host still saw 2, and the world gained a burger.
//   apply  -- raw-write the receiver's own GObjStack slot, then re-derive the SETTER-MANAGED
//             state through the engine's own verbs: Aprop_container_C::updateVolumesAndMass()
//             (currVol + Mass) and propInventory_C::recalculateNames() (display). We never
//             raw-write currVol/Mass/names ([[feedback-no-raw-write-of-setter-managed-fields]]).
//             `addObject` CANNOT be the apply verb -- measured, it takes a live AActor* and
//             serializes it itself, so it cannot ingest a record off the wire.
//             `checkObjectsVolume` is NOT called on apply -- measured, it calls takeObj and is an
//             overflow EJECTOR; calling it would destroy contents. updateVolumesAndMass is safe:
//             measured, it calls only `Get Volume`.
//
// TWO HARD BOUNDARIES, both fail-closed
//   1. PERSONAL inventory is NOT this lane's business. mainPlayer and ui_playerInventory are
//      backed by the SAME global GObjStack, so a host-authored write over another peer's slice
//      would WIPE that player's inventory. The discriminator is `propInventory_C.Player`
//      (bool @ 0x00F9, true only on prop_inventoryContainer_player's component template). If it
//      is true -- OR if the offset cannot be resolved at all -- the container is SKIPPED.
//   2. A NESTED container ships WITHOUT its index. struct_save does not contain itself: a
//      container inside a container is represented by INDIRECTION -- its record's ints[] carries
//      its own GObjStack index, which names a slot in the SENDER's array and is meaningless on
//      the receiver (it would resolve to some unrelated container's contents -- worse than
//      empty). So a record whose class descends from prop_container_C ships with ints[] CLEARED
//      and arrives EMPTY-not-BROKEN. The transitive walk (with a receiver-side visited set,
//      depth cap and total-size cap -- a sender-side cap is not a cap) is INCREMENT 2.
//
// Wire: ReliableKind::ContainerContents (BlobChunkPayload; blob = [u8 op=0][u32 eid][u16 n] then
// n x the coop/items/save_record_wire per-record grammar). Never refanned; receivers accept
// senderSlot==0 only. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::container_contents_sync {

void Install(coop::net::Session* session);

// Registers the 0x45 verbs on first call, then drives the vm_dispatch FName resolve and the
// dirty-set drain. Near-free when the dirty set is empty (the steady state).
void Tick();

// ContainerContents chunks.
void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: ship the joiner one contents blob per live world container (principle 8 -- the join
// save-transfer blob is a snapshot taken at OnRequest time; this is the anchor that corrects it).
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

// HOST, one peer left (subsystems' per-slot fanout; v3 blocker L): drop THAT slot's pending
// extraction pairing state (parked intents, applied markers, bounded birth retries). The slot's
// next occupant is a NEW generation and starts a clean identity domain -- nothing of the leaver
// can pair, commit, or answer into it.
void OnDisconnectForSlot(int slot);

// ---- KROFNE FORK (batch-1C): the extraction-birth causality query ------------------------------
// True iff, on the CURRENT game thread, the innermost vm_dispatch verb is this module's takeObj
// edge on a SYNCED WORLD container's inventory (client role, connected). prop_drop_intent's
// FinishSpawningActor hook calls this DURING the verb body to flag a freshly-born Aprop_C as the
// causal product of a client container extraction (the birth the b133 pipeline silently dropped).
// Everything else -- addObject, personal inventory (BOUNDARY 1), untracked containers, other verbs
// -- is false, so this is NOT a blanket client-birth allow. Game thread only.
bool IsClientTakeObjExtractionActive();

// ---- KROFNE FORK (corrective pass, blocker E): the extraction PAIRING API ---------------------
// The host authors an extracted world birth ONLY after the paired contents mutation is Accepted
// by the existing baseHash CAS. The correlation token is minted on the client INSIDE the takeObj
// edge, rides the birth intent (ContainerExtractIntent) AND the client's next contents write for
// the same eid (an op=1 blob tail), and pairs on the host regardless of arrival order.

// The token minted at the CURRENT takeObj edge (0 = none). Called by prop_drop_intent's
// FinishSpawningActor hook WHILE the bracket is live, to bind the birth to the token. GT only.
void CurrentExtraction(uint64_t& outToken, uint32_t& outEid);

// HOST: record a just-arrived ContainerExtractIntent. The intent is PARKED (or, if its write
// already Applied, committed immediately); nothing spawns from the packet alone. The pairing
// key binds (senderSlot, occupancy generation, token) -- v3 blocker L. Returns false for a
// duplicate-committed or malformed intent.
bool ParkExtractionBirth(const coop::net::ContainerExtractIntentPayload& p, uint8_t senderSlot);

// ---- dev-instrument seams (coop/dev/container_selftest) --------------------------------------
//
// These exist so the instrument REUSES this module's measured world-vs-personal boundary instead
// of reimplementing it. A probe that re-derives BOUNDARY 1 for itself would be free to get it
// wrong in a way the shipped lane is not, and then the instrument would be testing a different
// rule than the one that ships.

struct WorldContainer {
    uint32_t eid;
    void* actor;
    void* inv;    // the propInventory_C component
};

// Fill `out` with up to `want` live WORLD containers (BOUNDARY 1 applied). Host or client.
size_t SnapshotWorldContainers(WorldContainer* out, size_t want);

// The observable digest for one container: how many records its GObjStack slice holds and what
// currVol the engine currently reports. Both peers print it; the smoke compares the NUMBERS.
// False if the eid does not resolve to a live world container on this peer.
bool ContentsDigest(uint32_t eid, int32_t& outCount, float& outVol);

// ---- KROFNE FORK (ACTUATOR-ONLY pass): the READ-ONLY extraction-baseline readout ------------
// Dev-instrument OBSERVER for the extraction actuator's convergence gate (coop/dev/
// extract_convergence.h): reports the base hash this peer last CONSUMED for the eid (0 = host
// truth not yet applied) and the hash of the live local GObjStack slice under the production
// ContentHash -- the two numbers "proven convergence" is defined on. WRITES NOTHING: no
// g_baseHash/g_publishedHash mutation, no version bump, no CAS interaction -- the actuator gate
// can wait for convergence but can never manufacture it. False if the eid does not resolve to a
// live world container on this peer (the caller treats that as base=0: never fire). Game thread.
bool DevBaselineReadout(uint32_t eid, uint64_t& outBaseHash, uint64_t& outLocalHash);

}  // namespace coop::props::container_contents_sync
