// Tests for the adaptive inverter-ripple notch:
//   - RippleFreqDetector (src/math/ripple_freq.h): finds the dominant tone in a band, including
//     on REAL Vout samples captured from converter "fry" with a ~2 kW inverter on its DC bus.
//   - End-to-end: a NotchFilter retuned to the detected frequency removes the ripple that the
//     fixed 100 Hz notch (50 Hz-mains assumption) lets straight through.
// Pure math; no hardware. The detector half also builds on host (see test/host-stub).

#include <unity.h>
#include <esp_log.h>
#include <Arduino.h>
#include <cmath>

#include "math/ripple_freq.h"
#include "math/notch.h"
#include "data/fry_vout_inverter_646.h"

// feed a clean tone (amp on a DC offset, optional noise) at fs through the detector and return
// the last block estimate
static void detectTone(float fs, float ftone, float amp, float dc, float &hz, float &snr) {
    RippleFreqDetector<61> det;
    uint16_t blockN = (uint16_t) (fs * 0.75f);
    det.configure(fs, 80.f, 140.f, blockN);
    hz = NAN; snr = 0;
    unsigned seed = 12345;
    for (int n = 0; n < blockN * 4; ++n) {
        seed = seed * 1103515245u + 12345u;          // cheap deterministic noise
        float noise = ((int) (seed >> 16) % 1000 - 500) * 1e-4f; // ~+-0.05
        float x = dc + noise + (amp > 0 ? amp * sinf(2 * (float) M_PI * ftone * n / fs) : 0.f);
        det.push(x);
        float h, s;
        if (det.poll(h, s)) { hz = h; snr = s; }
    }
}

void test_ripple_detects_100hz() {
    float hz, snr;
    detectTone(640.f, 100.f, 0.5f, 26.f, hz, snr);
    TEST_ASSERT_FLOAT_WITHIN(1.5f, 100.f, hz);
    TEST_ASSERT_TRUE(snr > 15.f);
}

void test_ripple_detects_120hz() {
    float hz, snr;
    detectTone(640.f, 120.f, 0.5f, 26.f, hz, snr);
    TEST_ASSERT_FLOAT_WITHIN(1.5f, 120.f, hz);
    TEST_ASSERT_TRUE(snr > 15.f);
}

// sub-bin frequency must be resolved by parabolic interpolation (bins are 1 Hz apart)
void test_ripple_subbin_interpolation() {
    float hz, snr;
    detectTone(640.f, 117.4f, 0.5f, 26.f, hz, snr);
    TEST_ASSERT_FLOAT_WITHIN(0.8f, 117.4f, hz);
}

// large DC offset (Vout ~ tens of volts) must not be mistaken for a tone / leak into the band
void test_ripple_ignores_dc_no_false_positive() {
    float hz, snr;
    detectTone(640.f, 0.f, 0.f, 26.f, hz, snr); // pure DC + noise, no ripple
    TEST_ASSERT_TRUE(snr < 15.f);               // below the trust threshold -> notch won't move
}

// the actual field capture: detector must land on the ~126 Hz inverter tone the host FFT found
void test_ripple_detects_real_fry_capture() {
    RippleFreqDetector<61> det;
    uint16_t blockN = (uint16_t) (FRY_VOUT_FS_HZ * 0.75f);
    det.configure(FRY_VOUT_FS_HZ, 80.f, 140.f, blockN);
    float hz = NAN, snr = 0, h, s;
    for (int n = 0; n < FRY_VOUT_N; ++n) {
        det.push((float) FRY_VOUT_INVERTER[n]);
        if (det.poll(h, s)) { hz = h; snr = s; }
    }
    TEST_ASSERT_FLOAT_WITHIN(2.0f, FRY_VOUT_RIPPLE_HZ, hz); // ~126 Hz
    TEST_ASSERT_TRUE(snr > 15.f);
}

// residual AC power (variance about a slow mean) of a signal after a notch
static float residualAc(const int16_t *x, int n, float fNorm) {
    NotchFilter nf;
    nf.begin(fNorm);
    float mean = 0; // slow one-pole to strip DC
    double var = 0; int cnt = 0;
    for (int i = 0; i < n; ++i) {
        float in = (float) x[i], out;
        nf.filter(&in, &out, 1);
        if (i < 200) { mean = out; continue; } // warmup
        mean += 0.01f * (out - mean);
        float d = out - mean;
        var += (double) d * d; ++cnt;
    }
    return cnt ? (float) (var / cnt) : 0.f;
}

// THE regression for this bug: on the real capture, a notch tuned to the detected ~126 Hz
// suppresses the ripple, while the fixed 100 Hz notch (old hard-coded behavior) barely touches it.
void test_adaptive_notch_beats_fixed_100hz_on_real_data() {
    RippleFreqDetector<61> det;
    uint16_t blockN = (uint16_t) (FRY_VOUT_FS_HZ * 0.75f);
    det.configure(FRY_VOUT_FS_HZ, 80.f, 140.f, blockN);
    float hz = NAN, h, s;
    for (int n = 0; n < FRY_VOUT_N; ++n) {
        det.push((float) FRY_VOUT_INVERTER[n]);
        if (det.poll(h, s)) hz = h;
    }

    float resFixed = residualAc(FRY_VOUT_INVERTER, FRY_VOUT_N, 100.f / FRY_VOUT_FS_HZ);
    float resAdaptive = residualAc(FRY_VOUT_INVERTER, FRY_VOUT_N, hz / FRY_VOUT_FS_HZ);

    // adaptive notch leaves at least 4x less ripple energy than the mistuned 100 Hz notch
    TEST_ASSERT_TRUE(resAdaptive * 4.f < resFixed);
}
