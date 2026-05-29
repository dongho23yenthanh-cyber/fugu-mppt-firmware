#pragma once

#include <INA226_WE.h> // INA226_CONV_TIME
#include <cstdint>

// Pure conversion-time helpers for the INA226 driver, split out so they can be unit-tested
// without pulling in Wire / attachInterrupt / the ADC_INA226 instance globals (see ina226.h).

struct INA226ConvTime {
    INA226_CONV_TIME setting;
    uint16_t us; // per-conversion time the device actually uses
};

// Device conversion-time options, ascending (datasheet table).
inline constexpr INA226ConvTime kIna226ConvTimes[] = {
        {CONV_TIME_140,  140},
        {CONV_TIME_204,  204},
        {CONV_TIME_332,  332},
        {CONV_TIME_588,  588},
        {CONV_TIME_1100, 1100},
        {CONV_TIME_2116, 2116},
        {CONV_TIME_4156, 4156},
        {CONV_TIME_8244, 8244},
};

// Smallest device setting whose time is >= the requested microseconds (so a derived alert
// timeout is never tighter than the real conversion), clamped to the device range.
inline INA226ConvTime ina226ConvTimeAtLeast(long us) {
    for (auto &c: kIna226ConvTimes)
        if ((long) c.us >= us)
            return c;
    return kIna226ConvTimes[sizeof(kIna226ConvTimes) / sizeof(*kIna226ConvTimes) - 1];
}

// Alert wait-timeout (ms) covering one full Vbus+I conversion pair with 2x headroom and a 3 ms
// floor (jitter margin / preserves the original busy-wait budget at the fast settings).
inline uint32_t ina226AlertTimeoutMs(uint16_t convUs) {
    uint32_t pairMs = (2u * (uint32_t) convUs + 999u) / 1000u; // ceil((2*convUs)/1000)
    uint32_t t = pairMs * 2u;
    return t < 3u ? 3u : t;
}

// Nominal sample rate for one channel: a Vbus+I pair completes every 2*convUs.
inline float ina226SampleRate(uint16_t convUs) {
    return 1e6f / (2.f * (float) convUs);
}
