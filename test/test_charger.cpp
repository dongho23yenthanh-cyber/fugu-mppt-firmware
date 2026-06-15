// Tests for src/charger.h (BatChargerParams, BatteryState, Li_ChgTerminationCondition,
// BatteryCharger MQTT callbacks) and src/etc/coulomb_counter.h.
//
// Termination/coulomb tests are pure-function — they construct the units under test
// directly and never touch MQTT.
//
// MQTT-callback tests synthesise messages via MqttService::_invokeForTest, which
// delivers a buffer to the registered handler without going through the broker.
// Tests use distinct topic names per logical channel but rely on subscribeTopic's
// overwrite semantics across tests (each test re-subscribes its own handler).

#include <unity.h>
#include <Arduino.h>

#include <cstring>
#include <unordered_map>

#include "charger.h"
#include "tele/mqtt.h"

// ------------------------------------------------------------------
//  Helpers
// ------------------------------------------------------------------

static BatChargerParams makeLfpParams() {
    BatChargerParams p;
    p.Vbat_max = 14.6f;        // 4S LFP
    p.Vbat_fallback = 13.4f;
    p.cv_eoc = 3.65f;
    p.cv_min = 3.37f;
    p.Cbat = 280.0f;           // 280 Ah pack
    p.tail_c_rate = 0.05f;
    p.Ibat_lim = 40.0f;
    p.recharge_dod = 0.20f;
    return p;
}

// Drive the integrator with a constant current over a duration in 10s steps so we
// stay well under TrapezoidalIntegrator's 30s maxDt guard.
static void driveCounter(CoulombCounter &cc, float ibat,
                         unsigned long startUs, unsigned long endUs,
                         unsigned long stepUs = 10'000'000UL) {
    for (unsigned long t = startUs; t <= endUs; t += stepUs) cc.updateBatCurrent(ibat, t);
}

// ------------------------------------------------------------------
//  Li_ChgTerminationCondition — termination-line math
// ------------------------------------------------------------------

void test_termination_line_at_zero_current() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    tc.update(0.0f, 0.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p.cv_min, tc.v_term());
}

void test_termination_line_at_tail_current() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    const float tailA = p.tail_c_rate * p.Cbat; // 14 A
    tc.update(0.0f, tailA, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p.cv_eoc, tc.v_term());
}

void test_termination_line_clamps_above_cv_eoc() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    const float tailA = p.tail_c_rate * p.Cbat;
    tc.update(0.0f, 2.0f * tailA, 0.0f); // 2× tail — line would be above cv_eoc
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p.cv_eoc, tc.v_term());
}

void test_termination_line_midpoint() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    const float halfTailA = 0.5f * p.tail_c_rate * p.Cbat;
    tc.update(0.0f, halfTailA, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f * (p.cv_min + p.cv_eoc), tc.v_term());
}

// ------------------------------------------------------------------
//  Li_ChgTerminationCondition — latch & release behaviour
// ------------------------------------------------------------------

void test_termination_does_not_latch_below_line() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    tc.update(3.40f, 14.0f, 0.0f); // vcell 3.40 < v_term=3.65
    TEST_ASSERT_FALSE(bool(tc));
}

void test_termination_latches_above_line() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    tc.update(3.70f, 14.0f, 0.0f); // vcell 3.70 > v_term=3.65
    TEST_ASSERT_TRUE(bool(tc));
}

void test_termination_release_via_dod() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    tc.update(3.70f, 14.0f, 0.0f);
    TEST_ASSERT_TRUE(bool(tc));

    // Below the DoD threshold: still terminated
    tc.update(3.40f, 0.0f, 0.9f * p.recharge_dod * p.Cbat); // 50.4 Ah
    TEST_ASSERT_TRUE(bool(tc));

    // Past the DoD threshold: released
    tc.update(3.40f, 0.0f, 1.1f * p.recharge_dod * p.Cbat); // 61.6 Ah
    TEST_ASSERT_FALSE(bool(tc));
}

void test_termination_release_via_voltage_floor() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    tc.update(3.70f, 14.0f, 0.0f);
    TEST_ASSERT_TRUE(bool(tc));

    // Release floor is cv_min - 0.1V, debounced over several BMS frames so a single
    // I·R sag doesn't release. Just above the floor: stays terminated.
    tc.update(p.cv_min - 0.08f, 0.0f, 0.0f);
    TEST_ASSERT_TRUE(bool(tc));

    // A single sub-threshold dip is ignored; going back above the floor resets the streak.
    tc.update(p.cv_min - 0.15f, 0.0f, 0.0f);
    TEST_ASSERT_TRUE(bool(tc));
    tc.update(p.cv_min - 0.08f, 0.0f, 0.0f);
    TEST_ASSERT_TRUE(bool(tc));

    // Sustained below the floor: released even without any Ah counted.
    for (int i = 0; i < 6; ++i) tc.update(p.cv_min - 0.15f, 0.0f, 0.0f);
    TEST_ASSERT_FALSE(bool(tc));
}

void test_termination_dod_release_skipped_when_cbat_missing() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    tc.update(3.70f, 14.0f, 0.0f); // latch with valid params
    TEST_ASSERT_TRUE(bool(tc));

    p.Cbat = NAN; // simulate config drop / unset bat_c

    // Large Ah-since-full but Cbat=NaN → Ah branch is skipped, still terminated
    tc.update(3.40f, 0.0f, 1000.0f);
    TEST_ASSERT_TRUE(bool(tc));

    // Voltage floor still works regardless of Cbat (sustained sub-threshold).
    for (int i = 0; i < 6; ++i) tc.update(p.cv_min - 0.15f, 0.0f, 1000.0f);
    TEST_ASSERT_FALSE(bool(tc));
}

void test_termination_reset_clears_latch() {
    auto p = makeLfpParams();
    Li_ChgTerminationCondition tc{p};
    tc.update(3.70f, 14.0f, 0.0f);
    TEST_ASSERT_TRUE(bool(tc));

    tc.reset();
    TEST_ASSERT_FALSE(bool(tc));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p.cv_min, tc.v_term());
}

// ------------------------------------------------------------------
//  CoulombCounter
// ------------------------------------------------------------------

void test_coulomb_counter_starts_at_zero() {
    CoulombCounter cc;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cc.ahSinceFull());
}

void test_coulomb_counter_discharge_accumulates() {
    CoulombCounter cc;
    // 60 s @ 10 A discharge = 10 * 60 / 3600 ≈ 0.1667 Ah
    driveCounter(cc, -10.0f, 0, 60'000'000UL);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 10.0f / 60.0f, cc.ahSinceFull());
}

void test_coulomb_counter_charging_decrements() {
    CoulombCounter cc;
    driveCounter(cc, -10.0f, 0, 60'000'000UL);
    const float afterDischarge = cc.ahSinceFull();
    TEST_ASSERT_TRUE(afterDischarge > 0.1f);

    driveCounter(cc, +10.0f, 70'000'000UL, 130'000'000UL);
    TEST_ASSERT_TRUE(cc.ahSinceFull() < afterDischarge);
}

void test_coulomb_counter_markfull_resets() {
    CoulombCounter cc;
    driveCounter(cc, -10.0f, 0, 60'000'000UL);
    TEST_ASSERT_TRUE(cc.ahSinceFull() > 0.1f);

    cc.markFull();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cc.ahSinceFull());
}

void test_coulomb_counter_drops_gap_over_maxdt() {
    CoulombCounter cc;
    cc.updateBatCurrent(-10.0f, 0); // seed lastTime/lastX
    cc.updateBatCurrent(-10.0f, 31'000'000UL); // gap > 30s maxDt → drop
    TEST_ASSERT_EQUAL_FLOAT(0.0f, cc.ahSinceFull());

    // Next sample within maxDt resumes integration
    cc.updateBatCurrent(-10.0f, 41'000'000UL); // 10 s after the prior sample
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f * 10.0f / 3600.0f, cc.ahSinceFull());
}

// ------------------------------------------------------------------
//  MQTT-callback integration
// ------------------------------------------------------------------

void test_mqtt_vcell_message_updates_state() {
    BatteryCharger charger;
    charger.params = makeLfpParams();

    ConfFile mqttConf{{
        {"cell_voltages_max_topic", "test/vcell"},
    }};
    charger.beginMqtt(mqttConf);

    const char *msg = "3.512";
    MQTT._invokeForTest("test/vcell", msg, std::strlen(msg));

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.512f, (float) charger.batSt.vcell_high);
    TEST_ASSERT_TRUE(charger.batSt.haveValidCellVoltage());
}

// Test/main.cpp owns this variable in the test build. The MQTT-driven ibat path
// uses wallClockUs() (which reads loopWallClockUs_) for integration timestamps,
// so we must advance it ourselves — nothing else does during tests.
extern unsigned long loopWallClockUs_;

void test_mqtt_ibat_message_drives_coulomb_counter() {
    BatteryCharger charger;
    charger.params = makeLfpParams();

    ConfFile mqttConf{{
        {"ibat_topic", "test/ibat"},
    }};
    charger.beginMqtt(mqttConf);

    // Two discharge samples spaced by a real ~20ms wall-clock gap. We advance
    // loopWallClockUs_ ourselves before each delivery so the integrator sees
    // a non-zero dt.
    loopWallClockUs_ = micros();
    MQTT._invokeForTest("test/ibat", "-5", 2);
    delay(20);
    loopWallClockUs_ = micros();
    MQTT._invokeForTest("test/ibat", "-5", 2);

    TEST_ASSERT_TRUE(charger.batSt.coulombCounter.ahSinceFull() > 0.0f);
}

void test_mqtt_ibat_lim_accepts_valid() {
    BatteryCharger charger;
    charger.params = makeLfpParams();

    ConfFile mqttConf{{
        {"ibat_lim_topic", "test/ibatlim"},
    }};
    charger.beginMqtt(mqttConf);

    MQTT._invokeForTest("test/ibatlim", "30", 2);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, charger.params.Ibat_lim);

    // Zero is valid — BMS can legitimately signal "no charging permitted"
    MQTT._invokeForTest("test/ibatlim", "0", 1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, charger.params.Ibat_lim);
}

void test_mqtt_ibat_lim_rejects_negative() {
    BatteryCharger charger;
    charger.params = makeLfpParams();
    const float initial = charger.params.Ibat_lim;

    ConfFile mqttConf{{
        {"ibat_lim_topic", "test/ibatlim"},
    }};
    charger.beginMqtt(mqttConf);

    MQTT._invokeForTest("test/ibatlim", "-5", 2);
    TEST_ASSERT_EQUAL_FLOAT(initial, charger.params.Ibat_lim);
}

void test_mqtt_ibat_lim_rejects_nan() {
    BatteryCharger charger;
    charger.params = makeLfpParams();
    const float initial = charger.params.Ibat_lim;

    ConfFile mqttConf{{
        {"ibat_lim_topic", "test/ibatlim"},
    }};
    charger.beginMqtt(mqttConf);

    MQTT._invokeForTest("test/ibatlim", "nan", 3);
    TEST_ASSERT_EQUAL_FLOAT(initial, charger.params.Ibat_lim);
}

// A zero-length payload (e.g. a retained-message clear) used to read dat[-1] inside strntof (OOB).
// All three BMS callbacks must now survive it and leave state untouched.
void test_mqtt_empty_payload_is_safe() {
    BatteryCharger charger;
    charger.params = makeLfpParams();
    const float limInit = charger.params.Ibat_lim;

    ConfFile mqttConf{{
        {"cell_voltages_max_topic", "test/vcell"},
        {"ibat_topic", "test/ibat"},
        {"ibat_lim_topic", "test/ibatlim"},
    }};
    charger.beginMqtt(mqttConf);

    MQTT._invokeForTest("test/vcell", "", 0);
    MQTT._invokeForTest("test/ibat", "", 0);
    MQTT._invokeForTest("test/ibatlim", "", 0);

    TEST_ASSERT_FALSE(charger.batSt.haveValidCellVoltage());        // NAN vcell not treated as valid
    TEST_ASSERT_EQUAL_FLOAT(limInit, charger.params.Ibat_lim);      // empty ignored, limit unchanged
    TEST_ASSERT_EQUAL_FLOAT(0.0f, charger.batSt.coulombCounter.ahSinceFull()); // ibat untouched
}

// releaseVoutPinning must release the pack-voltage pin UP to Vbat_max (so a converter that lost
// authority on a shared bus can climb back and re-take it), NOT down to Vbat_fallback — which would
// pin it at the resting bus voltage and throttle a battery that isn't full.
void test_release_vout_pinning_goes_to_vbat_max() {
    BatteryCharger charger;
    charger.params = makeLfpParams();   // Vbat_max=14.6, Vbat_fallback=13.4
    loopWallClockUs_ = 1'000'000;

    // No BMS cell data -> the charger pins Vout down to Vbat_fallback.
    charger.update(13.4f, 0.0f, /*voutAuthority*/ true);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, charger.params.Vbat_fallback, charger.Vout_max());

    // Release must move the target UP to Vbat_max, not leave it at the fallback.
    bool changed = charger.releaseVoutPinning("test");
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, charger.params.Vbat_max, charger.Vout_max());
}

// Drive the EOC feedback loop with the highest cell held above v_eoc, as if this converter's
// Vout reads high so lowering vpack_pin never actually brings the cell down. The BMS-driven loop
// must keep pulling vpack_pin below the nominal float floor (Vbat_fallback) by up to
// params.vout_offset_max, so a full pack stops being trickle-charged despite the Vout offset.
static void driveEocFeedback(BatteryCharger &charger, float vcellHigh, int frames) {
    // termCond.v_term() is cv_min only after _updateTermination runs; reset() seeds it now so v_eoc
    // isn't NaN-> cv_eoc before the ibat smoothing warms up.
    charger.termCond.reset();
    for (int i = 0; i < frames; ++i) {
        loopWallClockUs_ += 1'000'000;        // a fresh BMS frame each iteration (advances vcell_high_t)
        charger.batSt.setVcellHigh(vcellHigh);
        charger.batSt.updateBatCurrent(0.1f); // small +ibat: warms smoothing, latches termination (v_term~cv_min)
        charger.update(charger.params.Vbat_fallback, 0.1f, /*voutAuthority*/ true);
    }
}

void test_eoc_floor_allows_vout_offset_correction() {
    BatteryCharger charger;
    charger.params = makeLfpParams();          // Vbat_fallback=13.4, cv_min=3.37
    charger.params.vout_offset_max = 0.6f;
    loopWallClockUs_ = 1'000'000;

    driveEocFeedback(charger, charger.params.cv_min + 0.10f, 200); // cell stuck above v_eoc

    const float floor = charger.params.Vbat_fallback - charger.params.vout_offset_max; // 12.8
    TEST_ASSERT_TRUE(charger.Vout_max() < charger.params.Vbat_fallback - 0.05f); // used the headroom
    TEST_ASSERT_FLOAT_WITHIN(0.05f, floor, charger.Vout_max());                  // settled at the floor
    TEST_ASSERT_TRUE(charger.Vout_max() >= floor - 0.01f);                       // never below it
}

void test_eoc_floor_zero_offset_stops_at_fallback() {
    BatteryCharger charger;
    charger.params = makeLfpParams();
    charger.params.vout_offset_max = 0.0f;     // legacy behaviour: floor == Vbat_fallback
    loopWallClockUs_ = 1'000'000;

    driveEocFeedback(charger, charger.params.cv_min + 0.10f, 200);

    TEST_ASSERT_FLOAT_WITHIN(0.05f, charger.params.Vbat_fallback, charger.Vout_max());
}

// The float floor tracks vout_offset_max: a 0.3 V budget floors 0.3 V below Vbat_fallback.
void test_eoc_floor_scales_with_offset() {
    BatteryCharger charger;
    charger.params = makeLfpParams();
    charger.params.vout_offset_max = 0.3f;
    loopWallClockUs_ = 1'000'000;

    driveEocFeedback(charger, charger.params.cv_min + 0.10f, 200);

    TEST_ASSERT_FLOAT_WITHIN(0.05f, charger.params.Vbat_fallback - 0.3f, charger.Vout_max());
}

