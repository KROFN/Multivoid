// coop/interactables/drone_replay_domain.h -- KROFNE FORK (2133): the HOST-side per-slot
// drone-take replay domain, carved verbatim out of drone_take_sync.cpp in the test-contract
// pass (Layer 1/2 of the Batch-1 test contract need the REAL admission decisions headless;
// a file-local copy cannot be compiled by tests/batch1_*.cpp).
//
// PURE MOVE ONLY: the structs/inline bodies below are byte-identical to the ones that lived
// in drone_take_sync.cpp (which now includes this header). No decision changed.
//
// Per-slot REPLAY DOMAIN (host, game-thread only; BLOCKER D).
//
// Why a domain and not a bare highest-nonce scalar: the b133-era draft kept a process-lifetime
// `static uint32_t lastNonce[kMaxPeers]` that (a) survived a peer disconnect, so a REJOINING
// client that restarts its nonce at 1 on a reused slot was refused kReplay FOREVER, and (b)
// remembered only ACCEPTED nonces, so a denied nonce could become executable the moment the
// host's gate state changed. The domain fixes both:
//   - it is bound to the slot's OCCUPANCY GENERATION (session.peerGenerationForSlot -- the
//     host-minted per-occupancy counter): a new occupant on the same slot starts a FRESH domain;
//   - it caches the verdict for every seen nonce (bounded): a duplicate/retransmitted request is
//     answered from the cache WITHOUT executing, and a DENIED nonce stays denied no matter how
//     the host's gates later change;
//   - OnDisconnectForSlot wipes it as a belt-and-braces reset on top of the generation binding.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <map>

namespace coop::drone_take_sync {

// ---- pure validator + admission gate (unit-tested by SelfTest and the headless suites) --------
// The ONE-request admission gate: what may run, what may be re-answered, what is refused.
enum class Admit { Execute, ResendVerdict, Refuse };
struct DroneVerdict { uint8_t accepted; uint8_t reason; };
struct Admission { Admit action; DroneVerdict verdict; };

struct DroneReplayDomain {
    uint32_t generation = 0;                      // slot occupancy generation this domain serves
    uint32_t lastAcceptedNonce = 0;               // highest ACCEPTED nonce in THIS domain
    std::map<uint32_t, DroneVerdict> verdicts;    // every seen nonce -> its verdict (bounded)
};
// Verdict-cache bound: a nonce falls out only after 16 newer nonces were seen; the original
// verdict rode a reliable lane, so an evicted duplicate can only ever be answered kReplay.
constexpr size_t kMaxVerdictCache = 16;

// Pure admission (testable headless). op gate -> zero gate -> cache -> monotonicity.
inline Admission AdmitRequest(DroneReplayDomain& d, uint8_t op, uint32_t nonce) {
    if (op != coop::net::drone_action_op::kDropSack)
        return {Admit::Refuse, {0, coop::net::drone_action_result::kBadOp}};
    if (nonce == 0)
        return {Admit::Refuse, {0, coop::net::drone_action_result::kReplay}};
    const auto it = d.verdicts.find(nonce);
    if (it != d.verdicts.end())
        return {Admit::ResendVerdict, it->second};    // duplicate: the verdict, never the action
    if (nonce <= d.lastAcceptedNonce)
        return {Admit::Refuse, {0, coop::net::drone_action_result::kReplay}};  // ancient, evicted
    return {Admit::Execute, {}};
}

// Commit an EXECUTED request's outcome into the domain (both outcomes are cached -- the deny
// must not become executable later just because the gates changed).
inline void RecordVerdict(DroneReplayDomain& d, uint32_t nonce, uint8_t accepted, uint8_t reason) {
    if (accepted) d.lastAcceptedNonce = nonce;
    d.verdicts[nonce] = DroneVerdict{accepted, reason};
    while (d.verdicts.size() > kMaxVerdictCache)
        d.verdicts.erase(d.verdicts.begin());         // evict the OLDEST nonce
}

}  // namespace coop::drone_take_sync
