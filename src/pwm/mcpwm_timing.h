#pragma once
#include <cstdint>
#include <cmath>

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

// rect_offset is a fixed gate-drive/MOSFET turn-off delay (a TIME), stored in coil.conf as
// rect_offset_ns and converted to PWM comparator counts with the driver tick rate
// (counts/sec = fsw * pwmMax, same basis as boot_refresh_ns). Storing the time keeps it
// invariant to PWM resolution and switching frequency.
static inline int rectOffsetCountsFromNs(float ns, float tickRateHz) {
    return (int) std::lround(ns * 1e-9f * tickRateHz);
}
static inline float rectOffsetNsFromCounts(int counts, float tickRateHz) {
    return tickRateHz > 0.f ? (float) counts * 1e9f / tickRateHz : 0.f;
}
