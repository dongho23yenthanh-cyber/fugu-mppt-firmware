#pragma once
#include <cstdint>

struct PwmTiming {
    uint32_t resolution_hz;
    uint32_t period_ticks;
    uint32_t actual_freq;
};

// Largest period_ticks (best duty resolution) for `freq`, keeping the group
// prescaler an integer divide of `src_clk` and period within 16 bits.
static inline PwmTiming bestTiming(uint32_t freq, uint32_t src_clk = 160000000u) {
    uint32_t presc = 1;
    while (src_clk / presc / freq > 65535u) ++presc;
    uint32_t res   = src_clk / presc;
    uint32_t ticks = (res + freq / 2) / freq;       // rounded
    return PwmTiming{res, ticks, res / ticks};
}
