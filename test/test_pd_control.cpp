// Tests for src/pd_control.h (PD_Control, PD_Control_SmoothSetpoint). Pure math.
// pd_control.h relies on the includer for EWMA / std::isnan / NAN.

#include <unity.h>
#include <cmath>
#include <esp_log.h>
#include <Arduino.h>

#include "math/statmath.h"
#include "pd_control.h"

void test_pd_proportional_only() {
    PD_Control pd{2.f, 0.f, false};
    // first update: de forced to 0 -> output is purely Kp*e
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.f * (5.f - 3.f), pd.update(3.f, 5.f));
    // steady measurement: de == 0, same output
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.f, pd.update(3.f, 5.f));
}

void test_pd_first_tick_has_zero_derivative() {
    PD_Control pd{0.f, 1.f, false}; // derivative-only
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.f, pd.update(0.f, 0.f)); // _prevE NaN -> de=0
}

void test_pd_derivative_on_step() {
    PD_Control pd{0.f, 1.f, false};
    pd.update(0.f, 0.f);                                  // e=0, primes _prevE
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.f, pd.update(0.f, 2.f)); // de = 2-0 = 2
}

void test_pd_normalize_relative_error() {
    PD_Control pd{1.f, 0.f, true};
    // normalize: measurement/=setpoint, setpoint=1 -> e = 1 - 0.5 = 0.5
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, pd.update(4.f, 8.f));
}

void test_pd_reset_clears_derivative() {
    PD_Control pd{0.f, 1.f, false};
    pd.update(0.f, 0.f);
    pd.update(0.f, 5.f); // _prevE now 5
    pd.reset();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.f, pd.update(0.f, 5.f)); // de=0 again after reset
}

void test_pd_smooth_setpoint_lags_step() {
    // SmoothSetpoint forces normalize=true, so output is the relative error
    // e = 1 - measurement/smoothed_setpoint. With a fixed measurement, a setpoint
    // step appears only through the EWMA-smoothed setpoint, which lags.
    PD_Control_SmoothSetpoint pd{1.f, 0.f, 10};
    float first = pd.update(5.f, 10.f);   // seeds smoothed setpoint at 10: e = 1 - 5/10 = 0.5
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, first);
    float stepped = pd.update(5.f, 20.f); // un-smoothed would give 1 - 5/20 = 0.75
    TEST_ASSERT_TRUE(stepped > 0.5f && stepped < 0.75f); // lag: between old and new
}
