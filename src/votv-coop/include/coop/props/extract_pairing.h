// coop/props/extract_pairing.h -- KROFNE FORK (2133): the HOST-side extraction pairing ledger,
// carved out of container_contents_sync.cpp in the v3 corrective pass (blockers L + M).
//
// WHY A HEADER: the pairing rules -- slot+generation identity, exactly-once birth, both arrival
// orders, refuse/expiry answers, the bounded post-accept birth recovery -- are PURE decision
// logic once the sends and the spawn are abstracted behind two tiny sinks. tests/
// batch1_corrective_selftest.cpp compiles THIS class headless and drives the full pairing
// matrix against it; the game TU wires the sinks to send_backlog + prop_drop_intent, so the
// tested decisions are the shipped decisions.
//
// BLOCKER L (identity): every client mints extraction tokens from its OWN counter starting at 1,
// so client-slot 1's token 1 and client-slot 2's token 1 are DIFFERENT extractions -- and a slot
// reused after a disconnect is a DIFFERENT occupant whose token 1 must not collide with the
// previous occupant's either. v2 keyed the host pairing maps by the RAW token alone: two clients
// extracting simultaneously collided, and the 3-4 player use case was broken by construction.
// The ledger keys EVERY state (parked / applied / committed / birth-retry) by
// ExtractKey{senderSlot, generation, token}. The client wire format is UNCHANGED (the token
// stays per-client; the host already knows the sender slot and its occupancy generation).
//
// BLOCKER M (terminal success): a commit must REACH a terminal state. The ledger treats an
// authoritative birth verdict of Spawned OR AlreadyExists (the exact key is already the same
// authoritative birth) as SUCCESS -- it commits the key once-only and orders the kOk result --
// and TransientFailure as "park the birth and retry, bounded" (the container mutation was
// ALREADY accepted, so silently losing the item is not an option). If the bounded window
// lapses, the ledger orders the honest terminal kBirthFailed result: the item is now in
// neither place, and every peer is told so loudly instead of a ghost leaking forever.

#pragma once

#include "coop/net/protocol.h"   // ContainerExtractIntentPayload / ContainerExtractResultPayload (pure PODs)

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace coop::props::extract_pairing {

// ---- identity (BLOCKER L) ---------------------------------------------------------------------

struct ExtractKey {
    uint8_t  senderSlot = 0;   // 1..kMaxPeers-1 (the host is never an extractor)
    uint32_t generation = 0;   // Session::peerGenerationForSlot at arrival (the slot-reuse proof)
    uint64_t token      = 0;   // the client-minted correlation token (never 0 on the wire)

    friend bool operator==(const ExtractKey& a, const ExtractKey& b) {
        return a.senderSlot == b.senderSlot && a.generation == b.generation && a.token == b.token;
    }
    friend bool operator<(const ExtractKey& a, const ExtractKey& b) {
        if (a.token != b.token)          return a.token < b.token;
        if (a.senderSlot != b.senderSlot) return a.senderSlot < b.senderSlot;
        return a.generation < b.generation;
    }
};

// The birth-author outcome (what the real author -- HostSpawnPlacedProp -- reports).
enum class BirthVerdict : uint8_t {
    Spawned          = 0,   // the host spawned the keyed actor this call
    AlreadyExists    = 1,   // the exact key already exists as the same authoritative birth
    TransientFailure = 2    // the authoritative birth could not be established (yet)
};

// THE BIRTH SUCCESS PROOF (test-contract pass, review finding T).
//
// A spawned ACTOR POINTER by itself proves NOTHING: if the key write did not stick (a
// non-keyable class, a failed setKey resolve, a mangled key), the host now holds a pointer to
// an actor that is NOT the requested birth -- no peer can ever converge to it by key, yet the
// naive policy reported it as Spawned and the extractor was told kOk. The invariant this judge
// enforces (and the model suite asserts): **kOk may only leave the host over a PROVEN key** --
// the live actor's OWN key must equal the requested key. A mis-keyed pointer is a TRANSIENT
// failure (the caller reaps the zombie and the ledger's bounded retry re-authors the birth).
struct BirthKeyProof {
    bool actorLive           = false;  // the spawn call produced a live actor pointer
    bool requestedKeyNonEmpty= false;  // the intent actually named a key
    bool observedKeyMatches  = false;  // the live actor's OWN key == the requested key
    bool preExisting         = false;  // the key was already live before this attempt
};

inline BirthVerdict JudgeBirth(const BirthKeyProof& pr) {
    if (!pr.actorLive) return BirthVerdict::TransientFailure;
    if (!pr.requestedKeyNonEmpty || !pr.observedKeyMatches)
        return BirthVerdict::TransientFailure;    // pointer without identity = NOT the birth
    return pr.preExisting ? BirthVerdict::AlreadyExists : BirthVerdict::Spawned;
}

// Sinks: production wires them to send_backlog::SendCritical / prop_drop_intent::
// SpawnExtractionBirth; tests script them. Both run on the game thread in production.
class ResultSink {
public:
    virtual ~ResultSink() = default;
    virtual void SendExtractResult(uint8_t toSlot,
                                   const coop::net::ContainerExtractResultPayload& p) = 0;
};
class BirthSink {
public:
    virtual ~BirthSink() = default;
    virtual BirthVerdict AuthorBirth(const coop::net::ContainerExtractIntentPayload& p,
                                     uint8_t senderSlot) = 0;
};

// ---- timing constants (the v2 window values, unchanged) ---------------------------------------
inline constexpr uint64_t kPairTtlMs            = 10000;  // parked/applied pairing window
inline constexpr uint64_t kBirthRetryIntervalMs = 500;    // post-accept birth retry cadence
inline constexpr uint64_t kBirthRetryTtlMs      = 30000;  // bounded birth recovery, then kBirthFailed
inline constexpr size_t   kCommittedMemory      = 64;     // exactly-once memory (key -> ms)

// ---- state records ---------------------------------------------------------------------------

struct ParkedBirth {                                    // intent arrived, mutation not yet judged
    uint32_t eid = 0;
    coop::net::ContainerExtractIntentPayload payload{};
    uint64_t arrivedMs = 0;
};
struct AppliedMarker {                                  // mutation Applied, intent not yet arrived
    uint32_t eid = 0;
    uint64_t appliedMs = 0;
};
struct BirthRetry {                                     // mutation Applied, birth pending (blocker M)
    uint32_t eid = 0;
    coop::net::ContainerExtractIntentPayload payload{};
    uint64_t firstAttemptMs = 0;
    uint64_t nextAttemptMs  = 0;
    uint32_t attempts       = 0;
};

// ---- intent admission outcomes (the adapter logs them; tests assert them) ---------------------
enum class ParkResult : uint8_t {
    Parked             = 0,   // intent-first: parked, awaiting the paired mutation
    CommittedNow       = 1,   // write-first: the mutation already Applied -- birth authored this call
    BirthPending       = 2,   // write-first: committed direction, birth moved to the bounded retry
    DuplicateCommitted = 3,   // this exact key already birthed (its verdict already went out)
    ZeroToken          = 4,   // token 0 is never valid
    SlotEidMismatch    = 5   // the applied marker names a different container -- pairing refused
};

// ---- the ledger -------------------------------------------------------------------------------

class Ledger {
public:
    // INTENT ARRIVAL (ContainerExtractIntent). Idempotent per key; a duplicate intent for a
    // still-parked key just refreshes the parked entry (same metadata, same birth-once).
    ParkResult ParkIntent(const ExtractKey& key, uint32_t eid,
                          const coop::net::ContainerExtractIntentPayload& p, uint64_t nowMs,
                          ResultSink& rs, BirthSink& bs);

    struct WriteReport {
        size_t committed      = 0;   // births authored this call (kOk ordered per each)
        size_t birthsPending  = 0;   // commits moved to the bounded birth retry
        size_t markedApplied  = 0;   // write-first order: markers awaiting their intent
        size_t duplicates     = 0;   // already-committed keys in this write (no second birth)
        size_t mismatches     = 0;   // parked eid != write eid -- pairing dropped
    };

    // THE PAIRED WRITE RETURNED APPLIED from the host CAS. For each token key: commit (birth now,
    // once) or -- no intent yet -- remember the marker so the intent can commit on arrival.
    WriteReport OnWriteApplied(const std::vector<ExtractKey>& keys, uint32_t eid, uint64_t nowMs,
                               ResultSink& rs, BirthSink& bs);

    // THE PAIRED WRITE WAS REFUSED by the host CAS. No birth, ever; every token key is answered
    // kRefused and ALL of its pairing state dies (the item stays in the host's container).
    void OnWriteRefused(const std::vector<ExtractKey>& keys, ResultSink& rs);

    struct SweepReport {
        size_t expiredParked  = 0;   // pairing window lapsed -> kExpired ordered per each
        size_t expiredApplied = 0;   // accepted write never got its intent (item unbacked -- loud)
        size_t birthRetries   = 0;   // birth re-attempts made this sweep
        size_t birthsCommitted = 0;  // retries that succeeded (kOk ordered)
        size_t birthsFailed   = 0;   // bounded window lapsed -> kBirthFailed ordered (item lost)
    };
    // Periodic sweep: pairing TTLs + the bounded birth-retry pump.
    SweepReport Sweep(uint64_t nowMs, ResultSink& rs, BirthSink& bs);

    // A SLOT'S OCCUPANT LEFT (blocker L): all of its pending pairing state dies with its
    // occupancy. The next occupant (a new generation) starts clean.
    void OnDisconnectForSlot(uint8_t slot);

    // Session teardown.
    void Clear();

    // -- views (instruments + tests) --
    bool   IsCommitted(const ExtractKey& k) const { return committed_.count(k) != 0; }
    // ANY live pairing state for this key (parked / applied marker / birth retry)? A key that
    // is neither pending nor in the committed memory is TERMINAL: its verdict already went out.
    bool   IsPending(const ExtractKey& k) const {
        return parked_.count(k) != 0 || applied_.count(k) != 0 || birthRetry_.count(k) != 0;
    }
    size_t ParkedCount()     const { return parked_.size(); }
    size_t AppliedCount()    const { return applied_.size(); }
    size_t BirthRetryCount() const { return birthRetry_.size(); }
    size_t CommittedCount()  const { return committed_.size(); }

private:
    // The shared once-only commit machinery. Returns true when the birth reached a terminal
    // SUCCESS this call (kOk ordered); false when it moved to the bounded birth retry.
    bool CommitNow(const ExtractKey& key, uint32_t eid,
                   const coop::net::ContainerExtractIntentPayload& payload, uint64_t nowMs,
                   ResultSink& rs, BirthSink& bs);

    void CommitRemember(const ExtractKey& key, uint64_t nowMs);   // once-only memory + eviction

    std::map<ExtractKey, ParkedBirth>   parked_;
    std::map<ExtractKey, AppliedMarker> applied_;
    std::map<ExtractKey, BirthRetry>    birthRetry_;
    std::map<ExtractKey, uint64_t>      committed_;
    std::deque<ExtractKey>              committedOrder_;          // eviction order for committed_

    // TEST INSTRUMENTATION (test-contract pass): the UNBACKED-WRITE loss signature. When an
    // APPLIED marker dies without its intent (the pairing window lapsed, or the extractor
    // disconnected), the paired item is in NEITHER place -- the one lossy terminal the pairing
    // design cannot recover from by itself (the client is gone or will never send the intent).
    // Recorded here so the model/fuzz suites can DISTINGUISH this documented policy loss from
    // an unknown invariant violation instead of hiding it behind a log line.
    std::vector<ExtractKey> unbackedLost_;

public:
    // True when this key's applied marker expired/purged without ever pairing (loss signature).
    bool WasUnbacked(const ExtractKey& k) const {
        for (const ExtractKey& l : unbackedLost_) if (l == k) return true;
        return false;
    }
    size_t UnbackedLostTotal() const { return unbackedLost_.size(); }

    // Test view: the live key tuples per state bucket (identity-level debugging in the suites).
    struct KeyBuckets {
        std::vector<ExtractKey> parked, applied, retry, committed;
    };
    KeyBuckets Buckets() const {
        KeyBuckets b;
        for (const auto& kv : parked_)      b.parked.push_back(kv.first);
        for (const auto& kv : applied_)     b.applied.push_back(kv.first);
        for (const auto& kv : birthRetry_)  b.retry.push_back(kv.first);
        for (const auto& kv : committed_)   b.committed.push_back(kv.first);
        return b;
    }
};

// ---- implementation (header-only: the test TU must run the real bodies) -----------------------

inline ParkResult Ledger::ParkIntent(const ExtractKey& key, uint32_t eid,
                                     const coop::net::ContainerExtractIntentPayload& p,
                                     uint64_t nowMs, ResultSink& rs, BirthSink& bs) {
    if (key.token == 0) return ParkResult::ZeroToken;
    // Exactly-once: a committed key can never birth again, whatever arrives (its kOk already
    // went out with the commit; retransmit-safety for THAT verdict is the backlog's job).
    if (committed_.count(key)) return ParkResult::DuplicateCommitted;
    // Write-first order? The mutation already Applied for this key -- commit NOW.
    auto ait = applied_.find(key);
    if (ait != applied_.end()) {
        if (ait->second.eid != eid) {
            applied_.erase(ait);
            return ParkResult::SlotEidMismatch;
        }
        applied_.erase(ait);
        return CommitNow(key, eid, p, nowMs, rs, bs) ? ParkResult::CommittedNow
                                                     : ParkResult::BirthPending;
    }
    // Intent-first (the common case): park it. A retransmitted intent re-parks (same metadata).
    parked_[key] = ParkedBirth{eid, p, nowMs};
    return ParkResult::Parked;
}

inline bool Ledger::CommitNow(const ExtractKey& key, uint32_t eid,
                              const coop::net::ContainerExtractIntentPayload& payload,
                              uint64_t nowMs, ResultSink& rs, BirthSink& bs) {
    const BirthVerdict verdict = bs.AuthorBirth(payload, key.senderSlot);
    if (verdict != BirthVerdict::TransientFailure) {
        // SUCCESS (spawned now, or the exact key already IS the authoritative birth): terminal.
        CommitRemember(key, nowMs);
        coop::net::ContainerExtractResultPayload r{};
        r.extractToken = key.token;
        r.accepted     = 1;
        r.reason       = coop::net::container_extract_result::kOk;
        rs.SendExtractResult(key.senderSlot, r);
        (void)eid;
        return true;
    }
    // BLOCKER M: the mutation was ALREADY accepted -- never silently lose the item. Park the
    // birth for the bounded retry (NOT committed: a duplicate write finding nothing here would
    // still be able to trigger the birth via its own pairing path).
    BirthRetry br;
    br.eid = eid;
    br.payload = payload;
    br.firstAttemptMs = nowMs;
    br.nextAttemptMs  = nowMs + kBirthRetryIntervalMs;
    br.attempts       = 1;
    birthRetry_[key] = br;
    return false;
}

inline void Ledger::CommitRemember(const ExtractKey& key, uint64_t nowMs) {
    committed_[key] = nowMs;
    committedOrder_.push_back(key);
    while (committedOrder_.size() > kCommittedMemory) {
        committed_.erase(committedOrder_.front());
        committedOrder_.pop_front();
    }
}

inline Ledger::WriteReport Ledger::OnWriteApplied(const std::vector<ExtractKey>& keys,
                                                  uint32_t eid, uint64_t nowMs,
                                                  ResultSink& rs, BirthSink& bs) {
    WriteReport rep;
    for (const ExtractKey& key : keys) {
        if (committed_.count(key)) { ++rep.duplicates; continue; }   // one birth per key, ever
        auto pit = parked_.find(key);
        if (pit == parked_.end()) {
            // Write-first order: remember the acceptance so the intent can commit on arrival.
            applied_[key] = AppliedMarker{eid, nowMs};
            ++rep.markedApplied;
            continue;
        }
        if (pit->second.eid != eid) {
            // The token's parked intent names a DIFFERENT container: an honest client cannot
            // produce this (tokens are per-client monotonic). Drop the pairing, fail closed.
            parked_.erase(pit);
            ++rep.mismatches;
            continue;
        }
        const coop::net::ContainerExtractIntentPayload payload = pit->second.payload;
        parked_.erase(pit);
        if (CommitNow(key, eid, payload, nowMs, rs, bs)) ++rep.committed;
        else                                            ++rep.birthsPending;
    }
    return rep;
}

inline void Ledger::OnWriteRefused(const std::vector<ExtractKey>& keys, ResultSink& rs) {
    for (const ExtractKey& key : keys) {
        // FUZZ FINDING (test-contract pass, seed 764236619501 / iteration 334): a refused COPY
        // of a write cannot cancel a pairing whose mutation was ALREADY ACCEPTED. The sequence
        // "identical write delivered twice -> first copy applies (marker/commit/retry), second
        // copy loses the CAS" erased the live marker and answered kRefused -- the client retired
        // its ghost while the item had already left the container: an unbacked loss with no
        // signature. The refusal is only meaningful for a PARKED intent (its mutation never
        // applied). An applied marker, a bounded birth retry, and a committed key all prove the
        // mutation was ACCEPTED earlier -- they survive a refused duplicate untouched.
        if (applied_.count(key) || birthRetry_.count(key) || committed_.count(key)) continue;
        parked_.erase(key);
        applied_.erase(key);
        coop::net::ContainerExtractResultPayload r{};
        r.extractToken = key.token;
        r.accepted     = 0;
        r.reason       = coop::net::container_extract_result::kRefused;
        rs.SendExtractResult(key.senderSlot, r);
    }
}

inline Ledger::SweepReport Ledger::Sweep(uint64_t nowMs, ResultSink& rs, BirthSink& bs) {
    SweepReport rep;
    for (auto it = parked_.begin(); it != parked_.end();) {
        if (nowMs - it->second.arrivedMs <= kPairTtlMs) { ++it; continue; }
        coop::net::ContainerExtractResultPayload r{};
        r.extractToken = it->first.token;
        r.accepted     = 0;
        r.reason       = coop::net::container_extract_result::kExpired;
        rs.SendExtractResult(it->first.senderSlot, r);
        it = parked_.erase(it);
        ++rep.expiredParked;
    }
    for (auto it = applied_.begin(); it != applied_.end();) {
        if (nowMs - it->second.appliedMs <= kPairTtlMs) { ++it; continue; }
        unbackedLost_.push_back(it->first);   // TEST INSTRUMENTATION: the write applied, the
        it = applied_.erase(it);              // intent never came -- the item is unbacked (loud)
        ++rep.expiredApplied;
    }
    for (auto it = birthRetry_.begin(); it != birthRetry_.end();) {
        const BirthRetry& br = it->second;
        if (nowMs - br.firstAttemptMs > kBirthRetryTtlMs) {
            // BLOCKER M terminal failure: the mutation was accepted but the authoritative birth
            // could not be established within the bounded window. The item is in neither place;
            // every peer is told loudly (the client retires its ghost on any accepted=0).
            coop::net::ContainerExtractResultPayload r{};
            r.extractToken = it->first.token;
            r.accepted     = 0;
            r.reason       = coop::net::container_extract_result::kBirthFailed;
            rs.SendExtractResult(it->first.senderSlot, r);
            it = birthRetry_.erase(it);
            ++rep.birthsFailed;
            continue;
        }
        if (nowMs < br.nextAttemptMs) { ++it; continue; }
        ++rep.birthRetries;
        const ExtractKey key = it->first;
        const BirthRetry pending = br;
        if (bs.AuthorBirth(pending.payload, key.senderSlot) != BirthVerdict::TransientFailure) {
            CommitRemember(key, nowMs);
            coop::net::ContainerExtractResultPayload r{};
            r.extractToken = key.token;
            r.accepted     = 1;
            r.reason       = coop::net::container_extract_result::kOk;
            rs.SendExtractResult(key.senderSlot, r);
            it = birthRetry_.erase(it);
            ++rep.birthsCommitted;
        } else {
            it->second.attempts = pending.attempts + 1;
            it->second.nextAttemptMs = nowMs + kBirthRetryIntervalMs;
            ++it;
        }
    }
    return rep;
}

inline void Ledger::OnDisconnectForSlot(uint8_t slot) {
    const auto matches = [slot](const ExtractKey& k) { return k.senderSlot == slot; };
    for (auto it = parked_.begin(); it != parked_.end();)  it = matches(it->first) ? parked_.erase(it) : std::next(it);
    for (auto it = applied_.begin(); it != applied_.end();) {
        if (matches(it->first)) {
            unbackedLost_.push_back(it->first);   // TEST INSTRUMENTATION: applied + leaver = unbacked
            it = applied_.erase(it);
        } else {
            it = std::next(it);
        }
    }
    for (auto it = birthRetry_.begin(); it != birthRetry_.end();) it = matches(it->first) ? birthRetry_.erase(it) : std::next(it);
}

inline void Ledger::Clear() {
    parked_.clear();
    applied_.clear();
    birthRetry_.clear();
    committed_.clear();
    committedOrder_.clear();
    unbackedLost_.clear();
}

}  // namespace coop::props::extract_pairing
