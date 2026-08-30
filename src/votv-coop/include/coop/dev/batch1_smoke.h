// coop/dev/batch1_smoke.h -- KROFNE FORK (2133): the Batch-1 RUNTIME CONTRACT's diagnostic
// smoke-log lane (test-contract pass).
//
// WHAT IT IS: a log-only diagnostic mode. With `[dev] batch1_smoke=1` in multivoid.ini, one
// milestone line per Batch-1 causal event reaches the log during NORMAL play -- no automation,
// no gameplay effect, no performance probe (it writes at most one line per milestone tag per
// second, and the tags fire at most a handful of times per session anyway):
//
//   DRONE      B1_DRONE_VERB            the 0x45 dropSack verb was observed on a client
//              B1_DRONE_REQ nonce=      the request left the client
//              B1_DRONE_HOST_ACCEPT     the host admitted the request
//              B1_DRONE_NATIVE_DROPSACK the host dispatched the native verb body
//              B1_DRONE_PHANTOM_CAPTURE the client captured its exact local phantom
//              B1_DRONE_RESULT          the verdict reached the client
//              B1_DRONE_REAL_SACK       the host adopted the REAL sack spawn (with its key)
//   EXTRACTION B1_EXTRACT_EDGE          the takeObj edge minted a correlation token
//              B1_EXTRACT_MUTATION_APPLIED the paired contents write returned Applied
//              B1_EXTRACT_BIRTH_COMMIT  the pairing committed (kOk ordered)
//              B1_EXTRACT_HOST_KEY_PROVEN the spawned actor's OWN key was read back and MATCHED
//              B1_EXTRACT_RESULT        the verdict reached the extractor
//   SLEEP      B1_SLEEP_VIEW_OWN        the module took ownership of the local camera view
//              B1_SLEEP_DILATION_OWN    the module took ownership of the time dilation
//              B1_SLEEP_RESTORE reason= the module discharged its ownership obligations
//
// The runtime test these milestones enable requires ONLY normal gameplay actions: press the
// drone take, take an item from a synced container, sleep. A missing milestone names the dead
// causal hop precisely.
//
// GATE: everything is behind the ini flag (default OFF -- zero lines in normal play) AND the
// [dev] master switch. The rate limiter is a pure struct so the headless suites can test it.

#pragma once

#include <cstdint>

namespace coop::dev::batch1_smoke {

// Latched once per process (first call) from [dev] batch1_smoke AND the [dev] master switch.
bool Enabled();

// Test-only: forget the latch + rate slots (the headless suites re-latch from scratch).
void ResetForTest();

// PURE rate limiter: one slot per milestone tag; a tag may emit at most once per interval.
struct RateLimiter {
    uint64_t lastMs = 0;
    bool RateOk(uint64_t nowMs, uint64_t minIntervalMs) {
        if (lastMs != 0 && nowMs - lastMs < minIntervalMs) return false;
        lastMs = nowMs;
        return true;
    }
};

// Emit one milestone line: `milestone` is the exact B1_* tag, `fmt`/`...` the printf-style
// detail. Gated behind Enabled() and the per-tag rate limit; a no-op otherwise (near-free).
void Emit(const char* milestone, const char* fmt, ...);

}  // namespace coop::dev::batch1_smoke
