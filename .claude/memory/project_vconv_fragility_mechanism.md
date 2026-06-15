---
name: project_vconv_fragility_mechanism
description: "vconv MPPT timing-fragility is notification-coalescing under RT overrun, NOT a wall-clock dt"
metadata: 
  node_type: memory
  type: project
  originSessionId: 01c4c2d0-30f2-4d9b-bc17-62ce6a23fc3a
---

**FIXED as of 285cb181 (re-verified 2026-05-30 on 139C).** vconv now reliably converges to MPP and holds it — measured 814-815W at Vin~64.6V, advancing to slow-P&O, stable across a 35s trajectory. vconv is **now OK for validating control-behavior changes too** (still confirm anything risky on fry/flat). History below kept for the mechanism.

vconv (`CONFIG_FUGU_WITH_VCONV` + `config/lab/vconv_mock`) co-simulates a PV+buck plant on-device. Its MPPT loop *used to be* **timing-fragile**: sub-µs perturbations (an extra rtcount label, different instruction schedule) flipped it between converging to MPP (~790-815W) and parking near Voc at ~60W with duty pinned high. Deterministic per binary (noise=0, fixed RNG seed 0x9E3779B9).

**The mechanism was NOT "dt depends on how long the control code took."** dt is FIXED: `dt = 1/adcFreq` (`src/adc/vconv.h`); each tick advances a fixed number of switching cycles. No `micros()`/`esp_timer` in the path — that was the red herring.

**Real mechanism: notification coalescing under RT overrun.** `hasData()` waits on `ulTaskNotifyTake(pdTRUE, …)` (clear-on-exit binary). If the control chain overran one ADC tick, multiple timer-ISR notifications collapsed into one wakeup but the plant still stepped **once** by fixed dt — so plant-time fell behind controller-time by a jitter-dependent amount. P&O on the steep PV knee (α≈18.9) is bistable, so the basin it landed in flipped. **The fix:** `hasData()` now steps the plant by the coalesced tick count (`stepSeconds(dt * ticks)`, capped at kMaxCatchupTicks=8), so plant-time tracks samples-consumed regardless of timing.

Profiling lesson: rtcount means are inflated by ADC-ISR preemption (ISR + timed section share core 1); the `min` column is the truer pure-compute figure.

Sim integration math itself is sound (HS/LS slopes, body-diode signs, backward-Euler Vout, units clean). Fixed a stale conf header comment (was "8A/40V/MPP~200W", actual isc=13/voc=76/k=0.85 → ~790W). Related: [[project_fry_flat_may29_crash_rootcause]].
