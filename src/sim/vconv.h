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

    void setBat(float vbat, float rbat) { vbat_ = vbat; rbat_ = rbat; }
    void setPassives(float c_in, float c_out, float l) { cIn_ = c_in; cOut_ = c_out; l_ = l; }
    void setVin(float v) { vIn_ = v; }
    void setVout(float v) { vOut_ = v; }
    [[nodiscard]] float getVin() const  { return vIn_; }
    [[nodiscard]] float getVout() const { return vOut_; }
    [[nodiscard]] float getIL() const   { return iLEnd_; }

    void setPwm(const PwmState &s) { pwm_ = s; }
    [[nodiscard]] float getIinAvg() const  { return iInAvg_; }
    [[nodiscard]] float getIoutAvg() const { return iOutAvg_; }
    [[nodiscard]] bool  inDcm() const      { return dcm_; }

    // Advance the model by dt_s seconds. Without PwmState updates, uses last
    // latched pwm (zero by default -> converter idle, no I_L, I_in=I_out=0,
    // caps drift toward source/sink).
    void stepSeconds(float dt_s, uint32_t pwmFreqFallback);

private:
    // PV
    float isc_ = 8.0f;
    float voc_ = 40.0f;
    float pvK_ = 2.0f;

    // Battery + passives
    float vbat_ = 28.0f;
    float rbat_ = 0.05f;
    float cIn_  = 470e-6f;
    float cOut_ = 470e-6f;
    float l_    = 50e-6f;

    // State
    float vIn_   = 0.0f;
    float vOut_  = 0.0f;
    float iLEnd_ = 0.0f;   // coil current at end of last PWM cycle
    PwmState pwm_{};
    float iInAvg_  = 0.0f;
    float iOutAvg_ = 0.0f;
    bool  dcm_     = false;

    void stepOneCycle(float T);
};
