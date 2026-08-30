// coop/player/sleep_dilation_ownership.h -- KROFNE FORK (2133): the sleep-dilation OWNERSHIP
// state machine, carved out of sleep_sync.cpp in the v3 corrective pass (blocker P).
//
// THE BUG CLASS (v2): dilation ownership was ONE bool (`g_ownsSleepDilation`) latched ONLY by
// the ACCELERATE 20x write. The WAITING hold's forced 1.0 -- the module ALSO overwriting a
// native value, in the OTHER direction -- left no ownership trace. The disconnect handoff
// therefore returned the world to vanilla sleep at 20x ONLY when ACCELERATE had latched, and
// the exact WAITING -> disconnect-while-sleeping case handed vanilla SP sleep a world at 1x:
// its native timelapse presentation silently lost (the corrective report claimed this fixed;
// the code contradicted it).
//
// THE FIX: ownership is an EXPLICIT state for BOTH directions of override. The engine-facing
// caller executes the writes this tracker orders and calls the Commit* methods ONLY on a
// successful engine write; every decision (what to latch, what the handoff targets, what a
// restore targets) lives HERE so the four transition cases are pure and headless-testable:
//   WAITING(1x)        -> disconnect while natively sleeping -> 20x   (the blocker-P case)
//   ACCELERATE(20x)    -> END (wake)                           -> 1x
//   WAITING(1x)        -> wake                                 -> 1x
//   ACCELERATE -> WAITING -> disconnect while sleeping           -> 20x
// plus the "the module never touched dilation" case: NO write at all (native sleep owns its
// own 20x and the module must not stomp it).

#pragma once

#include <cstdint>

namespace coop::sleep_sync {

// WHOSE write is the current global time dilation carrying? `None` means the module has made
// NO successful override since the last discharge -- vanilla owns whatever the world runs at.
enum class SleepDilationOwnership : uint8_t {
    None                   = 0,
    WaitingForcedOne       = 1,   // the WAITING hold forced the native 20x down to 1.0
    AccelerateForcedTwenty = 2    // the ACCELERATE phase forced 20x (the native-timelapse mirror)
};

class DilationOwnershipTracker {
public:
    // The three values the module ever writes (documented as constants because the vanilla
    // presentation expects exactly these: 20x = the native sleep timelapse, 1x = normal world).
    static constexpr float kAccelerateDilation   = 20.f;
    static constexpr float kNormalDilation       = 1.f;
    static constexpr float kNativeSleepDilation  = 20.f;

    SleepDilationOwnership owned() const { return owned_; }

    // Latches (call ONLY after the corresponding engine write SUCCEEDED).
    void CommitAccelerate()    { owned_ = SleepDilationOwnership::AccelerateForcedTwenty; }
    void CommitWaitingUndo()   { owned_ = SleepDilationOwnership::WaitingForcedOne; }
    // Discharge on a successful restore-to-normal (END / falling edge / gamemode reset).
    void CommitRestored()      { owned_ = SleepDilationOwnership::None; }
    // Discharge when a higher authority superseded the obligation (documented supersede, never
    // a silent forget -- the caller logs loudly).
    void DischargeSuperseded() { owned_ = SleepDilationOwnership::None; }

    // THE BLOCKER-P HANDOFF (pure): the session dropped while the player is still natively
    // sleeping. If THIS MODULE forced a dilation in EITHER direction, vanilla sleep takes the
    // world back and its presentation expects its OWN 20x timelapse -- the handoff write is 20x
    // regardless of which direction we forced, because 20x is what native sleep would be
    // running had we never touched it. Nothing is owed (0.f = no write) when the module never
    // overrode dilation: native sleep has been running its own 20x the whole time.
    float DisconnectHandoffTargetWhileSleeping() const {
        return owned_ == SleepDilationOwnership::None ? 0.f : kNativeSleepDilation;
    }

    // A structural exit while NOT natively sleeping (END after wakeup, isSleep falling edge,
    // gamemode reset, disconnect-while-awake): the normal world value is the restore target.
    // 0.f = nothing owed.
    float RestoreTargetWhileAwake() const {
        return owned_ == SleepDilationOwnership::None ? 0.f : kNormalDilation;
    }

private:
    SleepDilationOwnership owned_ = SleepDilationOwnership::None;
};

}  // namespace coop::sleep_sync
