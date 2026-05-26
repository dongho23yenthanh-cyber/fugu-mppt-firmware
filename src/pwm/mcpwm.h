#pragma once

#include <array>
#include "driver/mcpwm_prelude.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mcpwm_timing.h"

// One GPIO fault per group, shared by all legs. OST brake latches gates LOW in HW.
class MCPWM_FaultBrake {
    mcpwm_fault_handle_t fault_ = nullptr;

public:
    void initGpio(int group, int pin, bool activeHigh) {
        mcpwm_gpio_fault_config_t fc = {
            .group_id = group,
            .intr_priority = 0,
            .gpio_num = pin,
            .flags = {
                .active_level = (uint32_t) (activeHigh ? 1 : 0),
                .io_loop_back = 0,
                .pull_up      = (uint32_t) (activeHigh ? 0 : 1),
                .pull_down    = (uint32_t) (activeHigh ? 1 : 0),
            },
        };
        ESP_ERROR_CHECK(mcpwm_new_gpio_fault(&fc, &fault_));
    }

    // OST brake on the operator + force both gens LOW on the brake event.
    void bindLeg(mcpwm_oper_handle_t op, mcpwm_gen_handle_t hs, mcpwm_gen_handle_t ls) {
        mcpwm_brake_config_t bc = {
            .fault = fault_,
            .brake_mode = MCPWM_OPER_BRAKE_MODE_OST,
            .flags = {.cbc_recover_on_tez = 0, .cbc_recover_on_tep = 0},
        };
        ESP_ERROR_CHECK(mcpwm_operator_set_brake_on_fault(op, &bc));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_brake_event(hs,
            MCPWM_GEN_BRAKE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_OPER_BRAKE_MODE_OST, MCPWM_GEN_ACTION_LOW)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_brake_event(ls,
            MCPWM_GEN_BRAKE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_OPER_BRAKE_MODE_OST, MCPWM_GEN_ACTION_LOW)));
    }

    mcpwm_fault_handle_t handle() const { return fault_; }

    // Clear a latched OST brake on `op`. Recovery is explicit so faults can't silently clear.
    void recover(mcpwm_oper_handle_t op) {
        ESP_ERROR_CHECK(mcpwm_operator_recover_from_fault(op, fault_));
    }
};

// One synchronous leg = one MCPWM operator. See doc/mcpwm-sync-buck-driver.md for the spec.
// HiLi:  HS on [0, hsOff],         LS on [hsOff, lsOff]   (MCPWM dead-time)
// InEn:  IN on [0, hsOff],         EN on [0, lsOff]       (driver chip dead-time, dtTicks=0)
// Comparators latch on TEZ -> glitch-free, order-independent duty updates.
class MCPWM_SyncLeg {
    mcpwm_timer_handle_t timer_ = nullptr;
    mcpwm_oper_handle_t  oper_  = nullptr;
    mcpwm_cmpr_handle_t  cmpHS_ = nullptr, cmpLS_ = nullptr;
    mcpwm_gen_handle_t   genHS_ = nullptr, genLS_ = nullptr;
    uint16_t dtTicks_  = 0;

public:
    const char *name = "mcpwm";
    uint16_t pwmMax    = 0;   // = period_ticks (same role as LEDC pwmMax)

    mcpwm_oper_handle_t  oper()  const { return oper_; }
    mcpwm_gen_handle_t   genHS() const { return genHS_; }
    mcpwm_gen_handle_t   genLS() const { return genLS_; }
    mcpwm_timer_handle_t timer() const { return timer_; }
    [[nodiscard]] uint16_t getDtTicks() const { return dtTicks_; }

    // fixedTicks=0 (default, production) -> bestTiming(freq): max counts the hw can give.
    // fixedTicks>0 overrides bestTiming with that exact period; for migration / bit-identical
    // calibration replays only.
    void init(int group, uint32_t freq, int pinHS, int pinLS,
              uint32_t dtTicks, bool enLogic, uint32_t fixedTicks = 0) {
        ESP_LOGI("mcpwm-leg", "init grp=%d freq=%u pinHS=%d pinLS=%d dtTicks=%u enLogic=%d fixed=%u",
                 group, (unsigned) freq, pinHS, pinLS, (unsigned) dtTicks, enLogic, (unsigned) fixedTicks);
        PwmTiming t = fixedTicks ? PwmTiming{freq * fixedTicks, fixedTicks, freq}
                                 : bestTiming(freq);
        pwmMax = (uint16_t) t.period_ticks;

        mcpwm_timer_config_t tc = {
            .group_id      = group,
            .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
            .resolution_hz = t.resolution_hz,
            .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
            .period_ticks  = t.period_ticks,
            .intr_priority = 0,
            .flags         = {},
        };
        ESP_ERROR_CHECK(mcpwm_new_timer(&tc, &timer_));

        mcpwm_operator_config_t oc = {.group_id = group, .intr_priority = 0, .flags = {}};
        ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &oper_));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper_, timer_));

        mcpwm_comparator_config_t cc = {
            .intr_priority = 0,
            .flags = {.update_cmp_on_tez = 1, .update_cmp_on_tep = 0, .update_cmp_on_sync = 0},
        };
        ESP_ERROR_CHECK(mcpwm_new_comparator(oper_, &cc, &cmpHS_));
        ESP_ERROR_CHECK(mcpwm_new_comparator(oper_, &cc, &cmpLS_));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpHS_, pwmMax / 4));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpLS_, pwmMax / 2));

        mcpwm_generator_config_t gHS = {.gen_gpio_num = pinHS, .flags = {}};
        mcpwm_generator_config_t gLS = {.gen_gpio_num = pinLS, .flags = {}};
        ESP_ERROR_CHECK(mcpwm_new_generator(oper_, &gHS, &genHS_));
        ESP_ERROR_CHECK(mcpwm_new_generator(oper_, &gLS, &genLS_));

        // HS: HIGH at TEZ, LOW at cmpHS  ->  on [0, hsOff]
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(genHS_,
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(genHS_,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpHS_, MCPWM_GEN_ACTION_LOW)));

        // LS: HIGH at (enLogic ? TEZ : cmpHS), LOW at cmpLS  ->  on [hsOff|0, lsOff]
        if (enLogic) {
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(genLS_,
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
        } else {
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(genLS_,
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpHS_, MCPWM_GEN_ACTION_HIGH)));
        }
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(genLS_,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpLS_, MCPWM_GEN_ACTION_LOW)));

        // Dead-time: each MCPWM operator has ONE shared dead-time submodule, so we delay only
        // the LS rising edge by dtTicks (covers the HS->LS transition). The LS->HS transition
        // (start of next period) gets its dead-band by reducing pwmMax by dtTicks so software
        // clamps keep lsOff <= period - dtTicks. HiLi only; 0 = no-op (InEn).
        dtTicks_ = (uint16_t) dtTicks;
        if (dtTicks) {
            mcpwm_dead_time_config_t dt = {
                .posedge_delay_ticks = dtTicks,
                .negedge_delay_ticks = 0,
                .flags = {.invert_output = 0},
            };
            ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(genLS_, genLS_, &dt));
            // KNOWN BUG: this dt config does not produce the expected HiLi pair on S3.
            // Empirically (test_mcpwm_deadband_hs_to_ls / 4b in test/test_pwm.cpp),
            // both HS and LS pins end up showing the same LS-with-rising-delay
            // waveform. Raw-LL register experiments (toggle bits 9, 10, 15, 16, 12
            // of dt_cfg individually) did not produce a stable {HS direct, LS-with-
            // delay} pair. The IDF MCPWM DT module API assumes a HiLi-pair pattern
            // where one output is derived from the other; our diode-emulation use
            // case wants independent cmpLS control. Resolution requires either the
            // ESP32-S3 MCPWM TRM topology diagram (S0–S7 switch network) or a
            // patched IDF driver. See doc/pwm-test-impl-plan.md §M4.
            pwmMax = (uint16_t) (t.period_ticks - dtTicks);   // reserves the LS->HS dead-band
        }
    }

    inline void setHsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpHS_, c); }
    inline void setLsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpLS_, c); }

    void start() {
        ESP_ERROR_CHECK(mcpwm_timer_enable(timer_));
        ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer_, MCPWM_TIMER_START_NO_STOP));
    }

    // Immediate, register-only: force both gates LOW (safe), hold until cleared.
    void forceShutdown() {
        if (genHS_) mcpwm_generator_set_force_level(genHS_, 0, true);
        if (genLS_) mcpwm_generator_set_force_level(genLS_, 0, true);
    }
    // Remove the forced level, returning control to timer/comparator actions.
    void clearForce() {
        if (genHS_) mcpwm_generator_set_force_level(genHS_, -1, true);
        if (genLS_) mcpwm_generator_set_force_level(genLS_, -1, true);
    }
    // Note: clearing a latched OST brake requires the originating fault handle;
    // call MCPWM_FaultBrake::recover(leg.oper()) on the brake that bound this leg.

    // Reverse-order teardown so the MCPWM group resource counts return to zero —
    // tests need to re-init multiple times. Production builds with a single global
    // leg never run this.
    ~MCPWM_SyncLeg() {
        if (timer_) mcpwm_timer_start_stop(timer_, MCPWM_TIMER_STOP_EMPTY);
        if (timer_) mcpwm_timer_disable(timer_);
        if (genHS_) mcpwm_del_generator(genHS_);
        if (genLS_) mcpwm_del_generator(genLS_);
        if (cmpHS_) mcpwm_del_comparator(cmpHS_);
        if (cmpLS_) mcpwm_del_comparator(cmpLS_);
        if (oper_)  mcpwm_del_operator(oper_);
        if (timer_) mcpwm_del_timer(timer_);
    }
    MCPWM_SyncLeg() = default;
    MCPWM_SyncLeg(const MCPWM_SyncLeg&) = delete;
    MCPWM_SyncLeg& operator=(const MCPWM_SyncLeg&) = delete;
};

// N interleaved synchronous legs, timers phase-shifted by period/N, one shared fault.
template <int N>
class MCPWM_Converter {
    std::array<MCPWM_SyncLeg, N> legs_;
    MCPWM_FaultBrake fault_;
    mcpwm_sync_handle_t sync_ = nullptr;

public:
    uint16_t pwmMax = 0;

    void init(int group, uint32_t freq, const int (&pinHS)[N], const int (&pinLS)[N],
              uint32_t dtTicks, bool enLogic, uint32_t fixedTicks,
              int faultPin = -1, bool faultActiveHigh = false) {
        for (int i = 0; i < N; ++i)
            legs_[i].init(group, freq, pinHS[i], pinLS[i], dtTicks, enLogic, fixedTicks);
        pwmMax = legs_[0].pwmMax;

        if (faultPin >= 0) {
            fault_.initGpio(group, faultPin, faultActiveHigh);
            for (auto &l : legs_) fault_.bindLeg(l.oper(), l.genHS(), l.genLS());
        }

        if (N > 1) {
            mcpwm_timer_sync_src_config_t sc = {
                .timer_event = MCPWM_TIMER_EVENT_EMPTY,
                .flags = {.propagate_input_sync = 0},
            };
            ESP_ERROR_CHECK(mcpwm_new_timer_sync_src(legs_[0].timer(), &sc, &sync_));
            for (int i = 1; i < N; ++i) {
                mcpwm_timer_sync_phase_config_t pc = {
                    .sync_src    = sync_,
                    .count_value = (uint32_t) ((uint32_t) pwmMax * i / N),
                    .direction   = MCPWM_TIMER_DIRECTION_UP,
                };
                ESP_ERROR_CHECK(mcpwm_timer_set_phase_on_sync(legs_[i].timer(), &pc));
            }
        }
        for (auto &l : legs_) l.start();
    }

    void setHsOff(uint16_t c) { for (auto &l : legs_) l.setHsOff(c); }
    void setLsOff(uint16_t c) { for (auto &l : legs_) l.setLsOff(c); }
    void forceShutdown()      { for (auto &l : legs_) l.forceShutdown(); }
    void clearForce()         { for (auto &l : legs_) l.clearForce(); }
};
