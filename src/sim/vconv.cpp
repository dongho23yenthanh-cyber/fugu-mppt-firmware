#include "vconv.h"

void VirtualConverter::stepSeconds(float dt_s, uint32_t pwmFreqFallback) {
    if (dt_s <= 0.0f) return;
    uint32_t freq = pwm_.pwmFreq ? pwm_.pwmFreq : pwmFreqFallback;
    if (freq < 1000) freq = 39000;   // sane default if all else fails
    const float T = 1.0f / static_cast<float>(freq);

    // Number of PWM cycles to simulate. Round to nearest, min 1.
    long n = std::lround(dt_s / T);
    if (n < 1) n = 1;

    for (long i = 0; i < n; ++i) {
        // No coil action until PWM is wired (Task 3). For now, Iin = Iout = 0.
        const float Iin = 0.0f;
        const float Iout = 0.0f;

        // Source/sink + cap dynamics (forward-Euler per PWM cycle).
        const float Ipv = pvCurrent(vIn_);
        // I_bat: backflow clamp only when converter disabled.
        float Ibat = (vOut_ - vbat_) / rbat_;
        if (pwm_.pwmCtrl == 0 && Ibat < 0.0f) Ibat = 0.0f;

        vIn_  += (Ipv  - Iin)  * T / cIn_;
        vOut_ += (Iout - Ibat) * T / cOut_;

        // Numerical guards (see spec §Numerical guards).
        if (vIn_ < 0.0f) vIn_ = 0.0f;
        if (vIn_ > voc_ * 1.05f) vIn_ = voc_ * 1.05f;
        if (vOut_ < 0.0f) vOut_ = 0.0f;
        if (vOut_ > vbat_ * 2.0f) vOut_ = vbat_ * 2.0f;
    }
}
