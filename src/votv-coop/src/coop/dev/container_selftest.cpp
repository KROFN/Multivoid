// coop/dev/container_selftest.cpp -- see coop/dev/container_selftest.h.

#include "coop/dev/container_selftest.h"

#include "coop/config/config.h"
#include "coop/dev/extract_convergence.h"   // ACTUATOR-ONLY pass: the pure convergence gate
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"


#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::dev::container_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace CC = coop::props::container_contents_sync;
namespace EC = coop::dev::extract_convergence;   // ACTUATOR-ONLY pass
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::container_selftest);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

// The schedule, measured from the first tick at which this peer is connected. The HOST keeps a
// fixed fire time (it authors from its own state; its word IS the base). The CLIENT no longer
// fires on a timer at all -- see the convergence gate below (ACTUATOR-ONLY pass).
constexpr uint64_t kHostFireMs   = 10000;
constexpr uint64_t kDigestEveryMs = 5000;

uint64_t g_connectedAtMs = 0;
uint64_t g_nextDigestMs  = 0;
bool g_fired = false;

// ACTUATOR-ONLY pass: the CLIENT extract no longer rides a fixed +25s timer. Node2 (protocol
// 2133, run batch1-2133-runtime-20260831T001318Z) showed that timer fires BEFORE the client has
// consumed the host's published baseline for the target container (author base=0 vs the
// host-published hash) and the host CAS correctly REFUSES it (STALE BASE) -- the CAS was working
// as designed; the ORCHESTRATION was early. The fire now waits for PROVEN baseline convergence
// (consumed base != 0 AND local ContentHash == that base) through the L1-tested pure gate,
// fed by the READ-ONLY CC::DevBaselineReadout (no write path -> convergence cannot be
// manufactured). A host refusal AFTER proven convergence is REAL regression evidence and is
// reported, not bypassed.
EC::Gate g_conv;
bool g_convWaitLogged     = false;
bool g_convTimedOutLogged = false;

// The two containers under test, chosen ONCE and by the same rule on both peers: the eids are
// cross-peer stable, so peer A's pick and peer B's pick name the same actors. Index 0 is the
// host's target, index 1 the client's -- deliberately DIFFERENT containers, so the two halves of
// the circle cannot mask each other (one lane working both ways would look identical to two lanes
// each working one way if both fired at the same container).
uint32_t g_eidHostTarget   = 0;
uint32_t g_eidClientTarget = 0;

void* g_extractFn = nullptr;

// Pick only containers that actually HOLD something. An empty container cannot be extracted from,
// and the first pass of this instrument proved why that matters: it fired on two empty containers,
// changed nothing, and would have reported an absent "callback ENTERED" as a lane failure if the
// inertness guard below had not caught it. Choosing by CONTENT rather than by registry order is
// what makes the absence of a log line meaningful.
constexpr size_t kScan = 64;

bool ResolveTargets() {
    if (g_eidHostTarget && g_eidClientTarget) return true;
    CC::WorldContainer picks[kScan]{};
    const size_t n = CC::SnapshotWorldContainers(picks, kScan);
    for (size_t i = 0; i < n; ++i) {
        int32_t cnt = -1; float vol = 0.f;
        if (!CC::ContentsDigest(picks[i].eid, cnt, vol) || cnt < 1) continue;
        if (!g_eidHostTarget)                                  g_eidHostTarget = picks[i].eid;
        else if (picks[i].eid != g_eidHostTarget && !g_eidClientTarget)
                                                               g_eidClientTarget = picks[i].eid;
        if (g_eidHostTarget && g_eidClientTarget) break;
    }
    if (!g_eidHostTarget || !g_eidClientTarget) return false;   // keep looking; the world may still be filling
    UE_LOGI("container_selftest: targets chosen (non-empty only) -- host extracts from eid=%u, "
            "client from eid=%u (scanned %zu world containers)",
            g_eidHostTarget, g_eidClientTarget, n);
    return true;
}

// Dispatch the ORGANIC mutation: `Aprop_container_C::extract(int32 Index)`, which is the very verb
// a player's UI extraction runs. `extract` calls `takeObj` through EX_LocalVirtualFunction
// (bytecode census, RE §5) -- so the call WE make is the outer one and the mutation the lane must
// catch is the game's own inner dispatch. Calling `takeObj` ourselves would arrive by ProcessEvent,
// i.e. through the one path the lane does NOT rely on, and would prove nothing
// ([[feedback-probe-must-count-not-confirm]]: never resolve through the mechanism under test).
//
// It spawns the extracted prop into the world, exactly as a real extraction does. That is dev
// litter in a throwaway smoke world and is the price of an HONEST trigger.
void FireExtract(uint32_t eid, const char* who) {
    CC::WorldContainer picks[kScan]{};
    const size_t n = CC::SnapshotWorldContainers(picks, kScan);
    void* actor = nullptr;
    for (size_t i = 0; i < n; ++i) {
        if (picks[i].eid == eid) { actor = picks[i].actor; break; }
    }
    if (!actor) {
        UE_LOGW("container_selftest: %s target eid=%u no longer resolves -- not fired", who, eid);
        return;
    }
    if (!g_extractFn) {
        // Resolve from the DECLARING class: FindFunction matches Outer == class EXACTLY and does
        // not walk the chain (reflection.cpp:427), so ClassOf(actor) -- always a subclass here --
        // would silently yield nullptr. That is the same bug this build fixes in the lane itself.
        void* cls = R::FindClass(L"prop_container_C");
        g_extractFn = cls ? R::FindFunction(cls, L"extract") : nullptr;
        if (!g_extractFn) {
            UE_LOGW("container_selftest: prop_container_C::extract did not resolve -- INERT");
            return;
        }
    }
    int32_t before = -1; float volBefore = 0.f;
    CC::ContentsDigest(eid, before, volBefore);
    struct { int32_t Index; } params{0};   // always the first record
    R::CallFunction(actor, g_extractFn, &params);
    int32_t after = -1; float volAfter = 0.f;
    CC::ContentsDigest(eid, after, volAfter);
    UE_LOGI("container_selftest: %s FIRED extract(0) on eid=%u -- records %d -> %d, "
            "currVol %.1f -> %.1f", who, eid, before, after, volBefore, volAfter);
    if (before == after) {
        UE_LOGW("container_selftest: %s extract changed NOTHING on eid=%u -- the TRIGGER is inert, "
                "so an absent 'callback ENTERED' line says nothing about the lane. Fix the trigger "
                "before reading any verdict from this run.", who, eid);
    }
}

void Digest() {
    for (uint32_t eid : {g_eidHostTarget, g_eidClientTarget}) {
        if (!eid) continue;
        int32_t n = -1; float vol = 0.f;
        if (CC::ContentsDigest(eid, n, vol)) {
            UE_LOGI("container_selftest: DIGEST eid=%u records=%d currVol=%.1f", eid, n, vol);
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    const uint64_t now = NowMs();
    if (!g_connectedAtMs) {
        g_connectedAtMs = now;
        g_nextDigestMs = now + kDigestEveryMs;
        UE_LOGI("container_selftest: ARMED (host fires at +%llums; client fires ONLY on proven "
                "baseline convergence -- extract_convergence -- never on a timer)",
                static_cast<unsigned long long>(kHostFireMs));
    }
    if (!ResolveTargets()) return;

    const bool host = s->role() == coop::net::Role::Host;
    if (!g_fired && host && now - g_connectedAtMs >= kHostFireMs) {
        g_fired = true;
        FireExtract(g_eidHostTarget, "HOST");
    }
    if (!g_fired && !host) {
        // ACTUATOR-ONLY pass: wait for PROVEN convergence on the CLIENT target eid, then fire
        // the ONE real client extract. A failed readout resolve feeds base=0 (never fire --
        // fail closed). NOT-READY / TIMEOUT are logged on state change; the CONVERGENCE line is
        // the fire line.
        uint64_t base = 0, local = 0;
        const bool have = CC::DevBaselineReadout(g_eidClientTarget, base, local);
        const auto d = g_conv.Tick(now, EC::Observation{have ? base : 0, have ? local : 0});
        if (d.action == EC::Action::Fire) {
            g_fired = true;
            UE_LOGI("container_selftest: CONVERGENCE eid=%u base=%016llx local=%016llx -- proven "
                    "baseline convergence; firing the ONE real client extract (the host CAS now "
                    "judges it on its merits)", g_eidClientTarget,
                    static_cast<unsigned long long>(base), static_cast<unsigned long long>(local));
            FireExtract(g_eidClientTarget, "CLIENT");
        } else if (d.phase == EC::Phase::TimedOut) {
            if (!g_convTimedOutLogged) {
                g_convTimedOutLogged = true;
                UE_LOGW("container_selftest: TIMEOUT eid=%u base=%016llx local=%016llx -- the "
                        "baseline wait expired terminal NOT-FIRED (host truth never converged with "
                        "the local slice); production state untouched", g_eidClientTarget,
                        static_cast<unsigned long long>(base), static_cast<unsigned long long>(local));
            }
        } else if (!g_convWaitLogged) {
            g_convWaitLogged = true;
            UE_LOGI("container_selftest: NOT-READY eid=%u base=%016llx local=%016llx -- baseline "
                    "not converged yet (base=0 = host truth not consumed; base!=local = drift); "
                    "the extract stays unfired", g_eidClientTarget,
                    static_cast<unsigned long long>(base), static_cast<unsigned long long>(local));
        }
    }
    if (now >= g_nextDigestMs) {
        g_nextDigestMs = now + kDigestEveryMs;
        Digest();
    }
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_connectedAtMs = 0;
    g_nextDigestMs = 0;
    g_fired = false;
    g_eidHostTarget = g_eidClientTarget = 0;
    g_conv = EC::Gate{};   // ACTUATOR-ONLY pass: the convergence wait dies with the session
    g_convWaitLogged = g_convTimedOutLogged = false;
}

}  // namespace coop::dev::container_selftest
