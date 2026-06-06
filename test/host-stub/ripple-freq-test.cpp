// Host-side test for the adaptive inverter-ripple frequency detector (src/math/ripple_freq.h).
// Self-contained, no ESP-IDF, no hardware. Build + run:
//
//   c++ -std=gnu++17 -O2 -I src -I test \
//       -o /tmp/ripple-freq-test test/host-stub/ripple-freq-test.cpp && /tmp/ripple-freq-test
//
// Covers synthetic tones (50/60 Hz inverter -> 100/120 Hz, sub-bin, weak ripple, DC-only) and
// the REAL Vout capture from converter "fry" with a ~2 kW inverter on its DC bus, which the
// host FFT pins at ~126 Hz.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "math/ripple_freq.h"
#include "data/fry_vout_inverter_646.h"

static int failures = 0;
#define CHECK(cond, fmt, ...) do { \
    if (cond) { printf("  PASS  " fmt "\n", ##__VA_ARGS__); } \
    else { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); ++failures; } } while (0)

static void detectTone(float fs, float ftone, float amp, float dc, float &hz, float &snr) {
    RippleFreqDetector<61> det;
    uint16_t blockN = (uint16_t) (fs * 0.75f);
    det.configure(fs, 80.f, 140.f, blockN);
    hz = NAN; snr = 0;
    unsigned seed = 12345;
    for (int n = 0; n < blockN * 4; ++n) {
        seed = seed * 1103515245u + 12345u;
        float noise = ((int) (seed >> 16) % 1000 - 500) * 1e-4f;
        float x = dc + noise + (amp > 0 ? amp * sinf(2 * (float) M_PI * ftone * n / fs) : 0.f);
        det.push(x);
        float h, s;
        if (det.poll(h, s)) { hz = h; snr = s; }
    }
}

int main() {
    printf("ripple-freq-test (adaptive inverter-ripple detector)\n");

    float hz, snr;
    struct { float f, amp; const char *name; } tones[] = {
        {100.f, 0.5f, "50Hz inverter -> 100Hz"},
        {120.f, 0.5f, "60Hz inverter -> 120Hz"},
        {126.f, 0.4f, "odd 63Hz -> 126Hz"},
        {117.4f, 0.5f, "sub-bin 117.4Hz (parabolic)"},
        {130.f, 0.05f, "weak 0.05V ripple"},
    };
    for (auto &t : tones) {
        detectTone(640.f, t.f, t.amp, 26.f, hz, snr);
        CHECK(fabsf(hz - t.f) < 1.5f && snr > 15.f,
              "%-32s detected %.2fHz snr=%.0f (want %.1f)", t.name, hz, snr, t.f);
    }

    // DC + noise only: must NOT report a confident tone (else the notch would chase noise)
    detectTone(640.f, 0.f, 0.f, 26.f, hz, snr);
    CHECK(snr < 15.f, "DC-only no false positive       snr=%.1f (< 15)", snr);

    // real field capture
    {
        RippleFreqDetector<61> det;
        uint16_t blockN = (uint16_t) (FRY_VOUT_FS_HZ * 0.75f);
        det.configure(FRY_VOUT_FS_HZ, 80.f, 140.f, blockN);
        hz = NAN; snr = 0; float h, s;
        for (int n = 0; n < FRY_VOUT_N; ++n) {
            det.push((float) FRY_VOUT_INVERTER[n]);
            if (det.poll(h, s)) { hz = h; snr = s; }
        }
        CHECK(fabsf(hz - FRY_VOUT_RIPPLE_HZ) < 2.0f && snr > 15.f,
              "REAL fry capture detected %.2fHz snr=%.0f (host-FFT %.2fHz)",
              hz, snr, FRY_VOUT_RIPPLE_HZ);
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nOK\n", failures);
    return failures ? 1 : 0;
}
