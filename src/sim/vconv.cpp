#include "vconv.h"

namespace {
// Trapezoid area under a piecewise-linear segment from y0 to y1 over duration t.
inline float trapArea(float y0, float y1, float t) {
    return 0.5f * (y0 + y1) * t;
}
}

void VirtualConverter::stepOneCycle(float T) {
    const float L = l_;
    const float dHS = pwm_.pwmMax ? float(pwm_.pwmCtrl) / float(pwm_.pwmMax) : 0.0f;
    const float dLS = pwm_.pwmMax ? float(pwm_.pwmRect) / float(pwm_.pwmMax) : 0.0f;
    const float tHS = dHS * T;
    const float tLS = dLS * T;
    const float tOff = std::max(0.0f, T - tHS - tLS);

    // Phase 1: HS on.
    const float a = iLEnd_;
    const float b = (tHS > 0.0f && L > 0.0f)
                        ? a + (vIn_ - vOut_) / L * tHS
                        : a;
    const float areaHS = trapArea(a, b, tHS);

    // Phase 2: LS on. Allow signed I_L (reverse current in forced PWM).
    float c;
    float areaLS;
    if (tLS > 0.0f && L > 0.0f) {
        c = b - vOut_ / L * tLS;
        if (b > 0.0f && c < 0.0f) {
            // Natural zero-crossing inside phase 2 -> firmware/diode holds I_L=0.
            const float tZc = b * L / vOut_;
            areaLS = trapArea(b, 0.0f, tZc);
            c = 0.0f;
        } else {
            areaLS = trapArea(b, c, tLS);
        }
    } else {
        c = b;
        areaLS = 0.0f;
    }

    // Phase 3: both switches off, body diodes conduct toward zero.
    float cEnd = c;
    float areaOff = 0.0f;
    if (tOff > 0.0f && L > 0.0f) {
        if (c > 0.0f) {
            const float tToZero = c * L / vOut_;
            if (tToZero < tOff) {
                areaOff = trapArea(c, 0.0f, tToZero);
                cEnd = 0.0f;
            } else {
                const float cAtEnd = c - vOut_ / L * tOff;
                areaOff = trapArea(c, cAtEnd, tOff);
                cEnd = cAtEnd;
            }
        } else if (c < 0.0f) {
            const float tToZero = -c * L / vIn_;
            if (tToZero < tOff) {
                areaOff = trapArea(c, 0.0f, tToZero);
                cEnd = 0.0f;
            } else {
                const float cAtEnd = c + vIn_ / L * tOff;
                areaOff = trapArea(c, cAtEnd, tOff);
                cEnd = cAtEnd;
            }
        }
    }

    iInAvg_  = (T > 0.0f) ? (areaHS / T) : 0.0f;
    iOutAvg_ = (T > 0.0f) ? ((areaHS + areaLS + areaOff) / T) : 0.0f;
    iLEnd_ = cEnd;

    // Source/sink + cap dynamics.
    const float Ipv = pvCurrent(vIn_);
    float Ibat = (vOut_ - vbat_) / rbat_;
    if (pwm_.pwmCtrl == 0 && Ibat < 0.0f) Ibat = 0.0f;

    vIn_  += (Ipv      - iInAvg_)  * T / cIn_;
    vOut_ += (iOutAvg_ - Ibat)     * T / cOut_;

    // Numerical guards.
    if (vIn_ < 0.0f) vIn_ = 0.0f;
    if (vIn_ > voc_ * 1.05f) vIn_ = voc_ * 1.05f;
    if (vOut_ < 0.0f) vOut_ = 0.0f;
    if (vOut_ > vbat_ * 2.0f) vOut_ = vbat_ * 2.0f;
}

void VirtualConverter::stepSeconds(float dt_s, uint32_t pwmFreqFallback) {
    if (dt_s <= 0.0f) return;
    uint32_t freq = pwm_.pwmFreq ? pwm_.pwmFreq : pwmFreqFallback;
    if (freq < 1000) freq = 39000;
    const float T = 1.0f / static_cast<float>(freq);

    long n = std::lround(dt_s / T);
    if (n < 1) n = 1;
    for (long i = 0; i < n; ++i) stepOneCycle(T);
}
