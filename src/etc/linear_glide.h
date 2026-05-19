#pragma once

#include <cmath>
#include <cstdint>

// Time-bounded linear interpolation between two values. Use to smooth a
// setpoint that would otherwise step abruptly — e.g. the pack-voltage
// fallback transition when BMS data goes stale.
//
// Usage:
//   LinearGlide g{5'000'000}; // 5 s duration
//   if (!g.active()) g.start(currentValue, targetValue, nowUs);
//   setpoint = g.value(nowUs); // clamped to target once duration elapsed
//   ...
//   g.reset(); // when leaving the transition state
//
// Not thread-safe. Caller is responsible for serialising access.
class LinearGlide {
    const uint32_t _durUs;
    unsigned long _startUs = 0; // 0 == inactive
    float _from = NAN;
    float _to = NAN;

public:
    explicit LinearGlide(uint32_t durationUs) : _durUs(durationUs) {}

    void start(float from, float to, unsigned long nowUs) {
        _from = from;
        _to = to;
        _startUs = nowUs;
    }

    [[nodiscard]] bool active() const { return _startUs != 0; }

    // Linearly interpolated value at `nowUs`. Returns `to` once the duration
    // has elapsed. Precondition: active() — undefined return when inactive.
    [[nodiscard]] float value(unsigned long nowUs) const {
        unsigned long dt = nowUs - _startUs;
        if (dt >= _durUs) return _to;
        return _from + (_to - _from) * ((float) dt / (float) _durUs);
    }

    void reset() { _startUs = 0; }

    [[nodiscard]] uint32_t durationUs() const { return _durUs; }
    [[nodiscard]] float from() const { return _from; }
    [[nodiscard]] float to() const { return _to; }
};
