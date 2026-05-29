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
static constexpr float kFsw = 39000.f;

static void initConvEx(SynchronousConverter &c, const char *topo, const char *bootRefreshNs) {
    ConfFile converterConf{std::unordered_map<std::string, std::string>{{"topo", topo}}};
    ConfFile coilConf{std::unordered_map<std::string, std::string>{{"L0", "50e-6"}}};
    std::unordered_map<std::string, std::string> board{
        {"pwm_freq", "39000"},
        {"pwm_driver_logic", "HiLi"},
        {"pwm_hi", "1"},
        {"pwm_li", "2"},
        {"skip_assert", "1"},
    };
    if (bootRefreshNs) board.emplace("boot_refresh_ns", bootRefreshNs);
    ConfFile boardConf{std::move(board)};
    c.init(converterConf, boardConf, coilConf);
}

static void initConv(SynchronousConverter &c) { initConvEx(c, "buck", nullptr); }

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

// --- regression: convRatioWCE clamp. The WCEF division (/0.98) inflates M; without the clamp it
//     can push M past its physical bound (buck: >1), making rectCtrlRatio() = 1/M-1 negative. ---

void test_buck_ratio_clamped_below_unity() {
    SynchronousConverter c;
    initConv(c);
    // M = vout/vin ~ 0.997 -> constrain 0.99 -> /WCEF ~1.01 -> clamp back to <1
    c.updateSyncRectMaxDuty(30.f, 29.9f, 1.f);
    TEST_ASSERT_TRUE(c.voltageRatio() > 0.f);
    TEST_ASSERT_TRUE(c.voltageRatio() < 1.f); // not pushed to/over unity
    TEST_ASSERT_TRUE(c.rectCtrlRatio(c.voltageRatio()) >= 0.f);
}

void test_buck_ratio_clamped_when_vout_ge_vin() {
    SynchronousConverter c;
    initConv(c);
    // vout >= vin -> fallback M=1.0 -> /WCEF ~1.02 -> clamp back to 0.99
    c.updateSyncRectMaxDuty(30.f, 30.f, 1.f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.99f, c.voltageRatio());
    TEST_ASSERT_TRUE(c.rectCtrlRatio(c.voltageRatio()) >= 0.f);
}

void test_buck_ratio_never_negative_sweep() {
    SynchronousConverter c;
    initConv(c);
    for (float vout = 3.f; vout <= 36.f; vout += 1.f) {
        c.updateSyncRectMaxDuty(30.f, vout, 1.f);
        TEST_ASSERT_TRUE(c.voltageRatio() > 0.f && c.voltageRatio() < 1.f);
        TEST_ASSERT_TRUE(c.rectCtrlRatio(c.voltageRatio()) >= 0.f);
    }
}

// In DCM near M=1 with a nonzero duty, the (tiny, positive) ratio must floor pwmRectMax at the
// bootstrap min. Without the clamp the negative ratio would wrap (uint16) to the full CCM
// complement (pwmMax - pwmCtrl) — the opposite of the safe short LS time.
void test_buck_dcm_rectmax_not_full_ccm_near_unity() {
    SynchronousConverter c;
    initConv(c);
    c.pwmPerturb(c.pwmCtrlMax / 2); // nonzero duty so the DCM ratio actually multiplies
    c.updateSyncRectMaxDuty(30.f, 29.9f, 0.05f); // il<0.1 -> DCM, il>=0.01 & vl>=1 -> ratio path
    TEST_ASSERT_TRUE(c.inDCM());
    TEST_ASSERT_EQUAL_UINT(c.getRectOnPwmMin(), c.getRectOnPwmMax());
    TEST_ASSERT_TRUE(c.getRectOnPwmMax() < (uint16_t) (c.pwmMaxDriver() - c.getCtrlOnPwmCnt()));
}

// --- DCM sync-rect-off thresholds (SyncRectOffCurrent / SyncRectOffVoltage). ---

void test_buck_sync_rect_off_below_min_current() {
    SynchronousConverter c;
    initConv(c);
    c.pwmPerturb(c.pwmCtrlMax / 2);
    c.updateSyncRectMaxDuty(60.f, 30.f, 0.005f); // il < SyncRectOffCurrent -> disable sync rect
    TEST_ASSERT_TRUE(c.inDCM());
    TEST_ASSERT_EQUAL_UINT(c.getRectOnPwmMin(), c.getRectOnPwmMax());
}

void test_buck_sync_rect_active_in_dcm_with_current() {
    SynchronousConverter c;
    initConv(c);
    c.pwmPerturb(c.pwmCtrlMax / 2);
    c.updateSyncRectMaxDuty(60.f, 30.f, 1.f); // ripple 8.1 > 2*1 -> DCM, current healthy
    TEST_ASSERT_TRUE(c.inDCM());
    TEST_ASSERT_TRUE(c.getRectOnPwmMax() > c.getRectOnPwmMin()); // sync rect conducting
}

// --- boot_refresh_ns -> pwmRectMin count conversion (replaces the old fixed 6%). ---

void test_buck_bootstrap_min_default() {
    SynchronousConverter c;
    initConv(c); // no boot_refresh_ns -> default 500 ns (buck.h)
    auto expect = (uint16_t) std::ceil(500e-9f * kFsw * (float) c.pwmMaxDriver());
    TEST_ASSERT_EQUAL_UINT(expect, c.getRectOnPwmMin());
}

void test_buck_bootstrap_min_scales_with_conf() {
    SynchronousConverter c;
    initConvEx(c, "buck", "3000"); // double the refresh time -> ~double the count
    auto expect = (uint16_t) std::ceil(3000e-9f * kFsw * (float) c.pwmMaxDriver());
    TEST_ASSERT_EQUAL_UINT(expect, c.getRectOnPwmMin());
}

void test_boost_bootstrap_min_is_zero() {
    SynchronousConverter c;
    initConvEx(c, "boost", nullptr); // boost: ctrl=LS pulls node low every cycle, no LS floor
    TEST_ASSERT_TRUE(c.boost());
    TEST_ASSERT_EQUAL_UINT(0, c.getRectOnPwmMin());
}

// boost-side symmetric clamp: M = vout/vin must stay > 1 so rectCtrlRatio() = 1/(M-1) is finite.
void test_boost_ratio_clamped_above_unity() {
    SynchronousConverter c;
    initConvEx(c, "boost", nullptr);
    c.updateSyncRectMaxDuty(30.f, 30.3f, 1.f); // vin=30, vout=30.3, M just above 1
    TEST_ASSERT_TRUE(c.voltageRatio() > 1.f);
    TEST_ASSERT_TRUE(std::isfinite(c.rectCtrlRatio(c.voltageRatio())));
    TEST_ASSERT_TRUE(c.rectCtrlRatio(c.voltageRatio()) >= 0.f);
}
