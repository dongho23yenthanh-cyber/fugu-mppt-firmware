// Host-side unit test for VirtualConverter. Run with the clang++ command
// in docs/superpowers/plans/2026-05-23-virtual-converter.md (top of file).
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../../src/sim/vconv.h"

static bool approxEq(float a, float b, float rtol = 1e-3f, float atol = 1e-6f) {
    return std::abs(a - b) <= atol + rtol * std::abs(b);
}

static void test_pv_iv_curve() {
    VirtualConverter vc;
    vc.setPv(/*isc=*/8.0f, /*voc=*/40.0f, /*k=*/2.0f);

    assert(approxEq(vc.pvCurrent(0.0f), 8.0f, /*rtol*/0.01f));
    assert(vc.pvCurrent(40.0f) <= 0.001f);
    assert(vc.pvCurrent(50.0f) == 0.0f);            // clamp above voc
    assert(vc.pvCurrent(-1.0f) >= 0.0f && vc.pvCurrent(-1.0f) <= 8.0f);

    float prev = vc.pvCurrent(0.0f);
    for (float v = 1.0f; v <= 39.0f; v += 1.0f) {
        float i = vc.pvCurrent(v);
        assert(i <= prev + 1e-4f);                  // monotone non-increasing
        prev = i;
    }
}

static void test_battery_cap_dynamics() {
    VirtualConverter vc;
    vc.setPv(8.0f, 40.0f, 2.0f);
    vc.setBat(/*vbat=*/28.0f, /*rbat=*/0.05f);
    vc.setPassives(/*c_in=*/470e-6f, /*c_out=*/470e-6f, /*L=*/50e-6f);

    // No PWM applied yet -> caps integrate net source/sink current.
    vc.setVin(20.0f);
    vc.setVout(28.0f);
    // step 1 ms = ~39 PWM cycles at 39 kHz. Without PWM events, Iin/Iout=0,
    // so Vin should rise toward Voc (PV charges Cin), Vout should stay ~Vbat.
    const float vin0 = vc.getVin();
    const float vout0 = vc.getVout();
    vc.stepSeconds(/*dt=*/1e-3f, /*pwmFreqFallback=*/39000);

    assert(vc.getVin() > vin0);
    assert(vc.getVin() <= 40.5f);
    assert(approxEq(vc.getVout(), vout0, 0.05f));
}

int main() {
    test_pv_iv_curve();
    test_battery_cap_dynamics();
    std::printf("vconv-test: all asserts passed\n");
    return 0;
}
