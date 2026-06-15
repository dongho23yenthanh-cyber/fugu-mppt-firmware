---
name: project_flat_dawn_floor_stuck_after_reboot
description: flat stuck at duty=1/CV at dawn after a night reboot; restart recovers; NOT the mppt ns/clamp changes
metadata: 
  node_type: memory
  type: project
  originSessionId: 7c3ae65c-d2a4-4d52-a650-a134872a4247
---

2026-06-09 morning: flat stuck at **duty=1 (pwmCtrlMin), st=CV, ~0.5W** under full sun
(Vin=66 > Vout=25, battery low vcHigh 3.19, charger wanting Vout_max=29 @ 39A). It refused to
climb. `sweep` and `mppt` did NOT recover it (sweep is skipped while stuck; `mppt` says "already
enabled"). **`restart` recovered it** — boot sweep ran in daylight, found a real MPP, resumed MPPT
(~50W, outproducing fry).

Root cause (confirmed from the dawn log, NOT the controller theories I first guessed) = a **bogus
dawn boot-sweep in marginal light**, exposed by the night OTA reboot (flat cold-started into dawn;
fry ran continuously and never did a from-cold dawn sweep). Trajectory:
- 06:02 dawn: from-cold start sweep ran **428 s** in rising marginal light and captured a garbage MPP
  `MPP=(1.3W, 4010, 25.1V)` — duty 4010 ≈ pwmCtrlMax. With ~0 light there is no real power peak, so
  "max power seen" lands at near-full duty. It set target=4010 and locked there at ~1.3 W.
- As the sun climbed (Vin 25→66 V over the morning), 98% duty became wildly wrong (huge current at
  Vin 66/Vout 25); the I/V/P controllers slammed duty DOWN, overshooting to the floor (pwmCtrlMin=1).
- Stuck at duty=1, st=CV idx=1, I≈0. It self-rebooted once ~06:09 (likely watchdog). `sweep`/`mppt`
  couldn't recover (sweep skipped while it holds a target/limit). `restart` in FULL sun ran a clean
  10 s sweep, found the real MPP (1270, 44 W), recovered.

NOT caused by the 2026-06-08 mppt changes (commits 47be188 clamp + 8c0e4a1 ns-gate): the ns-gate
lives only in `updateCV()`, and flat has **no tracker.conf** → `target_duty_cycle=0` → `targetPwmCnt=0`
→ `update()` never calls `updateCV()` (mppt.cpp:14), so it's dormant. The clamp only raised the
sweep/step *upper* cap (16→32.8) and doesn't change WHICH MPP is captured. Clamp is now validated
live (flat tracking fine on fry-brk1-97-g8c0e4a1f).

Real fix: don't capture/commit an MPP when power is negligible (gate sweep MPP-capture on a min
power, or refuse to auto-sweep until power clears a floor). Operational: don't OTA-reboot a converter
near dusk — it cold-starts into a marginal-light dawn sweep. See
[[project_rect_offset_and_intrinsic_oscillation]] ("apply may strand at 0W -> sweep").
