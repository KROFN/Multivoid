// coop/net/send_backlog.cpp -- see coop/net/send_backlog.h (KROFNE FORK blocker N).

#include "coop/net/send_backlog.h"

#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <chrono>
#include <cstring>

namespace coop::net::send_backlog {
namespace {

bool KindEligible(ReliableKind kind) {
    switch (kind) {
    case ReliableKind::DroneActionRequest:
    case ReliableKind::DroneActionResult:
    case ReliableKind::ContainerExtractIntent:
    case ReliableKind::ContainerExtractResult:
        return true;
    default:
        return false;
    }
}

// The real transport. A false return means GNS refused to queue -- the exact condition the
// backlog exists to survive.
bool TransportSend(coop::net::Session* s, ReliableKind kind, int toSlot,
                   const void* data, size_t len) {
    if (!s || !s->connected()) return false;
    if (toSlot < 0) return s->SendReliable(kind, data, static_cast<int>(len));
    return s->SendReliableToSlot(toSlot, kind, data, static_cast<int>(len));
}

struct TransportSink : BacklogCore::ISendSink {
    coop::net::Session* session = nullptr;
    bool Send(ReliableKind kind, int toSlot, const uint8_t* data, size_t len) override {
        return TransportSend(session, kind, toSlot, data, len);
    }
};

// Game-thread-only module state (every caller site is a GT ingest/drain path). The clock is
// injected before EVERY interaction (Park/Drain) from steady_clock -- the same NowMs shape the
// other subsystem TUs use -- so entry schedules are always judged against current time.
BacklogCore  g_core{0};
uint64_t     g_droppedByTtl   = 0;
uint64_t     g_shippedOnRetry = 0;
uint64_t     g_droppedStale   = 0;   // S: entries dropped on an occupancy-generation change

// The generation authority (review finding S): entries parked for a SLOT are stamped with the
// destination's occupancy generation at Park time and refused at Drain time if it changed.
// Reads Session::peerGenerationForSlot -- the same host-minted token the drone replay domain
// and the extraction ledger bind to (one occupancy authority across the fork).
struct SessionGenResolver : BacklogCore::IGenerationResolver {
    coop::net::Session* session = nullptr;
    uint32_t GenerationForSlot(int slot) override {
        if (!session || slot < 0) return 0;
        return session->peerGenerationForSlot(slot);
    }
} g_resolver;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

// ---- the pure core lives INLINE in the header (send_backlog.h) so the headless corrective
// selftest compiles the exact tested bodies without dragging the transport in. This TU provides
// only the production wrapper below.

// ---- the production wrapper -------------------------------------------------------------------

bool SendCritical(coop::net::Session& session, ReliableKind kind, int toSlot,
                  const void* payload, size_t len) {
    if (!KindEligible(kind)) return false;   // not a backlog lane -- caller bug, refuse loudly-ish
    // The generation authority must reflect THIS session before any Park/Drain decision.
    g_resolver.session = &session;
    // FIRST attempt is the caller's actual send: if the transport queues it, done. One refusal
    // parks the exact bytes; nothing about the failure is treated as shipped.
    if (TransportSend(&session, kind, toSlot, payload, len)) return true;
    g_core.SetTime(NowMs());
    const BacklogCore::EnqueueResult r = g_core.Park(kind, toSlot, payload, len);
    if (r == BacklogCore::EnqueueResult::Parked) {
        UE_LOGW("send_backlog: kind=%u toSlot=%d refused by the transport -- PARKED for retry "
                "(%zu queued)", static_cast<unsigned>(kind), toSlot, g_core.Size());
        return false;
    }
    UE_LOGW("send_backlog: kind=%u toSlot=%d refused AND the backlog is %s -- message DROPPED "
            "(%s); the pairing/request TTL paths own the recovery",
            static_cast<unsigned>(kind), toSlot,
            r == BacklogCore::EnqueueResult::DroppedFull ? "FULL" : "ineligible for these bytes",
            r == BacklogCore::EnqueueResult::DroppedFull ? "bounded" : "oversize");
    return false;
}

void Tick(coop::net::Session* session) {
    if (g_core.Size() == 0) return;
    g_resolver.session = session;   // the generation check judges against the live session
    g_core.SetTime(NowMs());   // the retry cadence is judged against real time
    TransportSink sink;
    sink.session = session;
    const BacklogCore::DrainReport rep = g_core.Drain(sink);
    g_droppedByTtl += rep.expired;
    g_shippedOnRetry += rep.sent;
    g_droppedStale += rep.stale;
    if (rep.sent || rep.expired || rep.stale) {
        UE_LOGI("send_backlog: drained %zu shipped (%llu via retry), %zu expired (lifetime "
                "expired=%llu), %zu stale-generation dropped (lifetime=%llu), %zu still queued",
                rep.sent,
                static_cast<unsigned long long>(g_shippedOnRetry), rep.expired,
                static_cast<unsigned long long>(g_droppedByTtl), rep.stale,
                static_cast<unsigned long long>(g_droppedStale), g_core.Size());
    }
}

void OnDisconnect() {
    if (g_core.Size() != 0)
        UE_LOGW("send_backlog: session teardown dropped %zu parked message(s)", g_core.Size());
    g_core.Clear();
    g_resolver.session = nullptr;
}

void OnDisconnectForSlot(int slot) {
    if (slot < 0) return;
    g_resolver.session = nullptr;   // the slot's occupancy is over; the resolver is meaningless
    const size_t purged = g_core.PurgeSlot(slot);
    if (purged != 0)
        UE_LOGW("send_backlog: slot %d left -- purged %zu parked critical message(s) destined to "
                "them (the successor of a reused slot never receives the previous occupant's "
                "traffic)", slot, purged);
}

}  // namespace coop::net::send_backlog
