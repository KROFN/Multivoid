// coop/net/send_backlog.h -- KROFNE FORK (2133): the bounded retry/backlog for the fork's
// CRITICAL reliable sends (v3 corrective pass, blocker N).
//
// THE BUG CLASS: Session::SendReliable / SendReliableToSlot return false when the transport
// refuses to queue the message (payload too large, no connected peers, GNS backpressure). v2
// ignored that bool for all four fork-critical kinds. For ContainerExtractIntent the failure
// sequence was: intent send fails -> the paired ContainerContents write later arrives and
// applies -> the host removes the item from its container -> NO birth intent ever exists ->
// authoritative item loss. For DroneActionRequest it meant a pending take living for seconds
// (amplifying blocker O); for both RESULT kinds a client could hold a permanent ghost or a
// pending request merely because ONE GNS queue attempt was refused.
//
// THE FIX (Batch-1-local; NOT the Batch-2 reliability work): a small bounded FIFO backlog.
// SendCritical tries the transport ONCE; a refusal parks the EXACT BYTES for scheduled retry,
// so the same semantic request/token/nonce rides every attempt (no new nonce/token per retry),
// in FIFO order (ordering preserved per destination), with host idempotency making the
// retransmit safe (drone replay domains; the pairing ledger's once-only commits). The queue is
// bounded (64 entries) and every entry expires (10 s) -- expiry is counted, never silent.
// SendReliable returning false is NEVER treated as successfully shipped.
//
// ELIGIBLE KINDS ONLY: DroneActionRequest / DroneActionResult / ContainerExtractIntent /
// ContainerExtractResult. Everything else is refused by SendCritical (this is not a general
// transport; the stock lanes have their own retry arming).
//
// GENERATION BINDING (test-contract pass, review finding S): a parked entry addressed to a
// SLOT survives that peer's DISCONNECT -- GNS drops the connection's in-flight queue, but OUR
// backlog is ours. Without binding, a verdict/intent parked for slot 1 (occupant A, generation
// 10) is delivered to slot 1's SUCCESSOR (occupant B, generation 11) -- a verdict for a request
// B never made. The core therefore (a) STAMPS every slot-addressed entry with the destination's
// occupancy generation at Park time (Session::peerGenerationForSlot) and (b) refuses to deliver
// it once the slot's generation CHANGED (counted staleDropped, never delivered). A peer-left
// event additionally PURGES that slot's entries outright (OnDisconnectForSlot). Host-directed
// entries (toSlot < 0, i.e. client -> host) are bound to the session itself and need no stamp.
//
// The pure core (BacklogCore) is engine-free and headless-testable: the corrective selftest
// scripts a sink whose first N sends fail and whose next send succeeds, and asserts the same
// bytes come back, in order, with the refusals never lost or duplicated; the model suite
// (batch1_model_*.cpp) proves the generation binding at system level.

#pragma once

#include "coop/net/protocol.h"   // ReliableKind (pure POD header)

#include <cstddef>
#include <cstdint>
#include <deque>

namespace coop::net {
class Session;
namespace send_backlog {

// ---- tuning constants -------------------------------------------------------------------------
// The biggest fork-critical payload is ContainerExtractIntentPayload at 188 B.
inline constexpr size_t   kMaxEntryBytes     = 192;
inline constexpr size_t   kMaxBacklogEntries = 64;      // bounded: a dead destination cannot balloon it
inline constexpr uint64_t kRetryIntervalMs   = 250;      // per-entry retry cadence
inline constexpr uint64_t kEntryTtlMs        = 10000;    // expiry: matches the pairing window

// ---- the pure core ----------------------------------------------------------------------------
class BacklogCore {
public:
    struct Entry {
        ReliableKind kind      = static_cast<ReliableKind>(0);
        int          toSlot    = -1;            // -1 = SendReliable (broadcast/host-directed)
        uint8_t      len       = 0;
        uint8_t      data[kMaxEntryBytes] = {};
        uint64_t     enqueuedMs    = 0;
        uint64_t     nextAttemptMs = 0;
        uint32_t     attempts      = 0;
        uint32_t     destGeneration = 0;         // S: the destination's occupancy generation at Park
                                                  // (meaningful only when genStampValid)
        bool         genStampValid  = false;    // S: a resolver existed at Park for a slot address
    };

    enum class EnqueueResult : uint8_t { Queued = 0, Parked = 1, DroppedFull = 2, DroppedOversize = 3 };

    struct DrainReport {
        size_t sent    = 0;   // entries the sink accepted (retired)
        size_t retried = 0;   // entries the sink refused (rescheduled)
        size_t expired = 0;   // entries past the TTL (dropped, counted -- never silent)
        size_t stale   = 0;   // entries whose destination generation CHANGED (dropped, counted;
                              // review finding S: never deliver across an occupancy change)
    };

    // The transport seam. Returns TRUE = the message was queued (shipped); FALSE = refused
    // (the entry stays, rescheduled -- never dropped by a refusal alone).
    class ISendSink {
    public:
        virtual ~ISendSink() = default;
        virtual bool Send(ReliableKind kind, int toSlot, const uint8_t* data, size_t len) = 0;
    };

    // The destination-generation authority (review finding S). Production wires
    // Session::peerGenerationForSlot; tests script a table. 0 = the slot is empty/unknown.
    class IGenerationResolver {
    public:
        virtual ~IGenerationResolver() = default;
        virtual uint32_t GenerationForSlot(int slot) = 0;
    };
    void SetGenerationResolver(IGenerationResolver* r) { gen_ = r; }

    explicit BacklogCore(uint64_t nowMs = 0) : nowMs_(nowMs) {}

    void SetTime(uint64_t nowMs) { nowMs_ = nowMs; }

    // Try-now-or-park. The caller has ALREADY attempted the real send and it returned false
    // (SendCritical's shape), or wants deferred-first semantics; either way the entry parks
    // with its first retry scheduled one interval out.
    EnqueueResult Park(ReliableKind kind, int toSlot, const void* data, size_t len);

    // Drain every entry whose nextAttemptMs has come, FIFO. Accepted entries retire; refused
    // entries reschedule; entries past the TTL expire (counted). Bounded work per call.
    DrainReport Drain(ISendSink& sink);

    size_t Size() const { return q_.size(); }
    void   Clear()      { q_.clear(); }

    // Per-slot teardown (review finding S): drop every entry destined to `slot` -- its
    // occupancy ended, and nothing queued FOR that occupant may reach its successor. Returns
    // the number of purged entries (counted, never silent).
    size_t PurgeSlot(int slot) {
        size_t purged = 0;
        for (auto it = q_.begin(); it != q_.end();) {
            if (it->toSlot == slot) { it = q_.erase(it); ++purged; }
            else                    { ++it; }
        }
        return purged;
    }

private:
    std::deque<Entry>   q_;
    uint64_t            nowMs_ = 0;
    IGenerationResolver* gen_ = nullptr;
};

// The core is header-inline BY DESIGN: tests/batch1_corrective_selftest.cpp compiles it headless
// (the .cpp wrapper drags the transport in; the tested decisions must not).
inline BacklogCore::EnqueueResult BacklogCore::Park(ReliableKind kind, int toSlot,
                                                    const void* data, size_t len) {
    if (!data || len == 0 || len > kMaxEntryBytes) return EnqueueResult::DroppedOversize;
    if (q_.size() >= kMaxBacklogEntries) return EnqueueResult::DroppedFull;   // bounded: never balloons
    Entry e;
    e.kind = kind;
    e.toSlot = toSlot;
    e.len = static_cast<uint8_t>(len);
    std::memcpy(e.data, data, len);
    e.enqueuedMs = nowMs_;
    e.nextAttemptMs = nowMs_ + kRetryIntervalMs;
    e.attempts = 0;
    // Review finding S: bind slot-addressed entries to the destination's CURRENT occupancy
    // generation. With a resolver present, EVEN A 0 (empty slot) is a meaningful stamp: the
    // entry may only ship while the destination occupancy is UNCHANGED (an empty slot's entry
    // dies by the transport refusal + TTL, never by reaching a successor). Host-directed
    // entries (toSlot < 0) and resolver-less cores carry no stamp and skip the check.
    e.genStampValid = (toSlot >= 0 && gen_ != nullptr);
    e.destGeneration = e.genStampValid ? gen_->GenerationForSlot(toSlot) : 0;
    q_.push_back(e);
    return EnqueueResult::Parked;
}

inline BacklogCore::DrainReport BacklogCore::Drain(ISendSink& sink) {
    DrainReport rep;
    const size_t n = q_.size();
    for (size_t i = 0; i < n; ++i) {
        Entry e = q_.front();
        q_.pop_front();
        if (nowMs_ - e.enqueuedMs > kEntryTtlMs) {
            ++rep.expired;   // bounded lifetime: a destination that stays dead cannot hold bytes forever
            continue;
        }
        if (nowMs_ < e.nextAttemptMs) {
            q_.push_back(e);   // not due yet -- back it goes (FIFO order preserved)
            continue;
        }
        // Review finding S: the destination's occupancy moved since Park -- this entry can
        // never be delivered. Fail CLOSED (drop + count), never deliver across the change.
        // This includes entries parked for an EMPTY slot (stamped 0) once it is re-occupied.
        if (e.toSlot >= 0 && e.genStampValid &&
            (!gen_ || gen_->GenerationForSlot(e.toSlot) != e.destGeneration)) {
            ++rep.stale;
            continue;
        }
        ++e.attempts;
        if (sink.Send(e.kind, e.toSlot, e.data, e.len)) {
            ++rep.sent;        // accepted: the transport owns it now
        } else {
            e.nextAttemptMs = nowMs_ + kRetryIntervalMs;
            q_.push_back(e);   // refused: same bytes, same order, rescheduled
            ++rep.retried;
        }
    }
    return rep;
}

// ---- the production wrapper (game thread; the session fan-out drives Tick) --------------------

// The ONE way fork-critical traffic leaves a peer. Attempts the real send; on a false return
// parks the exact bytes for bounded retry. Returns TRUE only when the transport ACCEPTED the
// message (or the entry was already delivered by a previous attempt) -- a false transport
// return is NEVER reported as success.
bool SendCritical(coop::net::Session& session, ReliableKind kind, int toSlot,
                  const void* payload, size_t len);

// Per-net-pump-tick pump: primes the clock and drains due entries through the real transport.
// Near-free when the backlog is empty.
void Tick(coop::net::Session* session);

// Session teardown: the backlog dies with the session (nothing queued is meaningful after it).
void OnDisconnect();

// ONE PEER LEFT (review finding S): purge every entry destined to that slot. The successor of
// a reused slot must never receive the previous occupant's parked verdicts/intents. Wired from
// subsystems' per-slot teardown fanout, beside the other per-slot cleanups.
void OnDisconnectForSlot(int slot);

}  // namespace coop::net::send_backlog
}  // namespace coop::net
