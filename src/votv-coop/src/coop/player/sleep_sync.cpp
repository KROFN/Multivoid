// coop/sleep_sync.cpp -- see coop/sleep_sync.h.

#include "coop/player/sleep_sync.h"

#include "coop/comms/chat_feed.h"
#include "coop/dev/batch1_smoke.h"          // KROFNE FORK: B1_* diagnostic milestones (log-only)
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // KROFNE FORK: the restore target (local mainPlayer)
#include "coop/player/sleep_dilation_ownership.h"  // KROFNE FORK v3 (blocker P): dilation ownership
#include "coop/world/time_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"       // KROFNE FORK: IsLive (restore-target liveness)
#include "ue_wrap/engine/engine.h"         // KROFNE FORK: GetController + SetViewTargetWithBlend
#include "ue_wrap/actors/sleep.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>

namespace coop::sleep_sync {
namespace {

namespace SLP = ue_wrap::sleep;
namespace ENG = ue_wrap::engine;
namespace R   = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- local peer state -------------------------------------------------------
bool g_lastInBed = false;       // the isSleep edge detector
bool g_waitUndone = false;      // WAITING enforcement latch (dilation undone for this bed entry)
bool g_accelerate = false;      // the 20x phase is on (host decides; mirrored from the wire on clients)
bool g_dreamProbSuppressed = false;  // we hold gamemode.dreamProbability at 0
void* g_gmInst = nullptr;       // gamemode instance the policy/edge state was applied to

// KROFNE FORK (batch-1B, corrective G/H/I): SLEEP-OWNED STATE, tracked as TWO independent
// obligations.
//
// g_ownsSleepView: true ONLY after THIS module successfully wrote the local view target (the
//   ACCELERATE sleepCam handoff, or the WAITING bed hold) -- never inferred from isSleep alone.
//   The b133 module wrote the camera but delegated the restore to native wakeup() behind a
//   conditional `if (IsSleeping()) CallWakeup()`; every structural exit that skipped that branch
//   left the view stranded with NOTHING owning a restore (the faint -> camera-stuck softlock).
//
// g_ownsSleepDilation: true ONLY after THIS module successfully wrote a NON-NATIVE temporary
//   dilation (the ACCELERATE 20x). Corrective G: this is deliberately INDEPENDENT of
//   g_accelerate -- ApplyEndLocal clears g_accelerate BEFORE the restore runs, and the old
//   helper keyed the dilation undo off g_accelerate, so a stranded path could restore the camera
//   while leaving the world dilated at 20x. The obligation, once latched, survives until a
//   successful restore (or an explicit discharge) clears it.
//
// Corrective I: the restore clears an obligation ONLY on success. A failed resolution (world/
// gamemode transition) KEEPS the obligation and arms a bounded retry -- the module never
// permanently forgets a restore it owes, and never per-frame fights a valid transition (the
// retry is low-cadence and bounded).
// g_ownsSleepDilation (v2) was ONE bool latched ONLY by the ACCELERATE 20x write -- so the
// WAITING hold's forced 1.0 (the module overwriting a native value in the OTHER direction)
// left no ownership trace, and WAITING -> disconnect-while-sleeping handed vanilla sleep a
// world at 1x instead of its native 20x (blocker P). v3: ownership is an EXPLICIT state for
// BOTH directions (see sleep_dilation_ownership.h -- the tracker is pure and selftested
// headless); the engine-facing code below executes the writes it orders and commits ONLY on
// success.
bool g_ownsSleepView = false;
sleep_sync::DilationOwnershipTracker g_dilation;
bool g_restoreRetryArmed = false;
constexpr uint64_t kRestoreRetryIntervalMs = 500;   // low cadence: no per-frame fighting
constexpr uint64_t kRestoreRetryBoundMs = 10000;    // bounded: then log loudly and discharge
uint64_t g_restoreRetryStartMs = 0;
uint64_t g_retryDeadlineMs = 0;                     // the retry pump's absolute deadline

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// KROFNE FORK: the ONE restore helper (corrective G/I semantics). Discharges BOTH obligations
// independently and idempotently; an obligation clears ONLY on success. Never called per-frame
// -- only on structural exits (or by the bounded retry). Returns true iff nothing is owed.
bool RestoreLocallyOwnedSleepState(const char* reason) {
    bool owed = false;
    // Structural exit while NOT natively sleeping: restore the NORMAL world value (1.0) for
    // whichever override the module still owns (either direction -- blocker P).
    if (g_dilation.owned() != SleepDilationOwnership::None) {
        const float target = g_dilation.RestoreTargetWhileAwake();   // 1.0 when owned
        if (SLP::SetGlobalTimeDilation(target)) {
            g_dilation.CommitRestored();
            coop::dev::batch1_smoke::Emit("B1_SLEEP_RESTORE", "reason=%s dilation->%.1f", reason, target);
            UE_LOGI("sleep_sync: restored dilation -> %.1f (%s) [dilation ownership]", target, reason);
        } else {
            owed = true;
            UE_LOGW("sleep_sync: dilation restore FAILED (%s) -- obligation kept for retry", reason);
        }
    }
    if (g_ownsSleepView) {
        void* player = coop::players::Registry::Get().Local();
        void* pc = nullptr;
        if (player && R::IsLive(player)) pc = ENG::GetController(player);
        if (pc && ENG::SetViewTargetWithBlend(pc, player, 0.25f)) {
            g_ownsSleepView = false;
            coop::dev::batch1_smoke::Emit("B1_SLEEP_RESTORE", "reason=%s view->mainPlayer", reason);
            // Same-target re-entry after a native wakeup restore is harmless (blend to the view
            // it already has); the load-bearing case is the strand, where nothing else restores.
            UE_LOGI("sleep_sync: restored local view -> mainPlayer (%s) [view ownership]", reason);
        } else {
            owed = true;
            UE_LOGW("sleep_sync: view restore FAILED (%s; player=%p pc=%p) -- obligation kept "
                    "for retry", reason, static_cast<void*>(player), static_cast<void*>(pc));
        }
    }
    g_waitUndone = false;   // the WAITING hold is meaningless once a restore is (being) run
    if (owed && !g_restoreRetryArmed) {
        g_restoreRetryArmed = true;
        g_restoreRetryStartMs = NowMs();
        g_retryDeadlineMs = 0;   // (re)arm on the next pump
    }
    return !owed;
}

// The bounded retry pump (corrective I): discharges a restore the module still owes. Low cadence
// (500 ms), bounded (10 s) -- a failure that outlives the bound is logged loudly and DISCHARGED
// as superseded (a world/gamemode rebuild necessarily re-possesses the local player; fighting it
// longer would be the per-frame camera fight the design forbids). Runs on the game thread via
// Tick, before the connected gate, so a session that dropped mid-transition still retries.
void PumpRestoreRetry() {
    if (!g_restoreRetryArmed) return;
    if (!g_ownsSleepView && g_dilation.owned() == SleepDilationOwnership::None) {
        g_restoreRetryArmed = false;
        return;
    }
    const uint64_t now = NowMs();
    if (g_retryDeadlineMs == 0) g_retryDeadlineMs = now + kRestoreRetryBoundMs;  // arm the deadline
    if (now - g_restoreRetryStartMs < kRestoreRetryIntervalMs) return;
    g_restoreRetryStartMs = now;
    if (RestoreLocallyOwnedSleepState("restore-retry")) {
        g_restoreRetryArmed = false;
        g_retryDeadlineMs = 0;
        return;
    }
    if (now >= g_retryDeadlineMs) {
        // Outlived the bound: supersede-documented discharge (never a silent forget).
        UE_LOGW("sleep_sync: restore obligation superseded after %llums of failed retries "
                "(view owed=%d dilation owned=%d) -- a world/possession rebuild owns the camera "
                "now; DISCHARGING loudly", static_cast<unsigned long long>(kRestoreRetryBoundMs),
                g_ownsSleepView ? 1 : 0,
                g_dilation.owned() != SleepDilationOwnership::None ? 1 : 0);
        g_ownsSleepView = false;
        g_dilation.DischargeSuperseded();
        g_restoreRetryArmed = false;
        g_retryDeadlineMs = 0;
    }
}

// ---- host tally -------------------------------------------------------------
bool g_inBed[coop::net::kMaxPeers] = {};
uint8_t g_lastCount = 0, g_lastTotal = 0;

bool IsHost(coop::net::Session* s) { return s->role() == coop::net::Role::Host; }

void PushCounterLine(uint8_t count, uint8_t total) {
    if (count == 0) return;
    wchar_t buf[64];
    ::swprintf(buf, 64, L"%u/%u players sleeping", static_cast<unsigned>(count),
               static_cast<unsigned>(total));
    // All three sleep lines are Transient: they are a live STATUS readout of an
    // in-progress ritual ("2/3 players sleeping"), meaningless once it is over --
    // reading them back hours later would be noise, not history.
    coop::chat_feed::Push(buf, coop::chat_feed::Keep::Transient);
}

void ApplyDreamProbPolicy(coop::net::Session* s) {
    // CLIENTS: nightmares suppressed for the whole session (host-only rolls).
    // HOST: suppressed while idle/waiting; the -1 sentinel (use bed.dreamProb)
    // is restored only DURING the accelerate phase, making the host the single
    // nightmare roller of the gated night.
    const bool wantSuppressed = !(IsHost(s) && g_accelerate);
    if (SLP::SetDreamProbability(wantSuppressed ? 0.f : -1.f))
        g_dreamProbSuppressed = wantSuppressed;
}

// HOST: recount + broadcast the tally; fire phase transitions.
void HostRetally(coop::net::Session* s);

// Apply the accelerate phase locally (both roles). The dilation only applies
// to a peer that is actually in bed -- a peer that raced its exit stays at
// 1.0 (its inBed=false report is in flight; the host will End).
void ApplyAccelerateLocal(coop::net::Session* s, bool on) {
    if (g_accelerate == on) return;
    g_accelerate = on;
    if (on) {
        if (SLP::IsSleeping()) {
            // Corrective G/v3: latch DILATION ownership explicitly (only on success) -- the
            // ACCELERATE override is one of the TWO tracked directions (blocker P).
            if (SLP::SetGlobalTimeDilation(20.f)) {
                g_dilation.CommitAccelerate();
                coop::dev::batch1_smoke::Emit("B1_SLEEP_DILATION_OWN", "state=accelerate-20x");
            }
            // The cinematic base view belongs to THIS phase (user directive
            // 2026-06-13): the WAITING hold parked the camera at the bed;
            // hand it to the gamemode's own sleepCam now.
            if (SLP::SetSleepViewTarget(SLP::SleepCam())) {
                g_ownsSleepView = true;  // latch ownership (we wrote the view)
                coop::dev::batch1_smoke::Emit("B1_SLEEP_VIEW_OWN", "phase=accelerate sleepCam");
                UE_LOGI("sleep_sync: camera -> sleepCam (the timelapse view)");
            }
        }
        if (!IsHost(s)) coop::time_sync::SetSleepAccelerate(true);
        coop::chat_feed::Push(L"Everyone is asleep -- the night passes...",
                              coop::chat_feed::Keep::Transient);
        UE_LOGI("sleep_sync: ACCELERATE (local dilation -> %s)",
                SLP::IsSleeping() ? "20" : "1 (not in bed)");
    } else {
        if (!IsHost(s)) coop::time_sync::SetSleepAccelerate(false);
    }
    ApplyDreamProbPolicy(s);
}

// Apply the END locally (both roles). `natural` grants the full night's rest.
// ORDER MATTERS: wakeup() first (the 10% gearer gift rolls iff need >= 99 at
// call time -- the native waker keeps its own native roll; mirrors must not
// add one), the need write after.
void ApplyEndLocal(coop::net::Session* s, bool natural) {
    ApplyAccelerateLocal(s, false);
    if (SLP::IsSleeping()) SLP::CallWakeup();
    // KROFNE FORK (batch-1B, corrective G): the unconditional mod-owned restore edge. When
    // IsSleeping() was true the native wakeup() above already re-possessed + restored the view --
    // re-setting the same target is a harmless blend; the load-bearing case is the strand (End
    // while already !isSleep): native wakeup is an idempotent no-op there and restores NOTHING,
    // which is the faint -> camera-stuck-forever hole. The helper no-ops for obligations the mod
    // does not hold, clears each obligation ONLY on success (corrective I), and the dilation undo
    // keys off g_ownsSleepDilation -- NOT g_accelerate, which ApplyAccelerateLocal(false) above
    // already cleared (the old code's stranded-20x hole).
    RestoreLocallyOwnedSleepState("sleep-end");
    if (natural) SLP::WriteSleepNeed(100.f);
    coop::chat_feed::Push(natural ? L"Good morning -- everyone is rested."
                                  : L"Sleep interrupted -- everyone wakes up.",
                          coop::chat_feed::Keep::Transient);
    UE_LOGI("sleep_sync: END (natural=%d)", natural ? 1 : 0);
}

void HostBroadcast(coop::net::Session* s, uint8_t op, uint8_t flag,
                   uint8_t count, uint8_t total) {
    coop::net::SleepStatePayload p{};
    p.op = op;
    p.flag = flag;
    p.count = count;
    p.total = total;
    s->SendReliable(coop::net::ReliableKind::SleepState, &p, sizeof(p));
}

void HostRetally(coop::net::Session* s) {
    // total = the host + every world-ready client (a mid-join peer is not in
    // the world yet and must not block the gate; once world-ready it counts
    // -- and being awake, it holds the gate open until it sleeps too).
    uint8_t total = 1, count = g_inBed[0] ? 1 : 0;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (!s->IsSlotWorldReady(slot)) continue;
        ++total;
        if (g_inBed[slot]) ++count;
    }
    if (count != g_lastCount || total != g_lastTotal) {
        g_lastCount = count;
        g_lastTotal = total;
        // The counter is waiting-room UI only: during the accelerate phase a
        // count drop is the END transition, and "1/2 players sleeping"
        // flashing right before "Sleep interrupted" reads as nonsense (the
        // 00:28 probe smoke).
        if (!g_accelerate && count > 0 && count < total) {
            HostBroadcast(s, 1 /*Tally*/, 0, count, total);
            PushCounterLine(count, total);
            UE_LOGI("sleep_sync: tally %u/%u in bed", static_cast<unsigned>(count),
                    static_cast<unsigned>(total));
        }
    }
    if (!g_accelerate && count > 0 && count == total) {
        HostBroadcast(s, 2 /*Accelerate*/, 0, count, total);
        ApplyAccelerateLocal(s, true);
    } else if (g_accelerate && count < total) {
        // ANY drop ends the night. Natural iff the HOST is the one who woke
        // with a full need (its native wake-check fired at >= 100; clients
        // are clamped below the threshold and cannot end naturally; every
        // other cause -- manual exit, hunger, event, nightmare -- interrupts).
        float hostNeed = 0.f;
        const bool natural = !g_inBed[0] && SLP::ReadSleepNeed(hostNeed) && hostNeed >= 99.5f;
        // Zero the tally NOW: every peer's wakeup is about to report a falling
        // edge; processing those against stale true flags would paint spurious
        // "N/M sleeping" lines after the morning line.
        for (bool& b : g_inBed) b = false;
        g_lastCount = 0;
        HostBroadcast(s, 3 /*End*/, natural ? 1 : 0, 0, total);
        ApplyEndLocal(s, natural);
    }
}

void ReportLocalEdge(coop::net::Session* s, bool inBed) {
    if (IsHost(s)) {
        g_inBed[0] = inBed;
        HostRetally(s);
    } else {
        coop::net::SleepStatePayload p{};
        p.op = 0;  // Report
        p.flag = inBed ? 1 : 0;
        s->SendReliableToSlot(0, coop::net::ReliableKind::SleepState, &p, sizeof(p));
    }
    UE_LOGI("sleep_sync: local %s bed", inBed ? "entered" : "left");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!SLP::EnsureResolved()) return;
    void* gm = SLP::Gamemode();
    if (!gm) return;

    // Fresh gamemode (boot / level reload): re-prime the edge state + re-apply
    // the nightmare policy on the new instance (the signal_catch shape). A
    // connected gamemode swap is unreachable in the session lifecycle today
    // (level travel disconnects first), but a stranded accelerate/tally would
    // be poisonous if it ever became reachable -- clear it all (audit note 1).
    if (gm != g_gmInst) {
        // KROFNE FORK (batch-1B): a fresh gamemode is a structural exit for any state we still own
        // (the old sleepCam/sleepingPawn died with the old world). Restore-or-arm-retry BEFORE
        // the phase state is wiped, so no obligation can survive the swap pointing at a dead
        // camera (a failure arms the bounded retry, not a silent forget).
        RestoreLocallyOwnedSleepState("gamemode-reset");
        g_gmInst = gm;
        g_lastInBed = SLP::IsSleeping();
        g_waitUndone = false;
        g_accelerate = false;
        for (bool& b : g_inBed) b = false;
        g_lastCount = g_lastTotal = 0;
        if (s->connected()) ApplyDreamProbPolicy(s);
    }

    // Corrective I: the bounded restore-retry pump runs on every Tick (game thread), including a
    // connected-but-not-world-ready gap and a solo running session -- a restore obligation armed
    // mid-transition gets its bounded retries even if connectivity changed under it.
    PumpRestoreRetry();

    if (!s->connected()) {
        // Solo session (host with no peers yet): stock SP behavior, nothing
        // gated. The policy/state re-arms on the first connect edge below.
        return;
    }

    const bool inBed = SLP::IsSleeping();

    // One-shot per connect edge: make sure the nightmare policy is applied
    // (covers the host-was-already-running case + the first client connect).
    if (!g_dreamProbSuppressed && !g_accelerate) ApplyDreamProbPolicy(s);

    // WAITING enforcement: a peer in bed outside the accelerate phase runs at
    // dilation 1.0. Edge-latched -- the only native setter is the sleep()
    // entry (@70543), so one undo per bed entry suffices; wakeup() itself
    // restores 1.0 on every exit path. Covers ALL entries: bed interaction,
    // the dev probe's reflected sleep(), and a host that was already asleep
    // when the first client connected.
    if (inBed && !g_accelerate && !g_waitUndone) {
        // Latch ONLY on success (perf-audit note A): a transient world-context
        // gap would otherwise skip the undo yet latch, stranding this peer at
        // the native 20x for the whole wait. On failure we just retry next tick.
        if (SLP::SetGlobalTimeDilation(1.0f)) {
            g_waitUndone = true;
            coop::dev::batch1_smoke::Emit("B1_SLEEP_DILATION_OWN", "state=waiting-1x");
            // Corrective G/v3 (blocker P): the module just forced dilation to 1.0 -- THAT is now
            // the owned state (replacing any prior ACCELERATE ownership: the current value is
            // genuinely 1.0). The disconnect handoff reads this state and returns the world to
            // vanilla sleep's OWN 20x regardless of which direction we forced.
            g_dilation.CommitWaitingUndo();
            // Hold the camera AT THE BED while waiting (user directive
            // 2026-06-13): the native entry retargeted it to the sleepCam
            // base shot; the cinematic only belongs to the all-asleep phase
            // (ApplyAccelerateLocal hands it back). Null-safe: if the pawn/
            // controller is not up yet the view stays native -- cosmetic,
            // never a gate blocker.
            const bool held = SLP::SetSleepViewTarget(SLP::SleepingPawn());
            if (held) {
                g_ownsSleepView = true;  // latch ownership (we wrote the view)
                coop::dev::batch1_smoke::Emit("B1_SLEEP_VIEW_OWN", "phase=waiting bed");
            }
            UE_LOGI("sleep_sync: WAITING -- dilation undone to 1.0 (gate not full)%s",
                    held ? ", camera held at the bed" : "");
        }
    } else if ((!inBed || g_accelerate) && g_waitUndone) {
        g_waitUndone = false;
    }

    // The isSleep edge -> report (host tallies itself directly).
    if (inBed != g_lastInBed) {
        g_lastInBed = inBed;
        ReportLocalEdge(s, inBed);
        // KROFNE FORK (batch-1B, corrective I): the falling edge is the other structural exit. If
        // the native wakeup restored the view, this re-sets the same target (harmless blend); if
        // isSleep was cleared WITHOUT a wakeup (the strand -- latent-chain severance, a mod reset
        // path), this is the only edge that ever gives the camera back. A failed resolution arms
        // the bounded retry instead of discarding the obligation.
        if (!inBed) RestoreLocallyOwnedSleepState("isSleep-falling-edge");
    }

    // CLIENT clamp during the phase: only the HOST ends the night naturally
    // (one authority, no first-to-fill race; the full need is granted to
    // everyone at a natural END anyway). Clamp at 98 -- STRICTLY below both
    // the natural-wake check (>= 100) and wakeup's gearer-gift threshold
    // (>= 99 at call time): a 99 clamp would make every clamped client roll
    // the 10% gift inside the END wakeup. 1 float read per tick, only while
    // accelerated and in bed.
    if (g_accelerate && !IsHost(s) && inBed) {
        float need = 0.f;
        if (SLP::ReadSleepNeed(need) && need > 98.f) SLP::WriteSleepNeed(98.f);
    }
}

void OnReliable(const coop::net::SleepStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (IsHost(s)) {
        // Host ingests Reports only; trust = the transport sender slot.
        if (p.op != 0) return;
        if (senderSlot == 0 || senderSlot >= coop::net::kMaxPeers) return;
        if (g_inBed[senderSlot] == (p.flag != 0)) return;
        g_inBed[senderSlot] = p.flag != 0;
        HostRetally(s);
        return;
    }
    // Client: phase broadcasts from the host only (transport-trusted slot 0).
    if (senderSlot != 0) return;
    switch (p.op) {
    case 1:  // Tally
        PushCounterLine(p.count, p.total);
        break;
    case 2:  // Accelerate
        ApplyAccelerateLocal(s, true);
        break;
    case 3:  // End
        ApplyEndLocal(s, p.flag != 0);
        break;
    default:
        break;
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost(s)) return;
    if (peerSlot <= 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_inBed[peerSlot] = false;  // a joiner arrives awake
    // A running phase cannot survive an awake arrival; a waiting tally just
    // re-counts (the joiner now holds the gate open until it sleeps too).
    HostRetally(s);
}

void OnDisconnectForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost(s)) return;
    if (slot <= 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    if (!g_inBed[slot]) {
        // An awake leaver may have been the one holding the gate open.
        HostRetally(s);
        return;
    }
    g_inBed[slot] = false;
    HostRetally(s);
}

void OnDisconnect() {
    auto* s = g_session.load(std::memory_order_acquire);
    // KROFNE FORK (batch-1B, corrective H): the disconnect is a structural exit for OUR sleep
    // writes -- and a REAL HANDOFF, not a latch drop.
    //
    // While natively in bed, vanilla singleplayer sleep owns the presentation: its own sleepCam
    // (the gamemode actor SleepCam() resolves) + its own 20x timelapse (the stock behavior this
    // module mirrors during ACCELERATE). During WAITING the mod may have overwritten exactly
    // those (dilation forced to 1.0, view parked at the SleepingPawn), so merely dropping our
    // latches would leave the native timelapse running at our 1.0 on OUR camera hold. We hand
    // BACK to the vanilla presentation explicitly:
    //   - view we own -> SleepCam() (the native sleep presentation; the player keeps sleeping);
    //   - dilation we own -> 20.0f (the native timelapse value -- see the note below);
    //   - we NEVER wake the player as a cleanup shortcut; the native wake keeps its own restore.
    // If any handoff write fails (world teardown mid-call), the obligation is discharged with a
    // loud SUPERSEDED log: the level switch back to SP rebuilds possession and the native sleep
    // state wholesale, so no further restore of ours could win -- documented supersede, not a
    // silent forget (corrective I's spirit, adapted to the one transition where Tick retries
    // cannot run).
    if (SLP::IsSleeping()) {
        if (g_ownsSleepView) {
            if (SLP::SetSleepViewTarget(SLP::SleepCam())) {
                g_ownsSleepView = false;
                UE_LOGI("sleep_sync: disconnect while natively in bed -- view handed back to the "
                        "vanilla sleepCam");
            } else {
                g_ownsSleepView = false;
                UE_LOGW("sleep_sync: disconnect handoff view write FAILED (vanilla sleepCam "
                        "unavailable) -- SUPERSEDED by the SP level rebuild (player keeps "
                        "sleeping natively)");
            }
        }
        if (g_dilation.owned() != SleepDilationOwnership::None) {
            // BLOCKER P (v3): the handoff target is decided by the OWNERSHIP STATE, not by which
            // phase is live: EITHER direction of override (WaitingForcedOne OR
            // AccelerateForcedTwenty) must return the world to vanilla sleep's OWN presentation
            // value (20x -- what native sleep would be running had the module never touched it).
            // This is the exact WAITING -> disconnect-while-sleeping case v2 missed.
            const float target = g_dilation.DisconnectHandoffTargetWhileSleeping();   // 20.0
            if (SLP::SetGlobalTimeDilation(target)) {
                g_dilation.CommitRestored();
                UE_LOGI("sleep_sync: disconnect handoff -- dilation returned to the native "
                        "timelapse (%.0f)", target);
            } else {
                g_dilation.DischargeSuperseded();
                UE_LOGW("sleep_sync: disconnect handoff dilation write FAILED -- SUPERSEDED by "
                        "the SP level rebuild");
            }
        }
        if (!g_ownsSleepView && g_dilation.owned() == SleepDilationOwnership::None)
            UE_LOGI("sleep_sync: disconnect while natively in bed -- vanilla sleep owns the "
                    "presentation (player not woken)");
    } else {
        // Not natively asleep: nothing native will ever revisit a stranded WAITING/ACCELERATE
        // view -- restore it (corrective I: success-only clearing; a failure here cannot retry
        // because gameplay ticks end with the session, so the failure path logs and discharges
        // as superseded by the menu/SP rebuild).
        if (g_ownsSleepView || g_dilation.owned() != SleepDilationOwnership::None) {
            if (!RestoreLocallyOwnedSleepState("disconnect")) {
                UE_LOGW("sleep_sync: disconnect restore incomplete (player/controller already "
                        "torn down) -- SUPERSEDED by the menu/SP rebuild");
                g_ownsSleepView = false;
                g_dilation.DischargeSuperseded();
                g_restoreRetryArmed = false;
                g_retryDeadlineMs = 0;
            }
        }
    }
    // Restore the SP nightmare sentinel + normal time policy. A peer still in
    // bed keeps sleeping -- with the session gone its world is SP again and
    // the native 20x timelapse is exactly the stock behavior; time_sync's own
    // OnDisconnect restores the client TimeScale.
    if (g_dreamProbSuppressed) {
        SLP::SetDreamProbability(-1.f);
        g_dreamProbSuppressed = false;
    }
    if (g_accelerate && s && s->role() == coop::net::Role::Client)
        coop::time_sync::SetSleepAccelerate(false);
    g_accelerate = false;
    g_waitUndone = false;
    g_lastInBed = false;
    g_gmInst = nullptr;
    for (bool& b : g_inBed) b = false;
    g_lastCount = g_lastTotal = 0;
}

}  // namespace coop::sleep_sync
