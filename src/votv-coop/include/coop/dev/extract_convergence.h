// coop/dev/extract_convergence.h -- ACTUATOR-ONLY pass (T2): the extraction actuator's PURE
// baseline-convergence gate.
//
// Node2 evidence (protocol 2133 runtime, run batch1-2133-runtime-20260831T001318Z): the old
// container_selftest fired the CLIENT extract on a FIXED +25s timer -- before the client had
// consumed the host's published baseline for the container (author base=0 vs the host-published
// hash) -- and the host's CAS REFUSED it (STALE BASE). That refusal was the CAS WORKING AS
// DESIGNED; the deficiency was the actuator's orchestration. This gate replaces the fixed
// timer: the actuator fires only after PROVEN convergence for the SAME container identity (eid):
//   baseHash  != 0              -- the host truth was consumed (the readout's consumed-base hash)
//   && localHash == baseHash    -- the local slice IS that truth (the production ContentHash)
// Both numbers arrive from the READ-ONLY
// coop::props::container_contents_sync::DevBaselineReadout; this gate has NO write path to any
// base or version, so it can WAIT for convergence but can never MANUFACTURE it. A host refusal
// AFTER proven convergence is REAL regression evidence and is deliberately not bypassable here.
//
// Pure: no engine types, no I/O, no clock of its own. L1-tested
// (tests/batch1_corrective_selftest.cpp, section T2).

#pragma once

#include <cstdint>

namespace coop::dev::extract_convergence {

enum class Phase { Waiting, Fired, Done, TimedOut };
enum class Action { None, Fire };

// One observation of a container's baseline bookkeeping (READ-ONLY source:
// coop::props::container_contents_sync::DevBaselineReadout).
struct Observation {
    uint64_t baseHash  = 0;   // the host truth this peer last consumed (0 = none consumed yet)
    uint64_t localHash = 0;   // the live local slice under the production ContentHash
};

struct Decision {
    Action action = Action::None;
    Phase  phase  = Phase::Waiting;
};

// The baseline wait is terminal: this long after the first observation with no proven
// convergence, the gate expires into TimedOut (NOT-FIRED) and never fires.
inline constexpr uint64_t kWaitTimeoutMs = 300000;   // 5 min

class Gate {
public:
    Decision Tick(uint64_t nowMs, const Observation& obs) {
        if (fired_) {                          // exactly one fire; permanently inert after it
            phase_ = Phase::Done;
            return {Action::None, Phase::Done};
        }
        if (phase_ == Phase::TimedOut)         // terminal: even convergence cannot fire now
            return {Action::None, Phase::TimedOut};
        if (!started_) {
            started_ = true;
            startMs_ = nowMs;
        }
        if (nowMs - startMs_ > kWaitTimeoutMs) {
            phase_ = Phase::TimedOut;          // NOT-FIRED terminal; production state untouched
            return {Action::None, Phase::TimedOut};
        }
        // Convergence is defined by EXACT EQUALITY ONLY, and only on a consumed baseline. The
        // gate holds no counters that "approximate" it: contradictory observations cannot add
        // up to it, because the gate has nothing else to add.
        if (obs.baseHash != 0 && obs.baseHash == obs.localHash) {
            fired_ = true;
            phase_ = Phase::Fired;             // reported on the firing tick; Done from the next
            return {Action::Fire, Phase::Fired};
        }
        phase_ = Phase::Waiting;
        return {Action::None, Phase::Waiting};
    }

    Phase phase() const { return phase_; }
    bool  fired() const { return fired_; }

private:
    Phase    phase_   = Phase::Waiting;
    bool     started_ = false;
    bool     fired_   = false;
    uint64_t startMs_ = 0;
};

}  // namespace coop::dev::extract_convergence
