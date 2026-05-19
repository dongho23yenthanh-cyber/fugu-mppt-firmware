#include <unity.h>
//#include <task.h>
#include <Arduino.h>

//#warning "Test main"

// src/main.cpp defines this in the normal build; with RUN_TESTS=1 the entry point
// is swapped to this file, so we provide our own definition. wallClockUs() reads it,
// but nothing in tests writes — that's fine, time-based tests pass explicit timestamps.
unsigned long loopWallClockUs_ = 0;

void test_meter();

void test_meter_storage();

void test_LinearTransform();

// test_ADCSampler() body is currently #if 0'd in test/test_sampler.cpp — skip
// the RUN_TEST below until that is re-enabled.
// void test_ADCSampler();

void test_float16();

// charger.h
void test_termination_line_at_zero_current();
void test_termination_line_at_tail_current();
void test_termination_line_clamps_above_cv_eoc();
void test_termination_line_midpoint();
void test_termination_does_not_latch_below_line();
void test_termination_latches_above_line();
void test_termination_release_via_dod();
void test_termination_release_via_voltage_floor();
void test_termination_dod_release_skipped_when_cbat_missing();
void test_termination_reset_clears_latch();
void test_coulomb_counter_starts_at_zero();
void test_coulomb_counter_discharge_accumulates();
void test_coulomb_counter_charging_decrements();
void test_coulomb_counter_markfull_resets();
void test_coulomb_counter_drops_gap_over_maxdt();
void test_mqtt_vcell_message_updates_state();
void test_mqtt_ibat_message_drives_coulomb_counter();
void test_mqtt_ibat_lim_accepts_valid();
void test_mqtt_ibat_lim_rejects_negative();
void test_mqtt_ibat_lim_rejects_nan();

void setup() {
    UNITY_BEGIN();

    RUN_TEST(test_float16);

    RUN_TEST(test_meter);
    RUN_TEST(test_meter_storage);

    RUN_TEST(test_LinearTransform);
    // RUN_TEST(test_ADCSampler); // body is #if 0'd in test_sampler.cpp

    // charger.h — termination math
    RUN_TEST(test_termination_line_at_zero_current);
    RUN_TEST(test_termination_line_at_tail_current);
    RUN_TEST(test_termination_line_clamps_above_cv_eoc);
    RUN_TEST(test_termination_line_midpoint);
    // charger.h — latch/release
    RUN_TEST(test_termination_does_not_latch_below_line);
    RUN_TEST(test_termination_latches_above_line);
    RUN_TEST(test_termination_release_via_dod);
    RUN_TEST(test_termination_release_via_voltage_floor);
    RUN_TEST(test_termination_dod_release_skipped_when_cbat_missing);
    RUN_TEST(test_termination_reset_clears_latch);
    // etc/coulomb_counter.h
    RUN_TEST(test_coulomb_counter_starts_at_zero);
    RUN_TEST(test_coulomb_counter_discharge_accumulates);
    RUN_TEST(test_coulomb_counter_charging_decrements);
    RUN_TEST(test_coulomb_counter_markfull_resets);
    RUN_TEST(test_coulomb_counter_drops_gap_over_maxdt);
    // charger.h — MQTT callback integration
    RUN_TEST(test_mqtt_vcell_message_updates_state);
    RUN_TEST(test_mqtt_ibat_message_drives_coulomb_counter);
    RUN_TEST(test_mqtt_ibat_lim_accepts_valid);
    RUN_TEST(test_mqtt_ibat_lim_rejects_negative);
    RUN_TEST(test_mqtt_ibat_lim_rejects_nan);

    /**
     * TODO
     *  math/statmath stuff (EWM, med3)
     *  float16
     */

    UNITY_END();
}

void loop() {
    //vTaskDelay(5);
    delay(10);
    //delay(1);
}