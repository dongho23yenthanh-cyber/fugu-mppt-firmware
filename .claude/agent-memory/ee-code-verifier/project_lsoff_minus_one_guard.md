---
name: lsoff-minus-one-guard
description: pwmRect ceiling must be pwmMax − pwmCtrl − 1 (with MCPWM, also accounts for dtTicks already shaved from pwmMax); cmpLS==period silently keeps LS HIGH across the wrap
metadata:
  type: project
---

`src/buck.h:167, 303, 305, 328` all clamp `pwmRect ≤ pwmMax − pwmCtrl − 1`. The `−1` is load-bearing: if `cmpLS == period_ticks` the turn-off comparator event lands on the timer wrap and never fires — LS stays HIGH through the next HS-on edge → shoot-through.

Under the MCPWM driver (`src/pwm/mcpwm.h:138`), `pwmMax` is already `period_ticks − dtTicks` when dead-time is non-zero. So the absolute hardware ceiling is `period_ticks − dtTicks − 1`; the existing buck.h clamps satisfy this automatically because they use the shaved `pwmMax`. Do not "simplify" `pwmRect ≤ pwmMax − pwmCtrl` by dropping the `−1`.

**Why:** silent shoot-through on the LS→HS wrap kills both FETs and the bootstrap supply.

**How to apply:** when reviewing changes to `buck.h::pwmPerturb`, `computePwmRectMax`, `setManualRect`, or any new `setLsOff` call site, verify the `−1` is preserved. Same applies to any MCPWM doc or sketch — the resource-mapping table in `doc/mcpwm-sync-buck-driver.md:42` omits the `−1` and should be flagged.

Related: [[mcpwm-dt-one-submodule]]
