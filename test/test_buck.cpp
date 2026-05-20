// Tests for src/buck.h SynchronousConverter diode-emulation math (the safety-critical
// reverse-current-prevention path): ripple current, CCM/DCM decision + hysteresis, and
// the rect/ctrl duty ratio. init() is driven from in-memory ConfFiles; skip_assert=1
// bypasses the GPIO pin-state checks, and init_pwm configures the real LEDC on-target.

#include <unity.h>
#include <esp_log.h>
#include <Arduino.h>

#include <unordered_map>
#include <string>
#include <stdexcept>

#include "util.h" // defines ESP_ERROR_CHECK_THROW used by pwm/ledc.h (included from buck.h)
#include "buck.h"

// fL = pwm_freq * L0 * 0.95 = 39000 * 50e-6 * 0.95 = 1.8525
// rippleCurrent(60,30) = 30/fL * (1 - 30/60) ~= 8.1 A
static void initConv(SynchronousConverter &c) {
    ConfFile converterConf{std::unordered_map<std::string, std::string>{{"topo", "buck"}}};
    ConfFile coilConf{std::unordered_map<std::string, std::string>{{"L0", "50e-6"}}};
    ConfFile boardConf{std::unordered_map<std::string, std::string>{
        {"pwm_freq", "39000"},
        {"pwm_driver_logic", "HiLi"},
        {"pwm_hi", "1"},
        {"pwm_li", "2"},
        {"skip_assert", "1"},
    }};
    c.init(converterConf, boardConf, coilConf);
}

void test_buck_ripple_current() {
    SynchronousConverter c;
    initConv(c);
    TEST_ASSERT_FALSE(c.boost());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 8.1f, c.rippleCurrent(60.f, 30.f));
}

void test_buck_dcm_ccm_transition_and_hysteresis() {
    SynchronousConverter c;
    initConv(c);
    // ripple ~8.1 A; DCM while ripple > il*2, CCM above, with 1.8x release hysteresis
    c.updateSyncRectMaxDuty(60.f, 30.f, 1.f);
    TEST_ASSERT_TRUE(c.inDCM());
    c.updateSyncRectMaxDuty(60.f, 30.f, 4.f);
    TEST_ASSERT_TRUE(c.inDCM());          // 8.1 > 4*1.8=7.2 -> stays DCM
    c.updateSyncRectMaxDuty(60.f, 30.f, 5.f);
    TEST_ASSERT_FALSE(c.inDCM());         // 8.1 < 5*1.8=9 -> CCM
    c.updateSyncRectMaxDuty(60.f, 30.f, 10.f);
    TEST_ASSERT_FALSE(c.inDCM());
}

void test_buck_rect_ctrl_ratio() {
    SynchronousConverter c;
    initConv(c);
    // buck: rectCtrlRatio(m) = 1/m - 1
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, c.rectCtrlRatio(0.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f, c.rectCtrlRatio(0.25f));
}

void test_buck_current_sweep_no_crash() {
    SynchronousConverter c;
    initConv(c);
    // mirror the host-stub sweep: must not trip any internal assert / range error
    for (float il = 0.1f; il < 30.f; il *= 1.1f)
        c.updateSyncRectMaxDuty(60.f, 30.f, il);
    TEST_PASS();
}
