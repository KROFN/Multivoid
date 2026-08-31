// coop/dev/drone_take_gate.h -- ACTUATOR-ONLY pass (T1): the drone client-Take actuator's PURE
// decision half.
//
// The runtime half (coop/dev/drone_probe.cpp, ini drone_probe_take=1, CLIENT only) waits for the
// REAL delivery to land on the client mirror and then fires EXACTLY ONE faithful player Take:
// ue_wrap::drone::DispatchDropSack(mirror) -- the mirror drone's OWN dropSack UFunction via
// ProcessEvent, the very path the BP's take option runs -- so the shipped Batch-1 0x45 seam
// (drone_take_sync) authors the request, the host validates against its own drone, and the
// authoritative real sack comes from the host's own native dropSack. THIS header owns only the
// WHEN: never fire before the parked-with-cargo state, fire exactly once, never re-arm, never
// fire into an in-flight take, and a terminal wait timeout that cannot fire and mutates nothing
// (the FSM has no output that touches gameplay state -- a timeout is side-effect-free by
// construction, not by discipline).
//
// Pure: no engine types, no I/O, no clock of its own -- Tick() takes the caller's millisecond.
// L1-tested (tests/batch1_corrective_selftest.cpp, section T1).

#pragma once

#include <cstdint>

namespace coop::dev::drone_take_gate {

// Observable states of the actuator's decision half. Disabled = the ini switch is off (the
// default; a hard never-fire). Arming = waiting for the parked-with-cargo state (the NOT-READY
// diagnostics name this). Fired = the single fire, reported on the firing tick itself. Done =
// the fire happened and the gate is permanently inert (no re-arm). TimedOut = the delivery wait
// expired (the TIMEOUT diagnostic; terminal).
enum class Phase { Disabled, Arming, Fired, Done, TimedOut };

// The only output the gate has. Fire is produced at most ONCE per Gate lifetime; every other
// tick is None. There is deliberately no cancel/reset/force output: the actuator cannot mutate
// gameplay state through this FSM even by misuse.
enum class Action { None, Fire };

struct Decision {
    Action action = Action::None;
    Phase  phase  = Phase::Disabled;
};

// Readiness must HOLD this many CONSECUTIVE ticks before the one fire: the parked-with-cargo
// state must be stable, not a one-frame flicker between drone FSM states.
inline constexpr int kReadyHoldTicks = 5;

// The delivery wait is terminal: this long after the FIRST mirror sighting with no actionable
// state, the gate expires into TimedOut (NOT-FIRED) and never fires -- even if readiness arrives
// later. The clock arms only at the first sighting, so slow world loads do not eat the budget.
inline constexpr uint64_t kWaitTimeoutMs = 300000;   // 5 min

class Gate {
public:
    // One observation tick.
    //   enabled      the ini switch (drone_probe_take). false is a hard never-fire.
    //   dronePresent a live client mirror Adrone_C is resolved this tick.
    //   ready        the production gates a player's Take option needs, read through
    //                ue_wrap::drone::ReadGateFields on the mirror (canTakeOff && hasSack).
    //   pendingTake  a take request is already in flight (ours or a human's) -- observed through
    //                the mirror's own dropSack verb dispatch. Blocks the fire AND resets the hold.
    Decision Tick(uint64_t nowMs, bool enabled, bool dronePresent, bool ready, bool pendingTake) {
        if (!enabled) {                       // default OFF: a hard never-fire; hold discarded
            hold_ = 0;
            return {Action::None, Phase::Disabled};
        }
        if (fired_) {                         // exactly one fire, ever; permanently Done after it
            phase_ = Phase::Done;
            return {Action::None, Phase::Done};
        }
        if (phase_ == Phase::TimedOut)        // terminal: can never fire, ever
            return {Action::None, Phase::TimedOut};

        // The wait clock arms at the FIRST mirror sighting (slow world loads do not eat it).
        if (!armed_ && dronePresent) {
            armed_ = true;
            waitStartMs_ = nowMs;
        }
        if (armed_ && nowMs - waitStartMs_ > kWaitTimeoutMs) {
            phase_ = Phase::TimedOut;         // NOT-FIRED terminal; mutates nothing
            return {Action::None, Phase::TimedOut};
        }

        // The hold is CONSECUTIVE present+ready+not-pending ticks; any gap restarts it from zero.
        if (!dronePresent || !ready || pendingTake)
            hold_ = 0;
        else
            ++hold_;

        phase_ = Phase::Arming;               // enabled + not fired + not timed out = visible wait
        if (hold_ >= kReadyHoldTicks) {
            fired_ = true;
            phase_ = Phase::Fired;            // reported on the firing tick; Done from the next
            return {Action::Fire, Phase::Fired};
        }
        return {Action::None, Phase::Arming};
    }

    Phase phase() const { return phase_; }
    bool  fired() const { return fired_; }

private:
    Phase    phase_       = Phase::Disabled;
    int      hold_        = 0;
    bool     armed_       = false;   // the wait clock runs only from the first mirror sighting
    bool     fired_       = false;
    uint64_t waitStartMs_ = 0;
};

}  // namespace coop::dev::drone_take_gate
