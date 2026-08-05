#pragma once

#include <array>
#include "driver/mcpwm_prelude.h"
#if WITH_WSYNC
#include "driver/gpio_filter.h"
#endif
#include "hal/mcpwm_ll.h"
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
#if WITH_WSYNC
    // wired inter-chip sync (see doc/dev-notes/wired-sync.md): leader pulse out / follower phase-reload in
    mcpwm_oper_handle_t  syncOper_ = nullptr;
    mcpwm_cmpr_handle_t  syncCmp_  = nullptr, syncCmpA_ = nullptr;
    mcpwm_gen_handle_t   syncGen_  = nullptr;
    mcpwm_sync_handle_t  syncIn_   = nullptr;
    gpio_glitch_filter_handle_t syncFilt_ = nullptr;
#endif
    uint16_t dtTicks_  = 0;
    int group_ = 0;

public:
    const char *name = "mcpwm";
    uint16_t pwmMax     = 0;   // commandable span: period_ticks (InEn) or period_ticks-dtTicks (HiLi)
    uint16_t periodTicks = 0;  // true timer period; phase math must use this, not pwmMax
    static constexpr uint16_t wsyncLeadTicks = 2;  // follower period shortening, see init()
    uint32_t resolutionHz = 0; // timer tick rate (counts/sec)

    mcpwm_oper_handle_t  oper()  const { return oper_; }
    mcpwm_gen_handle_t   genHS() const { return genHS_; }
    mcpwm_gen_handle_t   genLS() const { return genLS_; }
    mcpwm_timer_handle_t timer() const { return timer_; }
    [[nodiscard]] uint16_t getDtTicks() const { return dtTicks_; }

    // fixedTicks=0 (default, production) -> bestTiming(freq): max counts the hw can give.
    // fixedTicks>0 overrides bestTiming with that exact period; for migration / bit-identical
    // calibration replays only.
    // syncFollower: this leg is a wired-sync follower (initSyncIn follows) — the incoming sync
    // event substitutes TEZ, so comparator + period shadow registers must also latch on sync (a
    // slow follower crystal would otherwise never reach its own TEZ once the reloads keep it
    // below period).
    void init(int group, uint32_t freq, int pinHS, int pinLS,
              uint32_t dtTicks, bool enLogic, uint32_t fixedTicks = 0, bool syncFollower = false) {
        ESP_LOGI("mcpwm-leg", "init grp=%d freq=%u pinHS=%d pinLS=%d dtTicks=%u enLogic=%d fixed=%u",
                 group, (unsigned) freq, pinHS, pinLS, (unsigned) dtTicks, enLogic, (unsigned) fixedTicks);
        PwmTiming t = fixedTicks ? PwmTiming{freq * fixedTicks, fixedTicks, freq}
                                 : bestTiming(freq);
        // A follower runs deliberately FAST (period short by wsyncLeadTicks) so that its own TEZ
        // always fires before the leader's sync edge arrives. The sync then only truncates an
        // already-wrapped period instead of pre-empting it, which matters twice over: the gates
        // get their normal TEZ actions with the software-reserved LS->HS dead-band, and the sync
        // never has to drive HS itself (a sync-driven HS turn-on has no dead-time against the LS
        // it switches off in the same clock). 2 ticks = ~490 ppm at 39 kHz, far above the ~40 ppm
        // worst-case crystal mismatch plus one tick of resync quantization.
        if (syncFollower) {
            assert_throw(t.period_ticks > wsyncLeadTicks + 1, "pwm_freq too high for wired sync");
            t.period_ticks -= wsyncLeadTicks;
        }
        pwmMax      = (uint16_t) t.period_ticks;
        periodTicks = (uint16_t) t.period_ticks;
        resolutionHz = t.resolution_hz;
        group_      = group;

        mcpwm_timer_config_t tc = {
            .group_id      = group,
            .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
            .resolution_hz = t.resolution_hz,
            .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
            .period_ticks  = t.period_ticks,
            .intr_priority = 0,
            // period writes (setPeriodTicks) latch on TEZ -> glitch-free, like the comparators
            .flags         = {.update_period_on_empty = 1, .update_period_on_sync = (uint32_t) syncFollower, .allow_pd = 0},
        };
        ESP_ERROR_CHECK(mcpwm_new_timer(&tc, &timer_));

        mcpwm_operator_config_t oc = {.group_id = group, .intr_priority = 0, .flags = {}};
        ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &oper_));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper_, timer_));

        mcpwm_comparator_config_t cc = {
            .intr_priority = 0,
            .flags = {.update_cmp_on_tez = 1, .update_cmp_on_tep = 0, .update_cmp_on_sync = (uint32_t) syncFollower},
        };
        ESP_ERROR_CHECK(mcpwm_new_comparator(oper_, &cc, &cmpHS_));
        ESP_ERROR_CHECK(mcpwm_new_comparator(oper_, &cc, &cmpLS_));
        // Boot at duty 0: cmp=0 makes HS/LS/EN all stay low out of reset, so gates
        // can't toggle before the first protected pwmPerturb. (The converter also
        // forceShutdown()s right after start; this closes the start()->force window.)
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpHS_, 0));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpLS_, 0));

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
            // The IDF mcpwm dt submodule binds RED to path 0 (= output A's natural
            // routing). To put RED on gen B (LS-rising), we must swap gen B onto
            // path 0 *and* swap gen A off path 0 onto path 1. swap_out_path is
            // only touched by non-bypass calls, so we configure FED with 1 tick
            // (~6.25 ns) on gen A to claim path 1 and trigger the swap. The 1-tick
            // falling-edge delay on HS is negligible (< 0.03 % duty at 39 kHz).
            mcpwm_dead_time_config_t a_fed = {
                .posedge_delay_ticks = 0,
                .negedge_delay_ticks = 1,
                .flags = {.invert_output = 0},
            };
            ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(genHS_, genHS_, &a_fed));
            mcpwm_dead_time_config_t b_red = {
                .posedge_delay_ticks = dtTicks,
                .negedge_delay_ticks = 0,
                .flags = {.invert_output = 0},
            };
            ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(genLS_, genLS_, &b_red));
            pwmMax = (uint16_t) (t.period_ticks - dtTicks);   // reserves the LS->HS dead-band
        }
    }

#if WITH_WSYNC
    // Leader: emit a pulse [offsetTicks, offsetTicks+pulseTicks] each period on `gpio`,
    // phase-rigid to the switching period (own operator, gate dead-time submodule untouched).
    // offsetTicks shifts the follower's period start relative to the leader's for interleaving.
    // Call before start().
    void initSyncOut(int gpio, uint16_t pulseTicks, uint16_t offsetTicks) {
        // the pulse must fit the period. Guard pulseTicks first: periodTicks - pulseTicks - 1
        // underflows to a huge uint16_t if the pulse alone is longer than the period.
        assert_throw(periodTicks > 1, "initSyncOut before init()");
        if (pulseTicks >= periodTicks) pulseTicks = (uint16_t) (periodTicks - 1);
        if ((uint32_t) offsetTicks + pulseTicks >= periodTicks) {
            offsetTicks = (uint16_t) (periodTicks - pulseTicks - 1);
            ESP_LOGW("mcpwm-leg", "sync pulse offset clamped to %u ticks", offsetTicks);
        }
        mcpwm_operator_config_t oc = {.group_id = group_, .intr_priority = 0, .flags = {}};
        ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &syncOper_));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(syncOper_, timer_));
        mcpwm_comparator_config_t cc = {
            .intr_priority = 0,
            .flags = {.update_cmp_on_tez = 1, .update_cmp_on_tep = 0, .update_cmp_on_sync = 0},
        };
        ESP_ERROR_CHECK(mcpwm_new_comparator(syncOper_, &cc, &syncCmp_));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(syncCmp_, (uint32_t) offsetTicks + pulseTicks));
        mcpwm_generator_config_t gc = {.gen_gpio_num = gpio, .flags = {}};
        ESP_ERROR_CHECK(mcpwm_new_generator(syncOper_, &gc, &syncGen_));
        if (offsetTicks) {
            ESP_ERROR_CHECK(mcpwm_new_comparator(syncOper_, &cc, &syncCmpA_));
            ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(syncCmpA_, offsetTicks));
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(syncGen_,
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, syncCmpA_, MCPWM_GEN_ACTION_HIGH)));
        } else {
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(syncGen_,
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
        }
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(syncGen_,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, syncCmp_, MCPWM_GEN_ACTION_LOW)));
    }

    // Follower: each rising edge on `gpio` IS the period boundary — the timer count hardware-reloads
    // to 0 and the generators run their period-start actions, making the sync a full TEZ
    // substitute: an arbitrary phase jump lands in a defined period-start gate state instead of
    // skipping a comparator event (stretched gate pulse), and a slow follower crystal that never
    // reaches its own TEZ (reloaded below period every cycle) still gets shadow latching + gate
    // actions. The reload target is fixed at 0 — any value past a live comparator would re-open
    // the skip hazard; shift the MASTER's pulse (initSyncOut offsetTicks) to phase-shift instead.
    // IDF allows one sync-trigger action per operator, so LS takes the public API (it guards
    // against HS/LS overlap after a mid-cycle jump) and HS's action on the same trigger is a
    // direct LL write: production has a single global leg, so this leg holds operator 0,
    // genHS_ = generator 0 (first alloc) and trigger slot 0 (first free; the brake does not use
    // the trigger table) — same hw-index assumption as count().
    // Requires init(syncFollower=true); call before start().
    void initSyncIn(int gpio, bool enLogic) {
        mcpwm_gpio_sync_src_config_t sc = {
            .group_id = group_,
            .gpio_num = gpio,
            .flags = {.active_neg = 0, .io_loop_back = 0, .pull_up = 0, .pull_down = 1},
        };
        ESP_ERROR_CHECK(mcpwm_new_gpio_sync_src(&sc, &syncIn_));
        // kill any legacy pad pull-up (e.g. U0RXD default): pull-up vs the sync-src pull-down
        // parks the pin at mid-rail and threshold chatter becomes a sync-reload storm
        ESP_ERROR_CHECK(gpio_set_pull_mode((gpio_num_t) gpio, GPIO_PULLDOWN_ONLY));
        mcpwm_timer_sync_phase_config_t pc = {
            .sync_src    = syncIn_,
            .count_value = 0,
            .direction   = MCPWM_TIMER_DIRECTION_UP,
        };
        ESP_ERROR_CHECK(mcpwm_timer_set_phase_on_sync(timer_, &pc));
        // LS to its period-start state on sync. There is deliberately NO HS action here: driving
        // HS high on the sync edge would switch it on in the same clock that switches LS off,
        // with zero dead time (the LS->HS band is reserved in software as pwmMax -= dtTicks,
        // which only holds at a real period boundary). HS turn-on stays with TEZ, which the
        // follower always reaches first thanks to wsyncLeadTicks.
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_sync_event(genLS_,
            MCPWM_GEN_SYNC_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, syncIn_,
                                        enLogic ? MCPWM_GEN_ACTION_HIGH : MCPWM_GEN_ACTION_LOW)));
    }

    // Glitch filter on the sync pad. The receiver has no input hysteresis and a noise-induced
    // edge is not cosmetic: it re-phases the timer mid-cycle. The S3 has only the fixed-width
    // PIN filter (~2 IO-MUX clocks, ~25 ns at 80 MHz) -- not the flex filter, so a 200 ns window
    // is not reachable in hardware here. That rejects fast spikes only; slow ringing on the
    // high-impedance bias node still needs the external Schmitt buffer. The filter's own
    // propagation delay is fixed and lands in what sync_phase_ns exists to trim out.
    void initSyncFilter(int gpio) {
        gpio_pin_glitch_filter_config_t fc = {
            .clk_src  = GLITCH_FILTER_CLK_SRC_DEFAULT,
            .gpio_num = (gpio_num_t) gpio,
        };
        ESP_ERROR_CHECK(gpio_new_pin_glitch_filter(&fc, &syncFilt_));
        ESP_ERROR_CHECK(gpio_glitch_filter_enable(syncFilt_));
    }
#endif // WITH_WSYNC

    inline void setHsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpHS_, c); }
    inline void setLsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpLS_, c); }

    // Trim the timer period (latches on TEZ, see update_period_on_empty). Callers must never pass
    // a value below the init periodTicks: comparators may sit at periodTicks-1 and a shrunken
    // period would skip their event, leaving the LS gate high for a full cycle.
    inline void setPeriodTicks(uint16_t t) { mcpwm_timer_set_period(timer_, t); }

    // Live counter value, register read. The driver never exposes the count; production has a
    // single global leg, so this leg holds hw timer 0 of its group (first mcpwm_new_timer alloc).
    [[nodiscard]] inline uint32_t count() const {
        return mcpwm_ll_timer_get_count_value(MCPWM_LL_GET_HW(group_), 0);
    }

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
#if WITH_WSYNC
        if (syncGen_)  mcpwm_del_generator(syncGen_);
        if (syncCmp_)  mcpwm_del_comparator(syncCmp_);
        if (syncCmpA_) mcpwm_del_comparator(syncCmpA_);
        if (syncOper_) mcpwm_del_operator(syncOper_);
        if (syncIn_)   mcpwm_del_sync_src(syncIn_);
        if (syncFilt_) { gpio_glitch_filter_disable(syncFilt_); gpio_del_glitch_filter(syncFilt_); }
#endif
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
                    // phase is a fraction of the true timer period, not the dt-reduced pwmMax
                    .count_value = (uint32_t) ((uint32_t) legs_[0].periodTicks * i / N),
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
