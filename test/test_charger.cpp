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

    // Voltage just above floor (cv_min - 0.05V = 3.32V): still terminated
    tc.update(p.cv_min - 0.04f, 0.0f, 0.0f);
    TEST_ASSERT_TRUE(bool(tc));

    // Below voltage floor: released even without any Ah counted
    tc.update(p.cv_min - 0.06f, 0.0f, 0.0f);
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

    // Voltage floor still works regardless of Cbat
    tc.update(p.cv_min - 0.06f, 0.0f, 1000.0f);
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

void test_mqtt_ibat_message_drives_coulomb_counter() {
    BatteryCharger charger;
    charger.params = makeLfpParams();

    ConfFile mqttConf{{
        {"ibat_topic", "test/ibat"},
    }};
    charger.beginMqtt(mqttConf);

    // Two discharge samples spaced by a real ~20ms wall-clock gap. Exact accumulation
    // depends on the real clock; we only verify that some discharge accumulated.
    MQTT._invokeForTest("test/ibat", "-5", 2);
    delay(20);
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

