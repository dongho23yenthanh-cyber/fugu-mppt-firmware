#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

// Pure-C++ model of a synchronous buck converter. No Arduino, no IDF, no FreeRTOS.
// Wire-up (PWM input, ADC output, conf parsing) lives in src/pwm/vconv.h,
// src/adc/vconv.h, and src/main.cpp. Spec:
// docs/superpowers/specs/2026-05-23-virtual-converter-design.md
class VirtualConverter {
public:
    struct PwmState {
        uint16_t pwmMax = 0;
        uint16_t pwmCtrl = 0;   // HS on-count (buck)
        uint16_t pwmRect = 0;   // LS on-count (buck)
        uint32_t pwmFreq = 0;   // Hz
    };

    void setPv(float isc, float voc, float k) {
        isc_ = isc; voc_ = voc; pvK_ = k;
    }

    // Single-diode-ish exponential. Clamped to [0, isc].
    [[nodiscard]] float pvCurrent(float v) const {
        if (v >= voc_) return 0.0f;
        float i = isc_ * (1.0f - std::exp((v - voc_) / pvK_));
        if (i < 0.0f) i = 0.0f;
        if (i > isc_) i = isc_;
        return i;
    }

private:
    // PV
    float isc_ = 8.0f;
    float voc_ = 40.0f;
    float pvK_ = 2.0f;
};
