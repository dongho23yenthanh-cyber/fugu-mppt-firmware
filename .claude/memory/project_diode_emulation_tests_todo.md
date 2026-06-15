---
name: project_diode_emulation_tests_todo
description: TODO write unit tests for diode-emulation edge cases reviewed/fixed 2026-05-21
metadata: 
  node_type: memory
  type: project
  originSessionId: 13f7d181-c345-4f24-90cc-5e985734bc1b
---

After the 2026-05-21 diode-emulation review of `src/buck.h`, we still owe unit tests for the edge cases.

**Why:** the fixes were applied without test coverage; these are sensor-noise / boundary conditions that are hard to hit on hardware but easy to assert in a unit test.

**How to apply:** add to `test/` (Unity, on-target or host-stub). Cover at least:
- **convRatioWCE clamp (the fix):** buck at M≈0.99 and the `vout>=vin` fallback (and boost equivalents) must NOT make `rectCtrlRatio()`/`pwmRectRatioDCM` negative. Assert `pwmRectRatioDCM >= 0` and that DCM `pwmRectMax` does not wrap to the full CCM complement.
- **DCM/CCM decision + hysteresis:** `computeDCM` band [1.8, 2.0]·il, plus the `il < 0.1` force-DCM path.
- **rectCtrlRatio** matches doc eqs: buck `1/M-1`, boost `1/(M-1)`.
- **boot_refresh_ns → pwmRectMin count conversion:** `pwmRectMin == ceil(ns·1e-9·fsw·pwmMax)`; default 1500 ns at 39 kHz must reproduce the old ~6% behaviour; boost gives `pwmRectMin == 0`.

Status: #1 convRatioWCE clamp DONE; cheap part of #2 DONE (`+N`/`-N` routed through `setTargetDutyCycle`, RT core sole PWM writer); `boot_refresh_ns` conf DONE (was hardcoded `MinDutyCycleLS=0.06`); magic constants named DONE.

Tests WRITTEN (in `test/test_buck.cpp`, registered in `test/main.cpp`): ratio clamp below/at unity, never-negative sweep, DCM rectMax not full-CCM near unity, sync-rect-off thresholds, sync-rect active in DCM, boot_refresh_ns→pwmRectMin (default/scaled/boost=0), boost ratio clamp. Compile-verified via `RUN_TESTS=1 idf.py build`. NOT yet run on-target — needs a bench device ($ESPPORT); the suite actuates LEDC/gate pins so use a safe board, not a live charger.

Still-open review findings (not yet fixed, may want tests too): full single-writer for #2 (`setManualRect`/`enableSyncRect` still actuate PWM from core 0 in manual mode), and the throw in `pwmPerturbFractional` on the RT path. See [[project_perf_h_noninline_odr]] for the single-TU header constraint when adding test files.
