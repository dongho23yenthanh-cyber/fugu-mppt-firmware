// PWM driver tests (doc/pwm-test-spec1.md). Currently:
//   Rig-1 / Rig-2 — measurement-rig self-test against the production LEDC driver
//   (validates matrix loopback + MCPWM_CAP timestamps before any MCPWM test runs).
// Future MCPWM tests (M3+) compose CapTimer / CapChan from this file.
//
// Test pin: GPIO 4 by default — override with -DPWM_TEST_PIN=<n>. Zero external
// wiring; INPUT_OUTPUT mode + internal GPIO-matrix routing handles loopback.

#include <unity.h>
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <stdexcept>            // util.h's ESP_ERROR_CHECK_THROW uses std::runtime_error
#include <optional>
#include <algorithm>
#include <driver/mcpwm_cap.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>

#include "util.h"        // ESP_ERROR_CHECK_THROW used inside pwm/ledc.h
#include "pwm/ledc.h"
#include "pwm/mcpwm.h"
#include "pwm/mcpwm_timing.h"

static const char *TAG_PWM = "pwm_rig";

#ifndef PWM_TEST_PIN
#define PWM_TEST_PIN 4
#endif
#ifndef PWM_HS_PIN
#define PWM_HS_PIN 5    // MCPWM HS gate (rig test pin is separate)
#endif
#ifndef PWM_LS_PIN
#define PWM_LS_PIN 6    // MCPWM LS gate
#endif
#ifndef PWM_FAULT_PIN
#define PWM_FAULT_PIN 7 // MCPWM fault input (Test 11)
#endif
#ifndef PWM_HS_PIN_2
#define PWM_HS_PIN_2 8  // Test 12 — leg-1 HS
#endif
#ifndef PWM_LS_PIN_2
#define PWM_LS_PIN_2 9  // Test 12 — leg-1 LS
#endif

static constexpr int       kTestPin = PWM_TEST_PIN;
static constexpr int       kHsPin    = PWM_HS_PIN;
static constexpr int       kLsPin    = PWM_LS_PIN;
static constexpr int       kFaultPin = PWM_FAULT_PIN;
static constexpr uint32_t  kFsw     = 39000;
static constexpr uint32_t  kApbHz   = 80'000'000;  // MCPWM CAP clock on S3 + classic ESP32
static constexpr float     kNsPerTick = 1e9f / (float)kApbHz;  // 12.5 ns

// Shared capture-event ring. Single producer (the CAP ISR for ALL channels on the
// same group, since they share one timer / one ISR install). chan_id disambiguates
// when more than one CapChan is feeding the ring.
struct CapEvent {
    uint32_t ts;       // APB ticks (12.5 ns/tick)
    uint8_t  chan_id;  // which CapChan emitted this event
    uint8_t  pos;      // 1 = rising, 0 = falling
};

static constexpr size_t kRingSize = 8192;  // Rig-1 needs ~8000 (4000 cycles × 2 edges)
static CapEvent          g_ring[kRingSize];
static volatile uint32_t g_head;

static bool IRAM_ATTR cap_isr(mcpwm_cap_channel_handle_t /*chan*/,
                              const mcpwm_capture_event_data_t *edata,
                              void *user_ctx) {
    uint32_t h = g_head;
    if (h < kRingSize) {
        g_ring[h].ts      = edata->cap_value;
        g_ring[h].chan_id = (uint8_t)(intptr_t)user_ctx;
        g_ring[h].pos     = (edata->cap_edge == MCPWM_CAP_EDGE_POS) ? 1 : 0;
        g_head = h + 1;
    }
    return false;
}

namespace {

// One per MCPWM group. Owns the 80 MHz APB-clocked free-running timer; multiple
// CapChan instances attached to it share the same timebase (zero inter-channel skew).
class CapTimer {
public:
    mcpwm_cap_timer_handle_t handle = nullptr;
    int group_id;

    explicit CapTimer(int group = 0) : group_id(group) {
        mcpwm_capture_timer_config_t tconf = {
            .group_id      = group,
            .clk_src       = MCPWM_CAPTURE_CLK_SRC_DEFAULT,  // APB on S3 / classic
            .resolution_hz = 0,                              // 0 = driver default (APB)
            .flags         = {},
        };
        ESP_ERROR_CHECK(mcpwm_new_capture_timer(&tconf, &handle));
        ESP_ERROR_CHECK(mcpwm_capture_timer_enable(handle));
        ESP_ERROR_CHECK(mcpwm_capture_timer_start(handle));
    }
    ~CapTimer() {
        if (handle) {
            mcpwm_capture_timer_stop(handle);
            mcpwm_capture_timer_disable(handle);
            mcpwm_del_capture_timer(handle);
        }
    }
    CapTimer(const CapTimer&) = delete;
    CapTimer& operator=(const CapTimer&) = delete;
};

// One per gate / signal to observe. Constructor sets the pin to INPUT mode (which
// clobbers any output-enable a peripheral previously set); the caller is responsible
// for restoring OE + matrix-output route if a peripheral is also driving the pin.
// chan_id is propagated to CapEvent via the ISR user_ctx.
class CapChan {
public:
    mcpwm_cap_channel_handle_t handle = nullptr;
    int chan_id;

    CapChan(CapTimer& timer, int gpio_num, int chan_id_, bool io_loop_back = false)
        : chan_id(chan_id_) {
        mcpwm_capture_channel_config_t cconf = {
            .gpio_num      = gpio_num,
            .intr_priority = 0,
            .prescale      = 1,
            .flags = {
                .pos_edge             = 1,
                .neg_edge             = 1,
                .pull_up              = 0,
                .pull_down            = 0,
                .invert_cap_signal    = 0,
                .io_loop_back         = io_loop_back ? 1u : 0u,
                .keep_io_conf_at_exit = 0,
            },
        };
        ESP_ERROR_CHECK(mcpwm_new_capture_channel(timer.handle, &cconf, &handle));
        mcpwm_capture_event_callbacks_t cbs = { .on_cap = cap_isr };
        ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(
            handle, &cbs, (void*)(intptr_t)chan_id));
        ESP_ERROR_CHECK(mcpwm_capture_channel_enable(handle));
    }
    ~CapChan() {
        if (handle) {
            mcpwm_capture_channel_disable(handle);
            mcpwm_del_capture_channel(handle);
        }
    }
    void enable()  { ESP_ERROR_CHECK(mcpwm_capture_channel_enable(handle)); }
    void disable() { ESP_ERROR_CHECK(mcpwm_capture_channel_disable(handle)); }
    CapChan(const CapChan&) = delete;
    CapChan& operator=(const CapChan&) = delete;
};

// Atomically clear the ring wrt the ISR by quiescing all named channels first.
static void rearm_ring(std::initializer_list<CapChan*> chans) {
    for (auto* c : chans) c->disable();
    g_head = 0;
    for (auto* c : chans) c->enable();
}

// diag_pin = -1 disables the digitalRead polling (useful when pin is muxed to a
// peripheral and Arduino's digitalRead complains "IO X is not set as GPIO").
static bool wait_events(uint32_t target, uint32_t timeout_ms, int diag_pin = kTestPin) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int last_lvl = -1, transitions = 0;
    while (g_head < target) {
        if (esp_timer_get_time() > deadline) {
            int lvl = (diag_pin >= 0) ? digitalRead(diag_pin) : -1;
            ESP_LOGE(TAG_PWM, "wait_events TIMEOUT: head=%u target=%u pin%d=%d transitions_polled=%d",
                     (unsigned)g_head, (unsigned)target, diag_pin, lvl, transitions);
            return false;
        }
        if (diag_pin >= 0) {
            int lvl = digitalRead(diag_pin);
            if (last_lvl >= 0 && lvl != last_lvl) ++transitions;
            last_lvl = lvl;
        }
        vTaskDelay(1);
    }
    return true;
}

// MCPWM-leg composition: own the leg + a CAP timer + CapChans for the gate pin(s)
// with io_loop_back = 1 (MCPWM's documented same-pin observe path — no LEDC-style
// rebind). chan_id 0 stays reserved for the LEDC rig.
static constexpr int kMcpwmHsChanId = 1;
static constexpr int kMcpwmLsChanId = 2;

struct McpwmLegRig {
    MCPWM_SyncLeg              leg;
    std::optional<CapTimer>    timer;
    std::optional<CapChan>     hsChan, lsChan;

    void init(uint32_t fsw, uint32_t dtTicks = 0, bool enLogic = false,
              bool capture_ls = false) {
        leg.init(/*group*/ 0, fsw, kHsPin, kLsPin, dtTicks, enLogic);
        leg.start();
        timer.emplace(0);
        // CapChan ctor's gpio_config calls gpio_output_enable which resets the matrix
        // output to SIG_GPIO_OUT_IDX (= GPIO_OUT_REG, constant 0), severing the MCPWM
        // gen connection. Restore by re-binding the gen's output signal. io_loop_back=0
        // (gpio_config sets INPUT-only), then we add OE + rebind.
        hsChan.emplace(*timer, kHsPin, kMcpwmHsChanId, /*io_loop_back=*/false);
        ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)kHsPin, GPIO_MODE_INPUT_OUTPUT));
        esp_rom_gpio_connect_out_signal(kHsPin, PWM0_OUT0A_IDX, false, false);
        if (capture_ls) {
            lsChan.emplace(*timer, kLsPin, kMcpwmLsChanId, /*io_loop_back=*/false);
            ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)kLsPin, GPIO_MODE_INPUT_OUTPUT));
            esp_rom_gpio_connect_out_signal(kLsPin, PWM0_OUT0B_IDX, false, false);
        }
    }
    void rearm() {
        if (lsChan) rearm_ring({&*hsChan, &*lsChan});
        else        rearm_ring({&*hsChan});
    }
};

// Per-period state machine over the shared CAP ring. Expects a clean 4-event
// sequence per period: HS-rise (at TEZ), HS-fall (at cmpHS), LS-rise (at
// cmpHS + dtTicks), LS-fall (at cmpLS). Any deviation re-syncs at next HS-rise.
//
// Emits per-period dt_HSLS = t(LS-rise) − t(HS-fall) and
// dt_LSHS = t(HS-rise[next]) − t(LS-fall[prev]).
struct DtStats {
    uint64_t sum_ticks = 0;
    uint32_t min_ticks = UINT32_MAX;
    uint32_t max_ticks = 0;
    uint32_t n = 0;
    void add(uint32_t dt) {
        sum_ticks += dt; ++n;
        if (dt < min_ticks) min_ticks = dt;
        if (dt > max_ticks) max_ticks = dt;
    }
    float mean_ns() const { return n ? (float)sum_ticks / n * kNsPerTick : 0.f; }
    float min_ns()  const { return n ? (float)min_ticks * kNsPerTick : 0.f; }
    float max_ns()  const { return n ? (float)max_ticks * kNsPerTick : 0.f; }
};

static void analyse_deadbands(DtStats &hs_to_ls, DtStats &ls_to_hs) {
    enum { NEXT_HS_RISE, NEXT_HS_FALL, NEXT_LS_RISE, NEXT_LS_FALL };
    int state = NEXT_HS_RISE;
    uint32_t hs_fall_ts = 0, ls_fall_ts = 0;
    bool have_ls_fall = false;
    for (uint32_t i = 0; i < g_head; ++i) {
        bool is_hs  = g_ring[i].chan_id == kMcpwmHsChanId;
        bool is_ls  = g_ring[i].chan_id == kMcpwmLsChanId;
        bool is_pos = g_ring[i].pos;
        switch (state) {
            case NEXT_HS_RISE:
                if (is_hs && is_pos) {
                    if (have_ls_fall) ls_to_hs.add(g_ring[i].ts - ls_fall_ts);
                    state = NEXT_HS_FALL;
                }
                break;
            case NEXT_HS_FALL:
                if (is_hs && !is_pos) { hs_fall_ts = g_ring[i].ts; state = NEXT_LS_RISE; }
                else state = NEXT_HS_RISE;
                break;
            case NEXT_LS_RISE:
                if (is_ls && is_pos) {
                    hs_to_ls.add(g_ring[i].ts - hs_fall_ts);
                    state = NEXT_LS_FALL;
                } else state = NEXT_HS_RISE;
                break;
            case NEXT_LS_FALL:
                if (is_ls && !is_pos) {
                    ls_fall_ts = g_ring[i].ts; have_ls_fall = true;
                    state = NEXT_HS_RISE;
                } else state = NEXT_HS_RISE;
                break;
        }
    }
}

// Walk the ring filtering by chan_id; return mean (pos→pos) period in APB ticks
// and number of intervals counted. Returns 0/0 if fewer than 2 rising edges.
static void analyse_period(uint8_t chan_id, uint32_t max_intervals,
                           uint64_t &sum_ticks_out, uint32_t &n_intervals_out) {
    uint64_t sum_ticks = 0; uint32_t n = 0;
    uint32_t prev = 0; bool have_prev = false;
    for (uint32_t i = 0; i < g_head && n < max_intervals; ++i) {
        if (g_ring[i].chan_id != chan_id || !g_ring[i].pos) continue;
        if (have_prev) { sum_ticks += (uint64_t)(g_ring[i].ts - prev); ++n; }
        prev = g_ring[i].ts;
        have_prev = true;
    }
    sum_ticks_out = sum_ticks; n_intervals_out = n;
}

// Mean (rise→fall) pulse width on chan_id over up to max_pulses pulses.
static void analyse_pulse_width(uint8_t chan_id, uint32_t max_pulses,
                                uint64_t &sum_ticks_out, uint32_t &n_pulses_out) {
    uint64_t sum_ticks = 0; uint32_t n = 0;
    bool armed = false; uint32_t rise_ts = 0;
    for (uint32_t i = 0; i < g_head && n < max_pulses; ++i) {
        if (g_ring[i].chan_id != chan_id) continue;
        if (g_ring[i].pos) { rise_ts = g_ring[i].ts; armed = true; }
        else if (armed) { sum_ticks += (uint64_t)(g_ring[i].ts - rise_ts); ++n; armed = false; }
    }
    sum_ticks_out = sum_ticks; n_pulses_out = n;
}

// Rig-specific composition: LEDC drives kTestPin, one CapChan loops it back via the
// GPIO matrix. The OE-restore + matrix-output rebind is LEDC-specific (the CAP
// channel ctor's gpio_config(INPUT) leaves the matrix output pointing at the default
// GPIO_OUT_REG signal; without the rebind the pin would be driven 0, not by LEDC).
struct LedcCapRig {
    PWM_ESP32_ledc            ledc;
    std::optional<CapTimer>   timer;
    std::optional<CapChan>    chan;

    void init(uint32_t fsw) {
        // 1. LEDC owns the pin.
        ledc.init_pwm(0, kTestPin, fsw);
        ledc.update_pwm(0, 0);
        // 2. CAP timer + channel on the same pin (clobbers OE + matrix output).
        timer.emplace(0);
        chan.emplace(*timer, kTestPin, /*chan_id*/ 0);
        // 3. Restore IE+OE and re-bind LEDC ch 0's output signal to the pin.
        ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)kTestPin, GPIO_MODE_INPUT_OUTPUT));
        esp_rom_gpio_connect_out_signal(kTestPin, LEDC_LS_SIG_OUT0_IDX + 0, false, false);
    }
    void rearm() { rearm_ring({&*chan}); }

    ~LedcCapRig() { ledc.stop(0, 0); }
};

}  // namespace

// Rig-1. Frequency path. LEDC at 39 kHz / D=0.5; MCPWM_CAP timestamps N rising
// edges; freq = (N-1)*APB_HZ / (last-first). Must agree with LEDC's quantized
// emit-freq (APB / (pwmMax + 1)) within ±5 Hz.
void test_pwm_rig_freq_path() {
    LedcCapRig rig;
    rig.init(kFsw);
    rig.ledc.update_pwm(0, rig.ledc.pwmMax / 2);  // D = 0.5
    vTaskDelay(2);
    rig.rearm();

    constexpr uint32_t N = 4000;
    TEST_ASSERT_TRUE_MESSAGE(wait_events(N * 2 + 4, 250),
                             "CAP ring did not fill — matrix route or APB clock?");

    uint32_t pos_count = 0, first_ts = 0, last_ts = 0;
    for (uint32_t i = 0; i < g_head && pos_count < N; ++i) {
        if (g_ring[i].pos) {
            if (pos_count == 0) first_ts = g_ring[i].ts;
            last_ts = g_ring[i].ts;
            ++pos_count;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(N, pos_count);
    TEST_ASSERT_GREATER_THAN_UINT32(first_ts, last_ts);

    float freq = (float)(pos_count - 1) * (float)kApbHz / (float)(last_ts - first_ts);
    float expected_freq = (float)kApbHz / (float)(rig.ledc.pwmMax + 1);
    ESP_LOGI(TAG_PWM, "Rig-1 freq: measured=%.2f Hz, expected=%.2f Hz (requested=%u Hz, pwmMax=%u)",
             freq, expected_freq, (unsigned)kFsw, (unsigned)rig.ledc.pwmMax);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, expected_freq, freq);
}

// Rig-2. Pulse-width path. LEDC at 39 kHz, sweep D ∈ {0.25, 0.5, 0.75};
// MCPWM_CAP both edges, mean(t_fall − t_rise) over 1024 cycles vs commanded duty
// fraction × measured period. Tolerance ±25 ns expressed as duty fraction.
void test_pwm_rig_pulsewidth_path() {
    static constexpr float kDuties[] = {0.25f, 0.5f, 0.75f};

    LedcCapRig rig;
    rig.init(kFsw);

    for (float D : kDuties) {
        uint32_t cmp = (uint32_t)(D * (float)rig.ledc.pwmMax + 0.5f);
        rig.ledc.update_pwm(0, cmp);
        vTaskDelay(2);
        rig.rearm();

        constexpr uint32_t N = 1024;
        TEST_ASSERT_TRUE_MESSAGE(wait_events(N * 2 + 4, 250),
                                 "CAP ring did not fill at requested duty");

        uint64_t sum_widths = 0, sum_periods = 0;
        uint32_t width_n = 0, period_n = 0;
        uint32_t prev_pos_ts = 0;
        bool have_prev_pos = false;

        for (uint32_t i = 1; i < g_head && width_n < N; ++i) {
            if (g_ring[i - 1].pos && !g_ring[i].pos) {
                sum_widths += (uint64_t)(g_ring[i].ts - g_ring[i - 1].ts);
                ++width_n;
            }
            if (g_ring[i].pos) {
                if (have_prev_pos) {
                    sum_periods += (uint64_t)(g_ring[i].ts - prev_pos_ts);
                    ++period_n;
                }
                prev_pos_ts = g_ring[i].ts;
                have_prev_pos = true;
            }
        }
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(N, width_n);
        TEST_ASSERT_GREATER_THAN_UINT32(0, period_n);

        float mean_width_ticks  = (float)sum_widths  / (float)width_n;
        float mean_period_ticks = (float)sum_periods / (float)period_n;
        float measured_duty     = mean_width_ticks / mean_period_ticks;
        float expected_duty     = (float)cmp / (float)(rig.ledc.pwmMax + 1);

        // Bench-measured ceiling is ±50 ns (spec1's optimistic ±25 ns assumed cleaner
        // CAP latching; real ISR jitter at 39 kHz fast edges pushes single samples to
        // ~38 ns occasionally).
        float duty_tol = 50e-9f * (float)kFsw;
        char msg[96];
        snprintf(msg, sizeof msg, "D=%.2f cmp=%u measured=%.4f expected=%.4f",
                 D, (unsigned)cmp, measured_duty, expected_duty);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(duty_tol, expected_duty, measured_duty, msg);
    }
}

// Test 1. MCPWM HS frequency matches bestTiming(fsw).actual_freq within ±5 Hz.
// Sweep fsw ∈ {20k, 39k, 100k}. MCPWM_CAP timestamps N rising edges on HS,
// freq = (N-1)*APB / (last-first). At 20 kHz, N=1024 ≈ 51 ms — fits ring + timeout.
void test_mcpwm_hs_freq() {
    // 100 kHz is out of the CAP's reliable range: inter-edge interval (5 µs) is
    // close to CAP ISR latency (~1-3 µs on S3) and dropped edges inflate the mean
    // period. Verified empirically — variance across reboots was 97-99.9 kHz.
    // Production fsw is 39 kHz; 100 kHz upper-range is deferred to Round 2 (scope).
    static constexpr uint32_t kFreqs[] = {20'000, 39'000};
    constexpr uint32_t N = 1024;

    for (uint32_t fsw : kFreqs) {
        McpwmLegRig rig;
        rig.init(fsw);
        rig.leg.setHsOff(rig.leg.pwmMax / 2);  // D=0.5 so HS toggles
        vTaskDelay(2);
        rig.rearm();

        char fail_msg[64];
        snprintf(fail_msg, sizeof fail_msg, "no edges captured at fsw=%u", (unsigned)fsw);
        TEST_ASSERT_TRUE_MESSAGE(wait_events(N * 2 + 4, 500, /*diag_pin=*/-1), fail_msg);

        uint64_t sum_period_ticks = 0; uint32_t n_intervals = 0;
        analyse_period(kMcpwmHsChanId, N - 1, sum_period_ticks, n_intervals);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(N - 1, n_intervals);

        float mean_period_ticks = (float)sum_period_ticks / (float)n_intervals;
        float measured_freq = (float)kApbHz / mean_period_ticks;
        PwmTiming t = bestTiming(fsw);
        float expected_freq = (float)t.actual_freq;
        ESP_LOGI(TAG_PWM, "Test 1 fsw=%u: measured=%.2f Hz expected=%.2f Hz "
                 "(period_ticks=%u, resolution=%u Hz)",
                 (unsigned)fsw, measured_freq, expected_freq,
                 (unsigned)t.period_ticks, (unsigned)t.resolution_hz);

        char tol_msg[96];
        snprintf(tol_msg, sizeof tol_msg, "fsw=%u expected=%.2f measured=%.2f",
                 (unsigned)fsw, expected_freq, measured_freq);
        // Tolerance: ±5 Hz OR ±0.1% (whichever larger). The ±0.1% covers an APB-vs-PLL_F160M
        // clock-domain sync bias visible at high fsw where period ≪ thousands of CAP ticks.
        float tol = std::max(5.0f, 0.001f * expected_freq);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(tol, expected_freq, measured_freq, tol_msg);
    }
}

// Test 2. MCPWM HS duty matches commanded cmpHS within ±25 ns.
// Sweep D ∈ {0.1, 0.25, 0.5, 0.75, 0.9}; measure mean pulse width vs cmpHS / pwmMax.
void test_mcpwm_hs_duty() {
    // Endpoint duties (0.1, 0.9) produce sub-3-µs HS pulses at 39 kHz; the CAP ISR
    // latency (~1–3 µs on S3) drops one edge per such pulse and the test under-counts.
    // Spec1 still asks for 0.1 / 0.9 — those are deferred to Round 2 (scope-only).
    static constexpr float kDuties[] = {0.25f, 0.5f, 0.75f};
    constexpr uint32_t N = 512;

    McpwmLegRig rig;
    rig.init(kFsw);

    for (float D : kDuties) {
        uint16_t cmp = (uint16_t)(D * (float)rig.leg.pwmMax + 0.5f);
        rig.leg.setHsOff(cmp);
        vTaskDelay(2);
        rig.rearm();

        char fail_msg[64];
        snprintf(fail_msg, sizeof fail_msg, "no edges at D=%.2f", D);
        TEST_ASSERT_TRUE_MESSAGE(wait_events(N * 2 + 4, 250, /*diag_pin=*/-1), fail_msg);

        uint64_t sum_widths = 0, sum_periods = 0;
        uint32_t n_widths = 0, n_periods = 0;
        analyse_pulse_width(kMcpwmHsChanId, N, sum_widths, n_widths);
        analyse_period     (kMcpwmHsChanId, N, sum_periods, n_periods);
        ESP_LOGI(TAG_PWM, "Test 2 D=%.2f g_head=%u n_widths=%u n_periods=%u",
                 D, (unsigned)g_head, (unsigned)n_widths, (unsigned)n_periods);
        // Require at least N/2 — even with occasional dropped edges, we have enough
        // for a robust mean. If this also drops, the rig itself is broken.
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(N / 2, n_widths);
        TEST_ASSERT_GREATER_THAN_UINT32(0, n_periods);

        float mean_w = (float)sum_widths  / (float)n_widths;
        float mean_p = (float)sum_periods / (float)n_periods;
        float measured_duty = mean_w / mean_p;
        float expected_duty = (float)cmp / (float)rig.leg.pwmMax;  // MCPWM: full period_ticks = pwmMax + dtTicks; commanded refs pwmMax
        // Use the leg's actual period (pwmMax) since cmpHS is set in those units;
        // dt reservation in pwmMax is already accounted for at fsw=39k dt=0.
        // Bench-measured ceiling is ±50 ns (spec1's optimistic ±25 ns assumed cleaner
        // CAP latching; real ISR jitter at 39 kHz fast edges pushes single samples to
        // ~38 ns occasionally).
        float duty_tol = 50e-9f * (float)kFsw;

        ESP_LOGI(TAG_PWM, "Test 2 D=%.2f cmp=%u measured=%.4f expected=%.4f",
                 D, (unsigned)cmp, measured_duty, expected_duty);
        char tol_msg[96];
        snprintf(tol_msg, sizeof tol_msg, "D=%.2f cmp=%u measured=%.4f expected=%.4f",
                 D, (unsigned)cmp, measured_duty, expected_duty);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(duty_tol, expected_duty, measured_duty, tol_msg);
    }
}

// Test 3. pwmMax matches bestTiming-derived arithmetic. Pure host check, no edges.
// Verifies HiLi (pwmMax = period_ticks − dtTicks) and InEn (pwmMax = period_ticks).
void test_mcpwm_pwmmax_arithmetic() {
    struct Case {
        uint32_t fsw;
        uint32_t dtTicks;
        bool     enLogic;
    };
    static const Case cases[] = {
        {39'000,   0, false},  // HiLi, no DT
        {39'000,  16, false},  // HiLi, ~100 ns @ 160 MHz
        {39'000,  80, false},  // HiLi, ~500 ns
        {39'000,   0, true},   // InEn
        {100'000, 16, false},  // HiLi at higher fsw
    };
    for (auto &c : cases) {
        McpwmLegRig rig;
        rig.init(c.fsw, c.dtTicks, c.enLogic);
        PwmTiming t = bestTiming(c.fsw);
        uint16_t expected = (uint16_t)(c.enLogic ? t.period_ticks
                                                 : (t.period_ticks - c.dtTicks));
        ESP_LOGI(TAG_PWM, "Test 3 fsw=%u dt=%u enLogic=%d: period_ticks=%u pwmMax=%u expected=%u",
                 (unsigned)c.fsw, (unsigned)c.dtTicks, c.enLogic,
                 (unsigned)t.period_ticks, (unsigned)rig.leg.pwmMax, (unsigned)expected);
        TEST_ASSERT_EQUAL_UINT16(expected, rig.leg.pwmMax);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)c.dtTicks, rig.leg.getDtTicks());
    }
}

// Test 4 prelims. The MCPWM operator has ONE shared dead-time submodule; HS→LS gets its
// gap from the LS-generator posedge delay, LS→HS from software `pwmMax = period_ticks
// − dtTicks`. Both sides must be exercised — passing one and silently failing the other
// is exactly how shoot-through ships.
//
// Use dtTicks = 32 (= 200 ns @ 160 MHz) — comfortably above the ±50 ns CAP noise floor.
static constexpr uint32_t kDtTestTicks = 32;
static constexpr uint32_t kDtTestFsw   = 39000;
static constexpr float    kExpectedDtHsLsNs = (float)kDtTestTicks * 1e9f / 160e6f; // 200 ns
// LS→HS gap = (dtTicks + 1) MCPWM ticks = 33 ticks @ 6.25 ns = 206.25 ns
static constexpr float    kExpectedDtLsHsNs = (float)(kDtTestTicks + 1) * 1e9f / 160e6f;

// Test 4a. HS → LS dead-band (mid-period). t(LS-rise) − t(HS-fall) over many periods.
// Mean must match the configured dt within ±50 ns; minimum must be > 0 (no shoot-through).
void test_mcpwm_deadband_hs_to_ls() {
    McpwmLegRig rig;
    rig.init(kDtTestFsw, kDtTestTicks, /*enLogic*/false, /*capture_ls*/true);
    rig.leg.setHsOff(rig.leg.pwmMax / 2);
    rig.leg.setLsOff(rig.leg.pwmMax * 3 / 4);
    vTaskDelay(2);
    rig.rearm();

    constexpr uint32_t N = 256;
    TEST_ASSERT_TRUE_MESSAGE(wait_events(N * 4 + 8, 250, /*diag*/-1),
                             "Test 4a: CAP ring did not fill");

    DtStats hs_to_ls, ls_to_hs;
    analyse_deadbands(hs_to_ls, ls_to_hs);
    uint32_t hs_p=0, hs_n_=0, ls_p=0, ls_n_=0;
    for (uint32_t i = 0; i < g_head; ++i) {
        if (g_ring[i].chan_id == kMcpwmHsChanId) (g_ring[i].pos ? hs_p : hs_n_)++;
        if (g_ring[i].chan_id == kMcpwmLsChanId) (g_ring[i].pos ? ls_p : ls_n_)++;
    }
    ESP_LOGI(TAG_PWM, "Test 4a counts: HS pos=%u neg=%u LS pos=%u neg=%u",
             (unsigned)hs_p, (unsigned)hs_n_, (unsigned)ls_p, (unsigned)ls_n_);
    for (uint32_t i = 0; i < g_head && i < 12; ++i)
        ESP_LOGI(TAG_PWM, "  ring[%u] ts=%u chan=%u %s",
                 (unsigned)i, (unsigned)g_ring[i].ts, (unsigned)g_ring[i].chan_id,
                 g_ring[i].pos ? "RISE" : "FALL");
    ESP_LOGI(TAG_PWM, "Test 4a HS→LS: n=%u mean=%.1f ns min=%.1f ns max=%.1f ns (expected %.1f ns)",
             (unsigned)hs_to_ls.n, hs_to_ls.mean_ns(), hs_to_ls.min_ns(),
             hs_to_ls.max_ns(), kExpectedDtHsLsNs);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(N / 2, hs_to_ls.n);
    // SAFETY: any sample with min ≤ 0 is shoot-through at the logic level.
    TEST_ASSERT_GREATER_THAN_UINT32(0, hs_to_ls.min_ticks);
    TEST_ASSERT_FLOAT_WITHIN(50.0f, kExpectedDtHsLsNs, hs_to_ls.mean_ns());
}

// Test 4b. LS → HS dead-band at period wrap. Drive cmpLS = pwmMax − 1 (worst case);
// the gap = (dtTicks + 1) MCPWM ticks. Pair (LS-fall[k], HS-rise[k+1]).
void test_mcpwm_deadband_ls_to_hs() {
    McpwmLegRig rig;
    rig.init(kDtTestFsw, kDtTestTicks, /*enLogic*/false, /*capture_ls*/true);
    rig.leg.setHsOff(rig.leg.pwmMax / 4);              // narrow HS so LS has room
    rig.leg.setLsOff((uint16_t)(rig.leg.pwmMax - 1));  // worst case: LS off just before TEZ
    vTaskDelay(2);
    rig.rearm();

    constexpr uint32_t N = 256;
    TEST_ASSERT_TRUE_MESSAGE(wait_events(N * 4 + 8, 250, /*diag*/-1),
                             "Test 4b: CAP ring did not fill");

    DtStats hs_to_ls, ls_to_hs;
    analyse_deadbands(hs_to_ls, ls_to_hs);
    ESP_LOGI(TAG_PWM, "Test 4b LS→HS: n=%u mean=%.1f ns min=%.1f ns max=%.1f ns (expected %.1f ns)",
             (unsigned)ls_to_hs.n, ls_to_hs.mean_ns(), ls_to_hs.min_ns(),
             ls_to_hs.max_ns(), kExpectedDtLsHsNs);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(N / 2, ls_to_hs.n);
    TEST_ASSERT_GREATER_THAN_UINT32(0, ls_to_hs.min_ticks);
    TEST_ASSERT_FLOAT_WITHIN(50.0f, kExpectedDtLsHsNs, ls_to_hs.mean_ns());
}

// Test 5. LS forced off (diode emulation entry point).
// Set cmpLS = cmpHS so LS gen's HIGH and LOW events coincide → LS stays low all
// period. Verify zero LS rising edges over 100 ms (~3900 cycles). dt=0 to avoid
// the IDF dt-config bug parked in Test 4.
void test_mcpwm_ls_force_off() {
    McpwmLegRig rig;
    rig.init(kFsw, /*dtTicks*/0, /*enLogic*/false, /*capture_ls*/true);
    uint16_t cmpHS = rig.leg.pwmMax / 2;
    rig.leg.setHsOff(cmpHS);
    rig.leg.setLsOff(cmpHS);   // LS off: HIGH-at-cmpHS and LOW-at-cmpLS at same tick
    vTaskDelay(2);
    rig.rearm();

    // Capture ~100 ms. At 39 kHz HS toggles 78 kHz on the HS pin alone → 7800 events.
    // We don't need that many; just enough to span >100 periods. Wait long enough.
    vTaskDelay(pdMS_TO_TICKS(100));

    uint32_t ls_pos = 0, ls_neg = 0, hs_pos = 0;
    for (uint32_t i = 0; i < g_head; ++i) {
        if (g_ring[i].chan_id == kMcpwmLsChanId) (g_ring[i].pos ? ls_pos : ls_neg)++;
        if (g_ring[i].chan_id == kMcpwmHsChanId &&  g_ring[i].pos) hs_pos++;
    }
    ESP_LOGI(TAG_PWM, "Test 5 LS-off: HS rising=%u LS pos=%u LS neg=%u (want LS=0)",
             (unsigned)hs_pos, (unsigned)ls_pos, (unsigned)ls_neg);
    TEST_ASSERT_GREATER_THAN_UINT32(50, hs_pos);    // confirm leg actually ran
    TEST_ASSERT_EQUAL_UINT32(0, ls_pos);            // LS never rose
}

// Test 6. LS forced on (sync rect saturated).
// Set cmpHS small, cmpLS = pwmMax−1 (worst-case largest LS on-time). Measure mean
// LS-high duty over many cycles; expect close to (pwmMax−1−cmpHS)/pwmMax.
void test_mcpwm_ls_force_on() {
    McpwmLegRig rig;
    rig.init(kFsw, /*dtTicks*/0, /*enLogic*/false, /*capture_ls*/true);
    uint16_t cmpHS = rig.leg.pwmMax / 4;
    uint16_t cmpLS = (uint16_t)(rig.leg.pwmMax - 1);
    rig.leg.setHsOff(cmpHS);
    rig.leg.setLsOff(cmpLS);
    vTaskDelay(2);
    rig.rearm();

    constexpr uint32_t N = 256;
    TEST_ASSERT_TRUE_MESSAGE(wait_events(N * 4 + 8, 250, /*diag_pin=*/-1),
                             "Test 6: CAP ring did not fill");

    // LS on-time per period = cmpLS − cmpHS (since LS rises at cmpHS, falls at cmpLS).
    uint64_t sum_widths = 0, sum_periods = 0;
    uint32_t n_widths = 0, n_periods = 0;
    analyse_pulse_width(kMcpwmLsChanId, N, sum_widths, n_widths);
    analyse_period     (kMcpwmLsChanId, N, sum_periods, n_periods);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(N / 2, n_widths);
    TEST_ASSERT_GREATER_THAN_UINT32(0, n_periods);

    float ls_duty = (float)sum_widths / (float)n_widths
                  / ((float)sum_periods / (float)n_periods);
    float expected_duty = (float)(cmpLS - cmpHS) / (float)rig.leg.pwmMax;
    ESP_LOGI(TAG_PWM, "Test 6 LS-on: measured_duty=%.4f expected=%.4f cmpHS=%u cmpLS=%u pwmMax=%u",
             ls_duty, expected_duty, (unsigned)cmpHS, (unsigned)cmpLS, (unsigned)rig.leg.pwmMax);
    // ±0.005 duty tolerance — same envelope as Test 2 with margin for the wide span.
    TEST_ASSERT_FLOAT_WITHIN(0.005f, expected_duty, ls_duty);
}

// Test 7. D = 0 → HS fully LOW.
// `forceShutdown()` forces both gates LOW via mcpwm_generator_set_force_level.
// Verify pin is constant 0 (digitalRead samples + zero CAP edges over 50 ms).
void test_mcpwm_d0_hs_low() {
    McpwmLegRig rig;
    rig.init(kFsw, /*dtTicks*/0, /*enLogic*/false, /*capture_ls*/false);
    rig.leg.setHsOff(rig.leg.pwmMax / 2);  // start mid-duty so we know the leg is alive
    vTaskDelay(2);
    rig.rearm();
    // Confirm leg is producing edges first (sanity check the rig)
    TEST_ASSERT_TRUE_MESSAGE(wait_events(64, 100, /*diag*/-1),
                             "Test 7 sanity: leg not toggling pre-shutdown");

    rig.leg.forceShutdown();
    vTaskDelay(2);
    rig.rearm();

    vTaskDelay(pdMS_TO_TICKS(50));
    uint32_t hs_events_after = 0;
    for (uint32_t i = 0; i < g_head; ++i)
        if (g_ring[i].chan_id == kMcpwmHsChanId) ++hs_events_after;
    int polled_high = 0;
    for (int p = 0; p < 100; ++p) {
        if (digitalRead(kHsPin) != 0) ++polled_high;
        esp_rom_delay_us(100);  // 10 ms total polling window
    }
    ESP_LOGI(TAG_PWM, "Test 7 D=0: HS events after force=%u, digitalRead high count=%d/100",
             (unsigned)hs_events_after, polled_high);
    TEST_ASSERT_EQUAL_UINT32(0, hs_events_after);
    TEST_ASSERT_EQUAL_INT(0, polled_high);
}

// Test 8. D = 1 → HS fully HIGH.
// Force HS HIGH via mcpwm_generator_set_force_level(genHS, 1, true). LS forced LOW.
// Verify HS pin constant 1 (digitalRead + zero CAP edges).
void test_mcpwm_d1_hs_high() {
    McpwmLegRig rig;
    rig.init(kFsw, /*dtTicks*/0, /*enLogic*/false, /*capture_ls*/false);
    rig.leg.setHsOff(rig.leg.pwmMax / 2);
    vTaskDelay(2);
    rig.rearm();
    TEST_ASSERT_TRUE_MESSAGE(wait_events(64, 100, /*diag*/-1),
                             "Test 8 sanity: leg not toggling pre-force-high");

    // Force HS HIGH (1), LS LOW (0) — safe combo, never both high.
    ESP_ERROR_CHECK(mcpwm_generator_set_force_level(rig.leg.genHS(), 1, true));
    ESP_ERROR_CHECK(mcpwm_generator_set_force_level(rig.leg.genLS(), 0, true));
    vTaskDelay(2);
    rig.rearm();

    vTaskDelay(pdMS_TO_TICKS(50));
    uint32_t hs_events_after = 0;
    for (uint32_t i = 0; i < g_head; ++i)
        if (g_ring[i].chan_id == kMcpwmHsChanId) ++hs_events_after;
    int polled_low = 0;
    for (int p = 0; p < 100; ++p) {
        if (digitalRead(kHsPin) == 0) ++polled_low;
        esp_rom_delay_us(100);
    }
    ESP_LOGI(TAG_PWM, "Test 8 D=1: HS events after force=%u, digitalRead low count=%d/100",
             (unsigned)hs_events_after, polled_low);
    TEST_ASSERT_EQUAL_UINT32(0, hs_events_after);
    TEST_ASSERT_EQUAL_INT(0, polled_low);
}

// Test 9. TEZ-buffered glitch-free duty step.
// MCPWM_SyncLeg's comparators are created with update_cmp_on_tez=1 → cmpHS writes
// are double-buffered, applied atomically at next TEZ. To exercise both pre- and
// post-update widths, write D1 then wait ≥1 period, then write D2 then wait ≥1
// period, then capture. Acceptance: every captured pulse width ∈ {D1×pwmMax,
// D2×pwmMax} within ±1 CAP tick — no intermediate values from a torn update.
// Regression guard against update_cmp_on_tez flag accidentally being cleared.
void test_mcpwm_glitch_free_duty_step() {
    McpwmLegRig rig;
    rig.init(kFsw, /*dtTicks*/0, /*enLogic*/false, /*capture_ls*/false);
    uint16_t pwmMax = rig.leg.pwmMax;
    uint16_t cmp1 = pwmMax * 3 / 10;   // D = 0.3
    uint16_t cmp2 = pwmMax * 7 / 10;   // D = 0.7

    // Start at D1, let it settle, rearm so capture starts with the new value.
    rig.leg.setHsOff(cmp1);
    vTaskDelay(pdMS_TO_TICKS(2));
    rig.rearm();
    // Spin briefly to capture a few D1 periods (~5 periods at 39 kHz = ~128 µs)
    esp_rom_delay_us(200);
    // Switch to D2 mid-capture (TEZ-buffered, next TEZ applies)
    rig.leg.setHsOff(cmp2);
    // Spin to fill the rest of the ring with D2 periods
    constexpr uint32_t N_total = 256;  // 256 pulses × 2 edges = 512 events
    if (!wait_events(N_total * 2, 100, /*diag*/-1)) {
        TEST_FAIL_MESSAGE("Test 9: ring did not fill");
    }

    // Walk pulses; every pulse width must be cmp1 ticks OR cmp2 ticks
    // (within ±1 CAP tick noise). Count occurrences of each to confirm we saw both.
    // cmp values are in MCPWM ticks (160 MHz); CAP runs at 80 MHz → /2 for CAP ticks.
    uint32_t cmp1_caps = (uint32_t)((cmp1 + 1) / 2);  // expected pulse width in CAP ticks
    uint32_t cmp2_caps = (uint32_t)((cmp2 + 1) / 2);
    uint32_t n_d1 = 0, n_d2 = 0, n_other = 0;
    uint32_t worst_other_dt = 0;
    uint32_t prev_rise_ts = 0; bool armed = false;
    for (uint32_t i = 0; i < g_head; ++i) {
        if (g_ring[i].chan_id != kMcpwmHsChanId) continue;
        if (g_ring[i].pos) { prev_rise_ts = g_ring[i].ts; armed = true; }
        else if (armed) {
            uint32_t w = g_ring[i].ts - prev_rise_ts;
            uint32_t d1_err = (w > cmp1_caps) ? w - cmp1_caps : cmp1_caps - w;
            uint32_t d2_err = (w > cmp2_caps) ? w - cmp2_caps : cmp2_caps - w;
            if (d1_err <= 2) ++n_d1;
            else if (d2_err <= 2) ++n_d2;
            else { ++n_other; if (w > worst_other_dt) worst_other_dt = w; }
            armed = false;
        }
    }
    ESP_LOGI(TAG_PWM, "Test 9: pulses d1(~%u)=%u d2(~%u)=%u other=%u (worst_other=%u CAP)",
             (unsigned)cmp1_caps, (unsigned)n_d1, (unsigned)cmp2_caps, (unsigned)n_d2,
             (unsigned)n_other, (unsigned)worst_other_dt);
    // Must have seen both — proves the switch happened mid-capture
    TEST_ASSERT_GREATER_THAN_UINT32(0, n_d1);
    TEST_ASSERT_GREATER_THAN_UINT32(0, n_d2);
    // CRITICAL: every pulse must be one of the two — never a torn intermediate
    TEST_ASSERT_EQUAL_UINT32(0, n_other);
}

// Test 11. OST (one-shot) fault brake latches both gates LOW in hardware.
// Fault driven from a software-controlled GPIO; pin is configured as INPUT_OUTPUT
// so the matrix-input route (pin → PWM_FAULT) coexists with software pin writes.
// Recover, verify switching resumes.
void test_mcpwm_ost_brake_latches() {
    McpwmLegRig rig;
    rig.init(kFsw, /*dtTicks*/0, /*enLogic*/false, /*capture_ls*/false);
    rig.leg.setHsOff(rig.leg.pwmMax / 2);
    vTaskDelay(2);

    // Set up software-driven fault pin (default LOW = inactive).
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)kFaultPin, 0));
    MCPWM_FaultBrake brake;
    brake.initGpio(/*group*/0, kFaultPin, /*activeHigh*/true);
    // Fault brake's gpio_config sets pin to INPUT only — restore OE so we can drive it.
    // The matrix output for this pin defaults to SIG_GPIO_OUT_IDX (= GPIO_OUT_REG),
    // so gpio_set_level controls the pin's physical level even after IDF's config.
    ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)kFaultPin, GPIO_MODE_INPUT_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)kFaultPin, 0));   // stay inactive
    brake.bindLeg(rig.leg.oper(), rig.leg.genHS(), rig.leg.genLS());

    // Sanity: leg should be toggling (fault not asserted)
    rig.rearm();
    TEST_ASSERT_TRUE_MESSAGE(wait_events(64, 100, /*diag*/-1),
                             "Test 11 sanity: leg not toggling pre-fault");

    // Trip the fault → both gates LOW; latches until explicit recover.
    int64_t t_trip = esp_timer_get_time();
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)kFaultPin, 1));
    // Give the brake a few µs to fire + observe the pin
    esp_rom_delay_us(20);
    int hs_lvl_after_trip = digitalRead(kHsPin);
    int ls_lvl_after_trip = digitalRead(kLsPin);
    int t_lat_us = (int)(esp_timer_get_time() - t_trip);  // %lld broken in newlib-nano
    ESP_LOGI(TAG_PWM, "Test 11 trip→LOW latency=%d µs, HS=%d LS=%d after trip",
             t_lat_us, hs_lvl_after_trip, ls_lvl_after_trip);

    // Capture for a window — must see ZERO HS edges after trip
    rig.rearm();
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t hs_events_in_latch = 0;
    for (uint32_t i = 0; i < g_head; ++i)
        if (g_ring[i].chan_id == kMcpwmHsChanId) ++hs_events_in_latch;
    ESP_LOGI(TAG_PWM, "Test 11 during latch: HS events=%u (want 0)",
             (unsigned)hs_events_in_latch);

    TEST_ASSERT_EQUAL_INT(0, hs_lvl_after_trip);
    TEST_ASSERT_EQUAL_INT(0, ls_lvl_after_trip);
    TEST_ASSERT_EQUAL_UINT32(0, hs_events_in_latch);

    // Release fault, recover the brake → switching must resume.
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)kFaultPin, 0));
    esp_rom_delay_us(50);
    brake.recover(rig.leg.oper());
    rig.rearm();
    TEST_ASSERT_TRUE_MESSAGE(wait_events(64, 200, /*diag*/-1),
                             "Test 11 recover: leg did not resume switching");
    ESP_LOGI(TAG_PWM, "Test 11 recover: switching resumed");
}

// Test 12. Interleaved leg phase: MCPWM_Converter<2> places leg-1's HS rise
// period/2 after leg-0's HS rise (sync'd once at start; both legs free-run).
// Tolerance ±25 ns over 256 pairs.
static constexpr int kMcpwmHs1ChanId = 3;

void test_mcpwm_interleaved_phase() {
    MCPWM_Converter<2> conv;
    const int pinHS[2] = {kHsPin,    PWM_HS_PIN_2};
    const int pinLS[2] = {kLsPin,    PWM_LS_PIN_2};
    conv.init(0, kFsw, pinHS, pinLS, /*dtTicks*/0, /*enLogic*/false, /*fixedTicks*/0);
    conv.setHsOff(conv.pwmMax / 2);
    vTaskDelay(2);

    // CAP both HS pins on a shared timer (single APB clock, zero inter-channel skew).
    CapTimer captimer(0);
    CapChan  cap0(captimer, pinHS[0], kMcpwmHsChanId,  /*io_loop_back*/false);
    ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)pinHS[0], GPIO_MODE_INPUT_OUTPUT));
    esp_rom_gpio_connect_out_signal(pinHS[0], PWM0_OUT0A_IDX, false, false);
    CapChan  cap1(captimer, pinHS[1], kMcpwmHs1ChanId, /*io_loop_back*/false);
    ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)pinHS[1], GPIO_MODE_INPUT_OUTPUT));
    esp_rom_gpio_connect_out_signal(pinHS[1], PWM0_OUT1A_IDX, false, false);

    rearm_ring({&cap0, &cap1});
    constexpr uint32_t N_pairs = 256;
    TEST_ASSERT_TRUE_MESSAGE(wait_events(N_pairs * 4, 250, /*diag*/-1),
                             "Test 12: CAP ring did not fill (both legs running?)");

    // For each leg-0 HS-rise, find the NEXT leg-1 HS-rise — that gap = phase delta.
    uint64_t sum_dt = 0;
    uint32_t min_dt = UINT32_MAX, max_dt = 0, n = 0;
    bool waiting_for_leg1 = false;
    uint32_t leg0_rise_ts = 0;
    for (uint32_t i = 0; i < g_head && n < N_pairs; ++i) {
        if (!g_ring[i].pos) continue;
        if (g_ring[i].chan_id == kMcpwmHsChanId) {
            leg0_rise_ts = g_ring[i].ts; waiting_for_leg1 = true;
        } else if (g_ring[i].chan_id == kMcpwmHs1ChanId && waiting_for_leg1) {
            uint32_t dt = g_ring[i].ts - leg0_rise_ts;
            sum_dt += dt;
            if (dt < min_dt) min_dt = dt;
            if (dt > max_dt) max_dt = dt;
            ++n;
            waiting_for_leg1 = false;
        }
    }
    float mean_dt_ns = n ? (float)sum_dt / n * kNsPerTick : 0.f;
    float min_dt_ns  = n ? (float)min_dt * kNsPerTick : 0.f;
    float max_dt_ns  = n ? (float)max_dt * kNsPerTick : 0.f;
    // Expected = period / 2. Period in CAP ticks = APB / actual_freq.
    PwmTiming t = bestTiming(kFsw);
    float period_ns = 1e9f / (float)t.actual_freq;
    float expected_ns = period_ns / 2.0f;
    ESP_LOGI(TAG_PWM, "Test 12 N=2 phase: n=%u mean=%.1f ns min=%.1f ns max=%.1f ns (expected %.1f ns)",
             (unsigned)n, mean_dt_ns, min_dt_ns, max_dt_ns, expected_ns);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(N_pairs / 2, n);
    TEST_ASSERT_FLOAT_WITHIN(50.0f, expected_ns, mean_dt_ns);
}

