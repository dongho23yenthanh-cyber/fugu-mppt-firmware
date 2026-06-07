#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

// Pure-C++ model of a synchronous buck converter. No Arduino, no IDF, no FreeRTOS.
// Wire-up (PWM input, ADC output, conf parsing) lives in src/pwm/vconv.h,
// src/adc/vconv.h, and src/sensor_setup.cpp. Spec:
// docs/superpowers/specs/2026-05-23-virtual-converter-design.md
class VirtualConverter {
public:
    struct PwmState {
        uint16_t pwmMax = 0;
        uint16_t pwmCtrl = 0;   // HS on-count (buck)
        uint16_t pwmRect = 0;   // LS on-count (buck)
        uint32_t pwmFreq = 0;   // Hz
    };

    // PV: single-diode-ish exponential parameterized by Isc, Voc, k = V_mpp/Voc.
    // alpha is solved from k once at setPv-time.
    void setPv(float isc, float voc, float k) {
        isc_ = isc; voc_ = voc; pvK_ = k;
        // Solve alpha * (1 - k) = ln(1 + k * alpha) by Newton iteration.
        // f(a)  = a*(1-k) - ln(1 + k*a)
        // f'(a) = (1-k) - k / (1 + k*a)
        float a = (k > 0.0f && k < 1.0f) ? (1.0f / (1.0f - k)) : 1.0f;
        for (int i = 0; i < 12; ++i) {
            float fa = a * (1.0f - k) - std::log(1.0f + k * a);
            float fp = (1.0f - k) - k / (1.0f + k * a);
            if (std::fabs(fp) < 1e-9f) break;
            float step = fa / fp;
            a -= step;
            if (a < 1e-3f) a = 1e-3f;
            if (std::fabs(step) < 1e-7f) break;
        }
        pvAlpha_ = a;
        pvNorm_ = 1.0f / (1.0f - std::exp(-a));
    }

    [[nodiscard]] float pvCurrent(float v) const {
        if (v >= voc_) return 0.0f;
        if (v <= 0.0f) return isc_;
        float i = isc_ * (1.0f - std::exp(pvAlpha_ * (v - voc_) / voc_)) * pvNorm_;
        if (i < 0.0f) return 0.0f;
        if (i > isc_) return isc_;
        return i;
    }

    void setBat(float vbat, float rbat) {
        vbat_ = vbat;
        // Guard against r_bat → 0: divides in stepOneCycle (BE coefficient and
        // kVMin branch) would produce inf/NaN. 1 µΩ floor is well below any
        // realistic short impedance and keeps the math finite.
        rbat_ = (rbat < 1e-6f) ? 1e-6f : rbat;
    }
    // Pluggable V_bat ripple shape: maps phase [0,2pi) -> a zero-mean waveform in ~[-1,1], scaled
    // by vbat_ac_amp. Add a new noise model by writing one of these (anywhere) and passing it to
    // setBatRippleShape(); for conf/runtime selection by integer id, also list it in shapeFromId().
    using RippleShape = float (*)(float phase);
    static float shapeSine(float phase) { return std::sin(phase); }                 // inverter (cos 2*line)
    static float shapeAbsSin(float phase) {                                          // rectifier load |sin|
        return 2.0f * (std::fabs(std::sin(phase * 0.5f)) - 0.63661977f);            // zero-mean, ~20% 2nd harm
    }
    static RippleShape shapeFromId(int id) {
        switch (id) {
            case 1:  return &VirtualConverter::shapeAbsSin;
            default: return &VirtualConverter::shapeSine;
        }
    }

    // shape: built-in id (0 = sine inverter, 1 = |sin| rectifier). For a custom model use
    // setBatRippleShape() instead.
    void setBatRipple(float amp, float freq, int shape = 0) {
        vbatAcAmp_ = amp; vbatAcFreq_ = freq; vbatAcShape_ = shape;
        vbatAcShapeFn_ = shapeFromId(shape);
        oscStep_ = -1.0f; // force sine-oscillator re-seed (freq/shape may have changed)
    }
    // Plug in an arbitrary ripple model (overrides the built-in shape; getVbatAcShape() -> -1).
    void setBatRippleShape(RippleShape fn) {
        if (fn) { vbatAcShapeFn_ = fn; vbatAcShape_ = -1; oscStep_ = -1.0f; }
    }
    void setPassives(float c_in, float c_out, float l) { cIn_ = c_in; cOut_ = c_out; l_ = l; }
    void setVin(float v)  { vIn_ = v; }
    void setVout(float v) { vOut_ = v; }
    [[nodiscard]] float getVin()  const { return vIn_; }
    [[nodiscard]] float getVout() const { return vOut_; }
    [[nodiscard]] float getIL()   const { return iLEnd_; }
    [[nodiscard]] float getIsc()  const { return isc_; }
    [[nodiscard]] float getVoc()  const { return voc_; }
    [[nodiscard]] float getPvK()  const { return pvK_; }
    [[nodiscard]] float getVbat() const { return vbat_; }
    [[nodiscard]] float getRbat() const { return rbat_; }
    [[nodiscard]] float getCin()  const { return cIn_; }
    [[nodiscard]] float getCout() const { return cOut_; }
    [[nodiscard]] float getL()    const { return l_; }
    [[nodiscard]] float getVbatAcAmp()  const { return vbatAcAmp_; }
    [[nodiscard]] float getVbatAcFreq() const { return vbatAcFreq_; }
    [[nodiscard]] int   getVbatAcShape() const { return vbatAcShape_; }

    void setPwm(const PwmState &s) { pwm_ = s; }
    [[nodiscard]] const PwmState &getPwm() const { return pwm_; }
    [[nodiscard]] float getIinAvg()  const { return iInAvg_; }
    [[nodiscard]] float getIoutAvg() const { return iOutAvg_; }
    [[nodiscard]] bool  inDcm() const     { return dcm_; }
    [[nodiscard]] bool  errored() const   { return errored_; }

    // Set HS/LS on-counts from PwmDriver shim. ch=0 -> Ctrl (HS), ch=1 -> Rect (LS).
    // The shim is responsible for translating LEDC's hpoint/duty pair (and EnLogic's
    // cumulative single-arg form) into an absolute on-count before calling this.
    void setPwmCount(uint8_t ch, uint32_t on) {
        if (ch == 0) pwm_.pwmCtrl = (uint16_t) on;
        else if (ch == 1) pwm_.pwmRect = (uint16_t) on;
    }

    void setPwmMax(uint16_t m)   { pwm_.pwmMax = m; }
    void setPwmFreq(uint32_t hz) { pwm_.pwmFreq = hz; }

    void stopChannel(uint8_t ch) {
        if (ch == 0) pwm_.pwmCtrl = 0;
        else if (ch == 1) pwm_.pwmRect = 0;
    }

    // Advance the model by dt_s seconds. Without prior pwmFreq, falls back.
    void stepSeconds(float dt_s, uint32_t pwmFreqFallback);

private:
    // PV
    float isc_ = 8.0f;
    float voc_ = 40.0f;
    float pvK_ = 0.8f;
    float pvAlpha_ = 11.5f;
    float pvNorm_  = 1.0f;

    // Battery + passives
    float vbat_ = 28.0f;
    float rbat_ = 0.05f;
    float vbatAcAmp_  = 0.0f;
    float vbatAcFreq_ = 100.0f;
    int   vbatAcShape_ = 0; // built-in id (0=sine, 1=|sin|); -1 = custom plugged via setBatRippleShape
    RippleShape vbatAcShapeFn_ = &VirtualConverter::shapeSine;
    float vbatAcPhase_ = 0.0f;
    float cIn_  = 470e-6f;
    float cOut_ = 470e-6f;
    float l_    = 50e-6f;

    // State
    float vIn_   = 0.0f;
    float vOut_  = 0.0f;
    float iLEnd_ = 0.0f;
    PwmState pwm_{};
    float iInAvg_  = 0.0f;
    float iOutAvg_ = 0.0f;
    bool  dcm_     = false;
    bool  errored_ = false;

    // Cached reciprocals — the LX7 FPU has no hardware float divide, so every `/` is a software
    // __divsf3 call (~tens of cycles). stepOneCycle runs ~pwmFreq/adc_freq times per ADC sample, so
    // recomputing per cycle was the dominant cost. These are rebuilt only when an input changes
    // (see the guard in stepOneCycle); across the steady N-cycle loop every divide becomes a mul.
    float cycT_ = 0.0f, cycL_ = 0.0f, cycCin_ = 0.0f, cycCout_ = 0.0f, cycRbat_ = 0.0f;
    uint16_t cycPmax_ = 0xFFFFu;
    float invL_ = 0.0f, invCin_ = 0.0f, invCout_ = 0.0f, invT_ = 0.0f;
    float aOut_ = 0.0f, inv1pAout_ = 1.0f, invPmax_ = 0.0f;

    // Recursive sine oscillator for the built-in inverter ripple: advances (sin,cos) of the ripple
    // phase by a fixed per-cycle rotation (4 mul + 2 add), avoiding a per-cycle sinf. Re-seeded when
    // the rotation step changes; |sin| / custom shapes still go through the function pointer.
    float oscS_ = 0.0f, oscC_ = 1.0f, rotS_ = 0.0f, rotC_ = 1.0f, oscStep_ = -1.0f;

    void stepOneCycle(float T);
};

// Singleton. Defined in src/sim/vconv.cpp.
extern VirtualConverter g_vconv;
