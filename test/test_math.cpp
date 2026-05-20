#include <unity.h>

#include <cstdlib>
#include "math/float16.h"


float randf() {
    return static_cast <float> (rand()) / static_cast <float> (RAND_MAX);
}

void test_float16() {
    // float16 is IEEE half-precision: 10 mantissa bits -> ~2^-11 (~5e-4) relative
    // precision. 1e-6 is far below one ULP; tolerances must match the format, and
    // grow for products where input + multiply rounding compound. randf() in [0,1).
    for (auto i = 0; i < 1000; ++i) {
        auto f32 = randf();

        float16 f16 = f32;
        TEST_ASSERT_FLOAT_WITHIN(1e-3, f32, f16.toFloat());

        auto f2 = f16 * 2;
        TEST_ASSERT_FLOAT_WITHIN(2e-3, f32 * 2, f2.toFloat());

        auto f3 = f16 * f16;
        TEST_ASSERT_FLOAT_WITHIN(3e-3, f32 * f32, f3.toFloat());
    }
}
