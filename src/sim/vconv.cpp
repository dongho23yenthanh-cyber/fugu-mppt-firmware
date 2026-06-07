#include "vconv.h"

#include <cstdio>

VirtualConverter g_vconv;

namespace {
inline float trapArea(float y0, float y1, float t) {
    return 0.5f * (y0 + y1) * t;
}
constexpr float kTwoPi = 6.28318530717958647692f;
}

void VirtualConverter::stepOneCycle(float T) {
    const float L = l_;
    const uint16_t pmax = pwm_.pwmMax;
    const uint16_t ctrl = pwm_.pwmCtrl;
    const uint16_t rect = pwm_.pwmRect;

    // Rebuild reciprocals only when an input changes; the steady N-cycle loop then divides nothing
    // (LX7 has no hw float divide, so each `/` was a software __divsf3). 6 compares << 18 divisions.
    if (T != cycT_ || pmax != cycPmax_ || l_ != cycL_ || cIn_ != cycCin_ ||
        cOut_ != cycCout_ || rbat_ != cycRbat_) {
        cycT_ = T; cycPmax_ = pmax; cycL_ = l_; cycCin_ = cIn_; cycCout_ = cOut_; cycRbat_ = rbat_;
        invL_    = (l_   > 0.0f) ? 1.0f / l_   : 0.0f;
        invCin_  = (cIn_ > 0.0f) ? 1.0f / cIn_ : 0.0f;
        invCout_ = (cOut_> 0.0f) ? 1.0f / cOut_: 0.0f;
        invT_    = 1.0f / T;
        aOut_    = T / (rbat_ * cOut_);
        inv1pAout_ = 1.0f / (1.0f + aOut_);
        invPmax_ = pmax ? 1.0f / (float) pmax : 0.0f;
    }

    // Spec invariant: pwmCtrl + pwmRect <= pwmMax. Idle while violated; recover
    // as soon as a subsequent update brings the counts back in range.
    if (pmax > 0 && (uint32_t) ctrl + (uint32_t) rect > pmax) {
        if (!errored_) {
            std::printf("vconv: pwmCtrl(%u)+pwmRect(%u) > pwmMax(%u)\n",
                        (unsigned) ctrl, (unsigned) rect, (unsigned) pmax);
            errored_ = true;
        }
        iInAvg_ = 0.0f;
        iOutAvg_ = 0.0f;
        return;
    }
    if (errored_) {
        std::printf("vconv: pwm counts back in range, resuming\n");
        errored_ = false;
    }

    // Mains ripple advances regardless of regime so disturbance is steady. Skip
    // when v_bat == 0 (open-circuit preset): a bipolar swing around zero is unphysical.
    float vBatEff = vbat_;
    if (vbatAcAmp_ != 0.0f && vbat_ > 0.0f) {
        const float step = kTwoPi * vbatAcFreq_ * T;
        if (vbatAcShape_ == 0) {
            // Built-in sine via recursive oscillator — no per-cycle sinf. (Re)seed on freq change.
            if (step != oscStep_) {
                oscStep_ = step;
                rotC_ = std::cos(step); rotS_ = std::sin(step);
                oscS_ = std::sin(vbatAcPhase_); oscC_ = std::cos(vbatAcPhase_);
            }
            vBatEff += vbatAcAmp_ * oscS_;
            const float ns = oscS_ * rotC_ + oscC_ * rotS_; // rotate (sin,cos) by +step
            const float nc = oscC_ * rotC_ - oscS_ * rotS_;
            const float corr = 1.5f - 0.5f * (ns * ns + nc * nc); // cheap renorm toward unit circle
            oscS_ = ns * corr; oscC_ = nc * corr;
        } else {
            vBatEff += vbatAcAmp_ * vbatAcShapeFn_(vbatAcPhase_); // |sin| / custom: keep the call
        }
        vbatAcPhase_ += step;
        if (vbatAcPhase_ > kTwoPi) vbatAcPhase_ -= kTwoPi;
        else if (vbatAcPhase_ < -kTwoPi) vbatAcPhase_ += kTwoPi;
    }

    // V_in below this means the PV is dead (no source) — phase-1 math becomes
    // degenerate, relax caps only. V_out is NOT guarded: a real short pulls
    // V_out near zero while the converter still delivers large I_L, and the
    // phase math handles V_out → 0 (phase-2/3 slopes go to zero, coil idles).
    constexpr float kVMin = 1e-4f;
    if (vIn_ < kVMin || pmax == 0 || L <= 0.0f) {
        const float Ipv = pvCurrent(vIn_);
        vIn_  += Ipv * T * invCin_;
        vOut_ = (vOut_ + aOut_ * vBatEff) * inv1pAout_;
        if (vIn_  < 0.0f) vIn_  = 0.0f;
        if (vOut_ < 0.0f) vOut_ = 0.0f;
        iInAvg_  = 0.0f;
        iOutAvg_ = 0.0f;
        iLEnd_   = 0.0f;
        dcm_     = true;
        return;
    }

    const float dHS = (float) ctrl * invPmax_;
    const float dLS = (float) rect * invPmax_;
    const float tHS = dHS * T;
    const float tLS = dLS * T;
    const float tOff = T - tHS - tLS;

    // Phase 1: HS on. dI/dt = (Vin - Vout) / L.
    const float a = iLEnd_;
    const float b = (tHS > 0.0f) ? (a + (vIn_ - vOut_) * invL_ * tHS) : a;
    const float areaHS = trapArea(a, b, tHS);

    // Phase 2: LS on. Signed trapezoid; no mode decision -- firmware picks
    // pwmRect to land near zero, forced-PWM commands past it.
    const float c = (tLS > 0.0f) ? (b - vOut_ * invL_ * tLS) : b;
    const float areaLS = trapArea(b, c, tLS);

    // Phase 3: both off. c > 0: LS body diode. c < 0: HS body diode (rev pump).
    float cEnd = c;
    float areaOff = 0.0f;
    if (tOff > 0.0f) {
        if (c > 0.0f) {
            const float slope = -vOut_ * invL_;     // <= 0; zero iff vOut_ == 0
            if (slope < 0.0f) {
                const float tZero = -c / slope;
                if (tZero < tOff) {
                    areaOff = 0.5f * c * tZero;
                    cEnd = 0.0f;
                } else {
                    cEnd = c + slope * tOff;
                    areaOff = trapArea(c, cEnd, tOff);
                }
            } else {
                // V_out == 0 (short): no back-EMF, body diode passes c through unchanged.
                cEnd = c;
                areaOff = c * tOff;
            }
        } else if (c < 0.0f) {
            const float slope = (vIn_ - vOut_) * invL_;
            if (slope > 0.0f) {
                const float tZero = -c / slope;
                if (tZero < tOff) {
                    areaOff = 0.5f * c * tZero;
                    cEnd = 0.0f;
                } else {
                    cEnd = c + slope * tOff;
                    areaOff = trapArea(c, cEnd, tOff);
                }
            } else {
                cEnd = c;
                areaOff = c * tOff;
            }
        }
    }

    iInAvg_  = areaHS * invT_;
    iOutAvg_ = (areaHS + areaLS + areaOff) * invT_;
    iLEnd_ = cEnd;
    // DCM := "coil is at zero at cycle end". c!=0 means phase 3 drove it down;
    // a==0 covers the staying-idle case (prior cycle ended at zero too).
    dcm_ = (cEnd == 0.0f) && (c != 0.0f || a == 0.0f);

    // Source/sink + cap dynamics. V_in: forward-Euler (PV+C_in time constant is
    // bounded). V_out: backward-Euler — unconditionally stable, lets r_bat go
    // arbitrarily small without crossing the T/(R·C)=2 cliff (short-circuit case).
    //   V_out_new = (V_out + T·I_out_avg/C_out + a·V_bat) / (1 + a),   a = T/(R_bat·C_out)
    const float Ipv  = pvCurrent(vIn_);

    vIn_ += (Ipv - iInAvg_) * T * invCin_;
    vOut_ = (vOut_ + (T * iOutAvg_) * invCout_ + aOut_ * vBatEff) * inv1pAout_;

    if (vIn_ < 0.0f) vIn_ = 0.0f;
    const float vInMax = voc_ * 1.05f;
    if (vIn_ > vInMax) vIn_ = vInMax;
    if (vOut_ < 0.0f) vOut_ = 0.0f;
    // Defensive ceiling: 2·V_bat for normal operation. When V_bat ≈ 0 (open-circuit
    // preset: r_bat≫1, v_bat=0) fall back to 2·Voc so V_out can charge up to the
    // firmware's OVP trip instead of being pinned at zero.
    const float vOutMax = std::max(vbat_ * 2.0f, voc_ * 2.0f);
    if (vOut_ > vOutMax) vOut_ = vOutMax;
}

void VirtualConverter::stepSeconds(float dt_s, uint32_t pwmFreqFallback) {
    if (dt_s <= 0.0f) return;
    uint32_t freq = pwm_.pwmFreq ? pwm_.pwmFreq : pwmFreqFallback;
    if (freq < 1000) freq = 39000;
    const float T = 1.0f / (float) freq;

    long n = std::lround(dt_s / T);
    if (n < 1) n = 1;
    for (long i = 0; i < n; ++i) stepOneCycle(T);
}
