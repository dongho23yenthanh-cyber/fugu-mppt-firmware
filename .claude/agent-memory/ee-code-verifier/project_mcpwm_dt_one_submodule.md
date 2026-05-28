---
name: mcpwm-dt-one-submodule
description: ESP32-S3 MCPWM operator has ONE shared dead-time submodule — apply posedge delay only to LS gen; reserve LS→HS wrap dead-band by shaving dtTicks off pwmMax
metadata:
  type: project
---

ESP32-S3 MCPWM: each operator has exactly one dead-time submodule shared by its two generators. Cannot independently configure posedge delays on genHS and genLS via two `mcpwm_generator_set_dead_time(...)` calls — the second call reconfigures the same submodule.

Correct pattern (already implemented in `src/pwm/mcpwm.h:127-139`):
1. Apply `posedge_delay_ticks=dtTicks` to LS gen only (covers HS→LS transition).
2. Reserve the LS→HS wrap dead-band in software by reducing exposed `pwmMax` to `period_ticks − dtTicks`.
3. SW clamps then keep `lsOff ≤ pwmMax − 1` which is `≤ period_ticks − dtTicks − 1`.

**Why:** delaying both rising edges does NOT prevent HS→LS shoot-through — at cmpHS, genHS goes LOW and genLS goes HIGH on the same tick; only the LS rising edge needs delaying. Real shoot-through here kills both FETs and the bootstrap supply.

**How to apply:** when reviewing/writing MCPWM driver code, verify (a) only one DT submodule call per operator, (b) `pwmMax` is shaved by `dtTicks` after init, (c) `lsOff` clamp uses the shaved `pwmMax`. The doc `doc/mcpwm-sync-buck-driver.md:102-104` currently states the wrong pattern (two DT calls) — flag and fix any code that copies from the doc.

Related: [[mcpwm-cmp-update-on-tez]], [[mcpwm-action-end-macro-trap]] (project memory in MEMORY.md).
