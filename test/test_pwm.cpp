// Rig self-test for the MCPWM-CAP measurement plumbing (doc/pwm-test-spec1.md).
// Uses the production LEDC driver as a known-good reference: matrix-route its
// output back into MCPWM capture on the same pin and verify measured freq +
// pulse-width match commanded LEDC values within the tolerance spec1 claims.
// If these fail, every downstream MCPWM test is suspect.
//
// Pin: GPIO 4 by default — override with -DPWM_TEST_PIN=<n> if it conflicts
// with the target board. No external wiring; INPUT_OUTPUT mode + internal
// GPIO-matrix routing handles the loopback.

#include <unity.h>
#include <Arduino.h>
#include <esp_timer.h>
#include <driver/mcpwm_cap.h>
#include <driver/gpio.h>

#include "pwm/ledc.h"

#ifndef PWM_TEST_PIN
#define PWM_TEST_PIN 4
#endif

static constexpr int       kTestPin = PWM_TEST_PIN;
static constexpr uint32_t  kFsw     = 39000;
static constexpr uint32_t  kApbHz   = 80'000'000;  // MCPWM CAP clock on S3 + classic ESP32
static constexpr float     kNsPerTick = 1e9f / (float)kApbHz;  // 12.5 ns

struct CapEvent {
    uint32_t ts;    // APB ticks (12.5 ns/tick)
    uint8_t  pos;   // 1 = rising, 0 = falling
};

static constexpr size_t kRingSize = 4096;
static CapEvent          g_ring[kRingSize];
static volatile uint32_t g_head;

static bool IRAM_ATTR cap_isr(mcpwm_cap_channel_handle_t /*chan*/,
                              const mcpwm_capture_event_data_t *edata,
                              void * /*ctx*/) {
    uint32_t h = g_head;
    if (h < kRingSize) {
        g_ring[h].ts  = edata->cap_value;
        g_ring[h].pos = (edata->cap_edge == MCPWM_CAP_EDGE_POS) ? 1 : 0;
        g_head = h + 1;
    }
    return false;
}

namespace {

struct CapRig {
    PWM_ESP32_ledc ledc;
    mcpwm_cap_timer_handle_t   timer = nullptr;
    mcpwm_cap_channel_handle_t chan  = nullptr;
    bool started = false;

    void init(uint32_t fsw) {
        // 1. LEDC drives the pin. Sets mode = OUTPUT, routes LEDC_LS_SIG_OUT.
        ledc.init_pwm(0, kTestPin, fsw);
        ledc.update_pwm(0, 0);

        // 2. MCPWM capture timer + channel on the same pin. Driver writes
        //    mode = INPUT (clobbers OE), routes pin → PWM0_CAPn_IN.
        mcpwm_capture_timer_config_t tconf = {
            .group_id = 0,
            .clk_src  = MCPWM_CAPTURE_CLK_SRC_DEFAULT,  // APB on S3 / classic
        };
        ESP_ERROR_CHECK(mcpwm_new_capture_timer(&tconf, &timer));

        mcpwm_capture_channel_config_t cconf = {
            .gpio_num = kTestPin,
            .prescale = 1,
            .flags = {
                .pos_edge          = 1,
                .neg_edge          = 1,
                .pull_up           = 0,
                .pull_down         = 0,
                .invert_cap_signal = 0,
                .io_loop_back      = 0,  // 0: we restore OE manually below
                .keep_io_conf_at_exit = 0,
            },
        };
        ESP_ERROR_CHECK(mcpwm_new_capture_channel(timer, &cconf, &chan));

        // 3. Restore both IE and OE so LEDC keeps driving while CAP reads.
        //    Matrix routes from steps 1+2 are preserved by gpio_set_direction.
        ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)kTestPin, GPIO_MODE_INPUT_OUTPUT));

        // 4. Install ISR + arm.
        mcpwm_capture_event_callbacks_t cbs = { .on_cap = cap_isr };
        ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(chan, &cbs, nullptr));
        ESP_ERROR_CHECK(mcpwm_capture_channel_enable(chan));
        ESP_ERROR_CHECK(mcpwm_capture_timer_enable(timer));
        ESP_ERROR_CHECK(mcpwm_capture_timer_start(timer));
        started = true;
    }

    // Clear the ring atomically wrt the ISR by disabling the channel first.
    void rearm() {
        if (chan) ESP_ERROR_CHECK(mcpwm_capture_channel_disable(chan));
        g_head = 0;
        if (chan) ESP_ERROR_CHECK(mcpwm_capture_channel_enable(chan));
    }

    bool wait_events(uint32_t target, uint32_t timeout_ms) {
        int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        while (g_head < target) {
            if (esp_timer_get_time() > deadline) return false;
            vTaskDelay(1);
        }
        return true;
    }

    ~CapRig() {
        if (started) {
            mcpwm_capture_timer_stop(timer);
            mcpwm_capture_channel_disable(chan);
            mcpwm_capture_timer_disable(timer);
            mcpwm_del_capture_channel(chan);
            mcpwm_del_capture_timer(timer);
            ledc.stop(0, 0);
        }
    }
};

}  // namespace

// Rig-1. Frequency path. LEDC at 39 kHz / D=0.5; MCPWM_CAP timestamps 4000
// rising edges; freq = (N-1)*APB_HZ / (last-first). Must agree with the
// commanded freq within ±5 Hz.
void test_pwm_rig_freq_path() {
    CapRig rig;
    rig.init(kFsw);
    rig.ledc.update_pwm(0, rig.ledc.pwmMax / 2);  // D = 0.5
    vTaskDelay(2);                                // let LEDC commit
    rig.rearm();

    constexpr uint32_t N = 4000;
    TEST_ASSERT_TRUE_MESSAGE(rig.wait_events(N * 2 + 4, 250),
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
    TEST_ASSERT_FLOAT_WITHIN(5.0f, (float)kFsw, freq);
}

// Rig-2. Pulse-width path. LEDC at 39 kHz, sweep D ∈ {0.25, 0.5, 0.75};
// MCPWM_CAP both edges, mean(t_fall - t_rise) over 1024 cycles vs the
// commanded duty fraction × measured period. Tolerance ±25 ns.
void test_pwm_rig_pulsewidth_path() {
    static constexpr float kDuties[] = {0.25f, 0.5f, 0.75f};

    CapRig rig;
    rig.init(kFsw);

    for (float D : kDuties) {
        uint32_t cmp = (uint32_t)(D * (float)rig.ledc.pwmMax + 0.5f);
        rig.ledc.update_pwm(0, cmp);
        vTaskDelay(2);
        rig.rearm();

        constexpr uint32_t N = 1024;
        TEST_ASSERT_TRUE_MESSAGE(rig.wait_events(N * 2 + 4, 250),
                                 "CAP ring did not fill at requested duty");

        // Pair adjacent (rise, fall) → pulse width; (rise[k+1] − rise[k]) → period.
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

        // ±25 ns tolerance, expressed in duty: ±25ns / (1/fsw)
        float duty_tol = 25e-9f * (float)kFsw;
        char msg[96];
        snprintf(msg, sizeof msg, "D=%.2f cmp=%u measured=%.4f expected=%.4f",
                 D, (unsigned)cmp, measured_duty, expected_duty);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(duty_tol, expected_duty, measured_duty, msg);
    }
}
