#include <unity.h>
//#include <task.h>
#include <Arduino.h>

#include "storage/key-value.h"

//#warning "Test main"

// src/main.cpp defines this in the normal build; with RUN_TESTS=1 the entry point
// is swapped to this file, so we provide our own definition. wallClockUs() reads it,
// but nothing in tests writes — that's fine, time-based tests pass explicit timestamps.
unsigned long loopWallClockUs_ = 0;

// Same swap: src/main.cpp owns the real nvs; telemetry.cpp (linked into the test
// build) references it, so define it here too.
KeyValueStorage nvs{};

// telemetry.cpp references the global `mppt`; mqtt.cpp references handleCommand() (cmd_input).
// Both live in src/main.cpp / src/cli.cpp in the normal build, which RUN_TESTS excludes — so
// stub them here to link. The ctor args use internal linkage to avoid clashing with any test
// TU's own globals; construction mirrors the real static-init (null sensors, empty LCD).
#include "adc/sampling.h"
#include "buck.h"
#include "viz/lcd.h"
#include "mppt.h"
static ADC_Sampler s_adcSampler{};
static VIinVout<const Sensor *> s_sensors{nullptr, nullptr, nullptr, nullptr};
static SynchronousConverter s_converter{};
static LCD s_lcd{};
MpptController mppt{s_adcSampler, s_sensors, s_converter, s_lcd};

// RT/ADC path (temperature.h, mppt) reaches the scope streamer through this pointer; null = off.
Scope *scope = nullptr;

bool handleCommand(const String &) { return false; }

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

// asciichart/ascii.h
void test_ascii_empty_single_series_emits_nothing();
void test_ascii_empty_multi_series_emits_nothing();
void test_ascii_multi_with_only_empty_inner_emits_nothing();
void test_ascii_all_nan_series_emits_nothing();
void test_ascii_leading_nan_does_not_pollute_min();
void test_ascii_trailing_nan_does_not_pollute_max();
void test_ascii_embedded_nan_gap_does_not_crash();
void test_ascii_single_finite_among_nan_renders();
void test_ascii_first_sample_nan_does_not_misplace_marker();
void test_ascii_constant_series_does_not_divide_by_zero();
void test_ascii_two_point_series();
void test_ascii_custom_min_max_used();
void test_ascii_zero_crossing_draws_center_glyph();
void test_ascii_all_positive_uses_axis_glyph();
void test_ascii_zero_offset_clamps_safely();
void test_ascii_negative_offset_clamps_safely();
void test_ascii_every_emitted_line_ends_with_reset();
void test_ascii_multi_series_uses_distinct_colors();
void test_ascii_multi_series_unequal_length_does_not_crash();
void test_ascii_multi_series_with_one_empty_renders_other();
void test_ascii_regression_plot_h_first_bin_nan();

// math/statmath.h
void test_ewma_seeds_on_first_finite();
void test_ewma_ignores_nan();
void test_ewma_converges_to_constant();
void test_ewma_reset_and_span();
void test_ewma_npass_converges_to_constant();
void test_median3_first_sample_passthrough();
void test_median3_rejects_spike();
void test_median3_picks_middle();
void test_median5_picks_middle();
void test_median5_rejects_spike();
void test_mean_accumulator_pop_resets();
void test_ewm_mean_tracks_and_var_zero_for_constant();
void test_integrator_constant_rate();
void test_integrator_drops_gap_over_maxdt();
void test_integrator_reset_and_restore();

// pd_control.h
void test_pd_proportional_only();
void test_pd_first_tick_has_zero_derivative();
void test_pd_derivative_on_step();
void test_pd_normalize_relative_error();
void test_pd_reset_clears_derivative();
void test_pd_smooth_setpoint_lags_step();

// buck.h
void test_buck_ripple_current();
void test_buck_dcm_ccm_transition_and_hysteresis();
void test_buck_rect_ctrl_ratio();
void test_buck_current_sweep_no_crash();

// conf.h
void test_conf_getlong_bases();
void test_conf_getlong_default_when_missing();
void test_conf_getfloat();
void test_conf_getstring();
void test_conf_required_missing_long_throws();
void test_conf_required_missing_string_throws();
void test_conf_operator_bool();
void test_conf_remove_drops_line_and_keeps_rest();
void test_conf_remove_strips_inline_comment_too();
void test_conf_remove_missing_key_returns_false();

void setup() {
    UNITY_BEGIN();

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

    // asciichart/ascii.h — NaN handling regression + edge cases
    RUN_TEST(test_ascii_empty_single_series_emits_nothing);
    RUN_TEST(test_ascii_empty_multi_series_emits_nothing);
    RUN_TEST(test_ascii_multi_with_only_empty_inner_emits_nothing);
    RUN_TEST(test_ascii_all_nan_series_emits_nothing);
    RUN_TEST(test_ascii_leading_nan_does_not_pollute_min);
    RUN_TEST(test_ascii_trailing_nan_does_not_pollute_max);
    RUN_TEST(test_ascii_embedded_nan_gap_does_not_crash);
    RUN_TEST(test_ascii_single_finite_among_nan_renders);
    RUN_TEST(test_ascii_first_sample_nan_does_not_misplace_marker);
    RUN_TEST(test_ascii_constant_series_does_not_divide_by_zero);
    RUN_TEST(test_ascii_two_point_series);
    RUN_TEST(test_ascii_custom_min_max_used);
    RUN_TEST(test_ascii_zero_crossing_draws_center_glyph);
    RUN_TEST(test_ascii_all_positive_uses_axis_glyph);
    RUN_TEST(test_ascii_zero_offset_clamps_safely);
    RUN_TEST(test_ascii_negative_offset_clamps_safely);
    RUN_TEST(test_ascii_every_emitted_line_ends_with_reset);
    RUN_TEST(test_ascii_multi_series_uses_distinct_colors);
    RUN_TEST(test_ascii_multi_series_unequal_length_does_not_crash);
    RUN_TEST(test_ascii_multi_series_with_one_empty_renders_other);
    RUN_TEST(test_ascii_regression_plot_h_first_bin_nan);

    // math/statmath.h — EWMA, median, mean, EWM, integrator
    RUN_TEST(test_ewma_seeds_on_first_finite);
    RUN_TEST(test_ewma_ignores_nan);
    RUN_TEST(test_ewma_converges_to_constant);
    RUN_TEST(test_ewma_reset_and_span);
    RUN_TEST(test_ewma_npass_converges_to_constant);
    RUN_TEST(test_median3_first_sample_passthrough);
    RUN_TEST(test_median3_rejects_spike);
    RUN_TEST(test_median3_picks_middle);
    RUN_TEST(test_median5_picks_middle);
    RUN_TEST(test_median5_rejects_spike);
    RUN_TEST(test_mean_accumulator_pop_resets);
    RUN_TEST(test_ewm_mean_tracks_and_var_zero_for_constant);
    RUN_TEST(test_integrator_constant_rate);
    RUN_TEST(test_integrator_drops_gap_over_maxdt);
    RUN_TEST(test_integrator_reset_and_restore);

    // pd_control.h
    RUN_TEST(test_pd_proportional_only);
    RUN_TEST(test_pd_first_tick_has_zero_derivative);
    RUN_TEST(test_pd_derivative_on_step);
    RUN_TEST(test_pd_normalize_relative_error);
    RUN_TEST(test_pd_reset_clears_derivative);
    RUN_TEST(test_pd_smooth_setpoint_lags_step);

    // buck.h — diode-emulation / sync-rect math
    RUN_TEST(test_buck_ripple_current);
    RUN_TEST(test_buck_dcm_ccm_transition_and_hysteresis);
    RUN_TEST(test_buck_rect_ctrl_ratio);
    RUN_TEST(test_buck_current_sweep_no_crash);

    // conf.h — ConfFile getters
    RUN_TEST(test_conf_getlong_bases);
    RUN_TEST(test_conf_getlong_default_when_missing);
    RUN_TEST(test_conf_getfloat);
    RUN_TEST(test_conf_getstring);
    RUN_TEST(test_conf_required_missing_long_throws);
    RUN_TEST(test_conf_required_missing_string_throws);
    RUN_TEST(test_conf_operator_bool);
    RUN_TEST(test_conf_remove_drops_line_and_keeps_rest);
    RUN_TEST(test_conf_remove_strips_inline_comment_too);
    RUN_TEST(test_conf_remove_missing_key_returns_false);

    RUN_TEST(test_float16);

    RUN_TEST(test_meter);
    RUN_TEST(test_meter_storage);

    RUN_TEST(test_LinearTransform);
    // RUN_TEST(test_ADCSampler); // body is #if 0'd in test_sampler.cpp

    UNITY_END();
}

void loop() {
    //vTaskDelay(5);
    delay(10);
    //delay(1);
}