// coop/dev/director/ctake_barrier.h -- ACTUATOR-ONLY pass (T3): the CTAKE race's PURE BOTH_READY
// barrier.
//
// The old peer orchestration ran ONE 60s GO-timeout from the peer's OWN ARRIVED: "HOST ARRIVED"
// alone started the clock, and a slow client let the host expire WITHOUT a GO. The orchestrator
// additionally tested readiness by marker PRESENCE, so a stale ARRIVED from a previous session
// instance could satisfy BOTH_READY. This barrier is the shipped semantics both sides now share:
//   - a single arrival starts NO GO clock (the barrier only waits);
//   - BOTH_READY requires BOTH roles' LATEST arrivals under ONE shared, still-valid generation;
//   - only BOTH_READY starts the GO countdown (goDelayMs, measured from the BOTH_READY tick);
//   - a generation change (disconnect / rejoin / new attempt) withdraws readiness proven under
//     it -- both roles re-prove under the current shared generation; stale readiness can never
//     pair with fresh;
//   - the BOTH-peers wait expires into an explicit terminal TimedOut that can never fire, and a
//     late arrival cannot resurrect it;
//   - after GO the race is underway: a later invalidation is recorded but does not rewind Go.
// Pure: no I/O, no threads, no clock of its own. L1-tested
// (tests/batch1_corrective_selftest.cpp, section T3).

#pragma once

#include <cstdint>

namespace coop::director::ctake_barrier {

enum class Role { Host, Client };
enum class Phase { Waiting, BothReady, Go, TimedOut };
enum class Event { None, BothReady, Go, TimedOut };

struct Decision {
    Phase phase   = Phase::Waiting;
    bool  mayFire = false;    // true ONLY in Go -- the barrier's one and only "take now" output
    Event event   = Event::None;
};

class Gate {
public:
    // One peer proved readiness: it ARRIVED at the shared container under `generation` (the
    // orchestrator's per-attempt occupancy/session identity). Re-observing a role REPLACES that
    // role's readiness -- the latest arrival wins, never an older one.
    void ObserveArrival(Role r, uint64_t generation) {
        if (phase_ == Phase::TimedOut) return;   // terminal: a late arrival cannot resurrect
        arrived_[Idx(r)] = true;
        gen_[Idx(r)]     = generation;
        seenAny_         = true;
    }

    // The shared generation ended (disconnect / rejoin / new attempt). Readiness proven under
    // this generation -- and under any older one -- is withdrawn; both roles must re-prove
    // under the CURRENT shared generation before BOTH_READY can pair again.
    void InvalidateGeneration(uint64_t generation) {
        if (generation > invalidGen_) invalidGen_ = generation;
    }

    Decision Tick(uint64_t nowMs, uint64_t goDelayMs, uint64_t bothWaitMs) {
        if (phase_ == Phase::TimedOut) return {Phase::TimedOut, false, Event::None};
        if (phase_ == Phase::Go)       return {Phase::Go, true, Event::None};   // underway: no rewind

        const bool paired = Ready(0) && Ready(1) && gen_[0] == gen_[1];

        if (phase_ == Phase::BothReady) {
            if (!paired) {                       // readiness withdrawn under us: back to waiting
                phase_ = Phase::Waiting;
                waitAnchorMs_ = nowMs;           // the BOTH wait restarts from the withdrawal
                return {Phase::Waiting, false, Event::None};
            }
            if (nowMs - goAnchorMs_ >= goDelayMs) {
                phase_ = Phase::Go;
                return {Phase::Go, true, Event::Go};
            }
            return {Phase::BothReady, false, Event::None};
        }

        // Waiting: BOTH_READY the moment both roles are proven under ONE shared generation.
        if (paired) {
            phase_ = Phase::BothReady;
            goAnchorMs_ = nowMs;                 // the GO countdown runs ONLY from here
            return {Phase::BothReady, false, Event::BothReady};
        }
        if (seenAny_) {                          // the wait anchors at the FIRST arrival of either
            if (waitAnchorMs_ == kNoAnchor) waitAnchorMs_ = nowMs;
            if (nowMs - waitAnchorMs_ > bothWaitMs) {
                phase_ = Phase::TimedOut;        // explicit terminal timeout -- the orchestration
                return {Phase::TimedOut, false, Event::TimedOut};   // layer names the missing peer
            }
        }
        return {Phase::Waiting, false, Event::None};
    }

    Phase phase() const { return phase_; }

private:
    static constexpr uint64_t kNoAnchor = ~uint64_t{0};

    static size_t Idx(Role r) { return r == Role::Host ? 0u : 1u; }
    bool Ready(size_t i) const { return arrived_[i] && gen_[i] != 0 && gen_[i] > invalidGen_; }

    Phase    phase_{Phase::Waiting};
    bool     arrived_[2]{false, false};
    uint64_t gen_[2]{0, 0};
    uint64_t invalidGen_   = 0;      // monotone: every generation <= this is dead
    bool     seenAny_      = false;
    uint64_t waitAnchorMs_ = kNoAnchor;
    uint64_t goAnchorMs_   = 0;
};

}  // namespace coop::director::ctake_barrier
