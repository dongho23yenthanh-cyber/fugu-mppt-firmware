---
name: pwm-internal-loopback
description: ESP32-S3 GPIO matrix + MCPWM io_loop_back flag let PWM gate outputs be self-observed with zero hardware jumpers; PWM test spec is zero-fixture
metadata:
  type: project
---

ESP32-S3 GPIO matrix allows any GPIO to simultaneously drive a peripheral output AND feed its own input back to another peripheral. Three independent confirmations from the IDF 5.5 source tree:

1. **`mcpwm_capture_channel_config_t::flags.io_loop_back = 1`** (`driver/mcpwm_cap.h:152`) — the IDF header itself documents: *"For debug/test, the signal output from the GPIO will be fed to the input path as well"*. Same-pin self-capture, no external wire.

2. **Capture timer is APB-clocked only** on S3 (`MCPWM_CAPTURE_CLK_SRC_APB`, `clk_tree_defs.h:292`) → 80 MHz / 12.5 ns per tick. 3 capture channels (`PWM0_CAP0..2_IN_IDX = 166..168`) per group, freely routable.

3. **Cross-peripheral loopback (LEDC → MCPWM_CAP):** any input signal (capture, PCNT, GPIO interrupt) can be routed from any pin via `esp_rom_gpio_connect_in_signal(pin, SIG_IN_*_IDX, false)`. So an LEDC-driven pin can be captured by MCPWM_CAP at 12.5 ns with no rewire.

**EE implication for PWM test spec:** the whole "loopback jumper" fixture from round 1 of `doc/pwm-test-spec.md` is unnecessary. Tests run on any board — bench or live — as long as a safety gate disables the half-bridge driver (SD pin / panel_en false) before the test pokes the gates. The same test suite covers LEDC and MCPWM drivers; capabilities are advertised per-driver (`has_hw_deadtime`, `has_hw_brake`, `is_complementary`, `has_glitch_free_update`) and tests gate on caps.

**Caveat:** internal loopback adds the GPIO matrix propagation delay (~5–10 ns systematic) to every captured timestamp, but that delay is *common-mode* to all edges on the same path, so dead-time / pulse-width measurements (which are differences) cancel it out. Only absolute-phase comparisons across different pins see the offset, and the prior ±30 ns tolerance already swallows it.

**How to apply:** when designing or reviewing tests for `src/pwm/*.h`, never spec a physical jumper. Use `io_loop_back = 1` for MCPWM-driven pins, or `esp_rom_gpio_connect_in_signal` for LEDC/other-driver pins. The driver-agnostic core invariants (freq, duty, D=0/D=1, no-glitch on duty step) are testable on any driver via the abstract `init_pwm` / `update_pwm` / `pwmMax` surface.

Related: [[mcpwm-cmp-update-on-tez]], [[mcpwm-dt-one-submodule]]
