---
name: mcpwm-cmp-update-on-tez
description: src/pwm/mcpwm.h comparator config now has update_cmp_on_tez=1 (confirmed 2026-05-25) — atomic TEZ-buffered writes match the doc and buck.h collapsed update path
metadata:
  type: project
---

`src/pwm/mcpwm.h:97` creates both comparators with `flags = {.update_cmp_on_tez = 1, .update_cmp_on_tep = 0, .update_cmp_on_sync = 0}`. Writes to `mcpwm_comparator_set_compare_value()` are double-buffered and commit atomically at the next TEZ.

**Why this matters:** `src/buck.h`'s combined-duty-update path (`setHsOff(new_cmpHS); setLsOff(new_cmpLS);` in either order) is now safe — the prior LEDC-style "largerDecrease first" ordering dance is unnecessary. Worst-case update latency = one PWM period (~26 µs at 39 kHz), well within RT-loop budget. Doc and code now agree.

**How to apply:** when reviewing the MCPWM driver or writing tests, treat comparator writes as atomic-at-TEZ. A glitch-free duty-step test should observe only commanded T_high values (D1, D2, D3 ...) with no intermediate values — that is the empirical signature of TEZ buffering, and is worth keeping in any PWM driver test suite as a regression guard against `update_cmp_on_tez` accidentally being cleared.

Related: [[mcpwm-dt-one-submodule]], [[lsoff-minus-one-guard]]
