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

int main() {
    test_pv_iv_curve();
    std::printf("vconv-test: all asserts passed\n");
    return 0;
}
