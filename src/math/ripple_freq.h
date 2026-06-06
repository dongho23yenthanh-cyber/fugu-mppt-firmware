#pragma once

#include <cmath>
#include <cstdint>

/**
 * Streaming multi-bin Goertzel scan that locates the dominant ripple tone (e.g. the 2x-line
 * ripple an inverter draws onto the DC bus) within a fixed frequency band, so a notch filter
 * can be auto-tuned to it instead of assuming a fixed mains frequency.
 *
 * push(x) is called per sample on the RT core (a handful of FLOPs per bin); every blockN samples
 * a block estimate becomes ready (poll()). DC and slow drift are removed by a one-pole high-pass
 * before the Goertzel banks so a large DC offset (Vout ~ tens of volts) does not leak into the
 * band and swamp a small ripple. Confidence is reported as the peak-to-mean magnitude ratio so
 * the caller can ignore the estimate when no real tone is present.
 */
template<int NBINS = 61>
class RippleFreqDetector {
    float coef_[NBINS];          // 2*cos(2*pi*f_k/fs)
    float q1_[NBINS], q2_[NBINS];
    float fLo_ = 0, fStep_ = 0;
    float dc_ = NAN;             // one-pole DC estimate (high-pass)
    uint16_t blockN_ = 512, pos_ = 0;
    float hz_ = NAN, snr_ = 0;
    bool ready_ = false;

public:
    [[nodiscard]] bool configured() const { return fStep_ > 0; }

    // fs: sample rate of the fed channel [Hz]; band [fLo,fHi] scanned across NBINS bins.
    void configure(float fs, float fLo, float fHi, uint16_t blockN) {
        fLo_ = fLo;
        fStep_ = (fHi - fLo) / (NBINS - 1);
        blockN_ = blockN;
        for (int k = 0; k < NBINS; ++k) {
            float f = fLo + fStep_ * k;
            coef_[k] = 2.0f * cosf(2.0f * (float) M_PI * f / fs);
            q1_[k] = q2_[k] = 0;
        }
        pos_ = 0;
        dc_ = NAN;
        ready_ = false;
    }

    inline void push(float x) {
        // one-pole DC removal (~0.1 Hz corner) so the band sees only ripple
        if (std::isnan(dc_)) dc_ = x;
        else dc_ += 0.001f * (x - dc_);
        const float s = x - dc_;

        for (int k = 0; k < NBINS; ++k) {
            float q0 = coef_[k] * q1_[k] - q2_[k] + s;
            q2_[k] = q1_[k];
            q1_[k] = q0;
        }
        if (++pos_ >= blockN_) finalize();
    }

    bool poll(float &hz, float &snr) {
        if (!ready_) return false;
        ready_ = false;
        hz = hz_;
        snr = snr_;
        return true;
    }

private:
    void finalize() {
        float mags[NBINS];
        float best = 0, sum = 0;
        int bi = 0;
        for (int k = 0; k < NBINS; ++k) {
            float m = q1_[k] * q1_[k] + q2_[k] * q2_[k] - coef_[k] * q1_[k] * q2_[k];
            if (m < 0) m = 0;
            mags[k] = m;
            sum += m;
            if (m > best) { best = m; bi = k; }
            q1_[k] = q2_[k] = 0;
        }
        pos_ = 0;

        float meanOther = (sum - best) / (NBINS - 1) + 1e-12f;
        snr_ = best / meanOther;

        // parabolic interpolation on amplitude (sqrt of power) for sub-bin resolution
        float delta = 0;
        if (bi > 0 && bi < NBINS - 1) {
            float a = sqrtf(mags[bi - 1]), b = sqrtf(mags[bi]), c = sqrtf(mags[bi + 1]);
            float den = a - 2 * b + c;
            if (den != 0) {
                delta = 0.5f * (a - c) / den;
                if (delta > 1) delta = 1; else if (delta < -1) delta = -1;
            }
        }
        hz_ = fLo_ + fStep_ * (bi + delta);
        ready_ = true;
    }
};
