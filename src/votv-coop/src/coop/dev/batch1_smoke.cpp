// coop/dev/batch1_smoke.cpp -- see coop/dev/batch1_smoke.h (the B1_* diagnostic smoke lane).

#include "coop/dev/batch1_smoke.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace coop::dev::batch1_smoke {
namespace {

bool g_enabled = false;
bool g_latched = false;

// One rate slot per milestone tag (linear scan; the tag set is small and fixed).
constexpr size_t      kMaxSlots       = 32;
constexpr uint64_t    kMinIntervalMs  = 1000;
struct Slot {
    const char* tag;
    RateLimiter rl;
};
Slot  g_slots[kMaxSlots];
size_t g_slotCount = 0;

RateLimiter* SlotFor(const char* tag) {
    for (size_t i = 0; i < g_slotCount; ++i)
        if (std::strcmp(g_slots[i].tag, tag) == 0) return &g_slots[i].rl;
    if (g_slotCount < kMaxSlots) {
        g_slots[g_slotCount] = Slot{tag, RateLimiter{}};
        return &g_slots[g_slotCount++].rl;
    }
    return nullptr;   // table full: the tag emits unrate-limited (cannot happen: fixed tag set)
}

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

bool Enabled() {
    if (!g_latched) {
        g_latched = true;
        g_enabled = config::MasterEnabled() &&
                    config::ResolveFlag(::coop::config_registry::rows::batch1_smoke);
        if (g_enabled)
            UE_LOGI("batch1_smoke: ENABLED -- Batch-1 milestone lines will be logged during "
                    "normal play (log-only; no gameplay effect)");
    }
    return g_enabled;
}

void ResetForTest() {
    g_latched = false;
    g_enabled = false;
    g_slotCount = 0;
}

void Emit(const char* milestone, const char* fmt, ...) {
    if (!Enabled()) return;
    RateLimiter* rl = SlotFor(milestone);
    if (rl && !rl->RateOk(NowMs(), kMinIntervalMs)) return;
    char detail[224];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, args);
    va_end(args);
    UE_LOGI("%s %s", milestone, detail);
}

}  // namespace coop::dev::batch1_smoke
