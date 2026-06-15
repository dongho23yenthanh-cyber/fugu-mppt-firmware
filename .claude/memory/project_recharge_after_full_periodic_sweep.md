---
name: project_recharge_after_full_periodic_sweep
description: "Root cause of \"charger recharges after battery full\" had two layers: periodic-sweep gate (fixed 2026-05-20) + pinning-tracks-vbat_avg (fixed 2026-05-24, charger.h terminated-float branch). PI-on-iBat experiment 2026-05-24 PM stashed (couldn't work due to protection trips at light-load DCM)."
metadata:
  node_type: memory
  type: project
  originSessionId: 6fb9ccbe-34cd-4888-86ca-bce04f58f2b5
---

Two distinct bugs caused the "charger recharges after full / pack discharges instead of resting" complaint on fry/flat (May 2026):

**Layer 1 (fixed 2026-05-20):** the periodic MPPT re-sweep at `src/mppt.cpp:65` fired every 30 min regardless of charge state. Sweep ramped duty 0→max into a full pack. Fixed by gating with `bool batteryFull = bool(charger.termCond) || ctrlState.mode == MpptControlMode::CV;`.

**Layer 2 (fixed 2026-05-24, commit 08db36d):** even with the sweep gated, the pack still discharged when terminated. Root cause was `_updatePackVoltagePinning()` in charger.h: while terminated, the pinning formula `vpack_pin = vbat_avg − (vcell_high − v_eoc)·2` *tracked* the pack voltage downward via the 60-sample EWMA. As any load drained the pack, vbat_avg fell, vpack_pin fell, the converter saw no headroom, stayed off, pack kept draining.

**Layer 2 fix (current state):** added a `terminated && batDataOk` branch that holds `vpack_pin = params.Vbat_fallback` (an absolute target = N_cells × cv_min ≈ 26.96 V for 8s LFP), bypassing the EWMA and cell-voltage feedback. 5 s `_floatGlide` on both rising and falling edges of termination prevents stepping the converter setpoint. Verified in production morning of 2026-05-24: pack ibat went from −2 A discharge (pre-fix) to ~0 A at rest (post-fix).

**Failed follow-up: PI-on-iBat → ioutLim (2026-05-24 PM, stashed in `stash@{0}` WIP-PI-experiment-2026-05-24).** Idea: regulate IoutCTRL via a slow PI on BMS-reported iBat to make the float loop calibration-immune (flat reads Vout ~0.16 V low vs BMS; absolute-target fix puts asymmetric load on flat). Implementation in charger.h: dropped `_floatGlide`, added `_termIoutPI` integrator (Ki=0.02 A/(A·s), 0.3 A deadband, 0.5 A min floor to keep IoutCTRL well-defined under normalize=true), gated PI on `vcell_high >= cv_min` (decoupled from termCond.terminated for boot/OTA case). Reasoning was sound — PI is what you'd want for "regulate iBat to 0" + calibration immunity.

Why it failed in the field:
1. **Boot OTA cycle drained pack below `cv_min`** during the experiment. Once cells < 3.37 V, the `inFloat` gate flipped → PI disengaged → vpack_pin returned to `Vbat_max` → MPPT ran free in bulk mode.
2. **Bulk-mode MPPT sweep tripped pre-existing protection** at `src/mppt.h:522`: `iOutSmall && (D*0.8) > vr` → "Buck D=45% but Vout(26.71,vr=0.36) Iout(-0.01) low! sensor/HB fail". With sun marginal and pack right at the converter's natural operating point, MPPT can't establish stable duty; protection trips, converter restarts, repeats every ~5 s.
3. Net result: both devices stuck in protection-trip loops while pack drained at -2.5 A. Reverted to the absolute-Vbat_fallback fix and re-OTA'd both.

Cost of experiment: ~0.27 % SOC (90.27 → 90.00). Recoverable on next sun.

**Lessons for next iteration:**
- The Vout-vs-iBat dichotomy is a false split. Any control approach has to coexist with the existing `iOutSmall + D > vr*1.25` protection, which is highly sensitive to the panel I-V curve / DCM operating point. The pre-PI Vbat_fallback fix avoids tripping it only because VoutCTRL clamps duty before the protection threshold.
- If you want calibration-immune float regulation, you can't just swap Vout regulation for Iout regulation — you need either (a) to soften the protection during float (it's there for sensor/HB fault detection, not actual feedback), or (b) to keep a VoutCTRL ceiling tight enough that duty doesn't reach the protection band. Option (b) is what the current fix already does.
- The protection trips were also pre-existing in the Layer-2 fix when pack is below cv_min — they just didn't manifest because the user never let the pack get that low for that long.
- Two-converter calibration drift remains an open problem ([[project_charger_shared_bms_and_flat_vout_cal]]). Recalibration via `set-config sensor.conf vout_rl` is still the right path; PI is not.

**Also done in earlier session:**
- VFLOOR release path in `Li_ChgTerminationCondition::shouldRelease` now requires 4 consecutive sub-threshold BMS frames (was 1) — kills transient I·R sag false releases (e.g. 50 A load spike).
- DoD release path now logs (was silent).
- Tried gating the auto-restart sweep at `main.cpp:670` on `!termCond` — **reverted same session**. The gate caused flat to stay disabled forever after every INA226 latency trip (chronic). Don't reintroduce.

**Remaining concerns (not blocking):**
- EOC taper formula at cvHigh≈cv_min boundary gives `vpack_pin = vbat_avg` with no headroom → converter has slow start when pack reboots sitting right at cv_min.
- flat reads Vout ~0.16 V low vs BMS sum, so under the current float behaviour flat carries most of the load and fry coasts. Independent issue — see [[project_charger_shared_bms_and_flat_vout_cal]].
- Termination state (terminated, ahSinceFull) still volatile across reboot. Deferred.

Related: [[project_charger_shared_bms_and_flat_vout_cal]], [[project_ina226_timeout_loop_latency_shutdown]].
