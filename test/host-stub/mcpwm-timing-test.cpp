#include <cassert>
#include <cstdio>
#include "../../src/pwm/mcpwm_timing.h"

int main() {
    // 39 kHz off 160 MHz: prescaler 1, ~4103 ticks, ~12-bit, freq within 0.1%
    auto t = bestTiming(39000);
    assert(t.resolution_hz == 160000000u);
    assert(t.period_ticks == 4103u);            // round(160e6/39000)
    assert(t.actual_freq > 38900 && t.actual_freq < 39100);

    // 5 kHz still fits 16-bit at prescaler 1 (32000 ticks)
    auto lo = bestTiming(5000);
    assert(lo.period_ticks == 32000u && lo.resolution_hz == 160000000u);

    // 2 kHz would need 80000 ticks > 65535 -> prescaler bumps to 2
    auto vlo = bestTiming(2000);
    assert(vlo.period_ticks <= 65535u);
    assert(vlo.resolution_hz == 80000000u);

    printf("mcpwm-timing-test OK\n");
    return 0;
}
