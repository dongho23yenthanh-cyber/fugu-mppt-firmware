---
name: project_charger_bms_same_core_lockfree
description: charger BMS producer/consumer are both core 0; portMUX locks replaced with lock-free atomics
metadata: 
  node_type: memory
  type: project
  originSessionId: 5215c8ee-5a83-4922-874c-f32e04e2ea63
---

The charger's BMS data flow is **same-core, two-task** — not cross-core. Producer (`updateBatCurrent`/`setVcellHigh` from the esp-mqtt event task) is pinned to core 0 via `CONFIG_MQTT_USE_CORE_0=y`; consumer (`charger.update()` → `_updateTermination` from `loopLF`, called only by `loopNetwork_task` which `assert(xPortGetCoreID()==0)`) is also core 0. So the only hazard is task preemption, not true SMP concurrency.

**Why:** the original `MeanAccumulatorSync` (ibat_mean) and `CoulombCounter` used `portMUX_TYPE` — an SMP spinlock that also disables interrupts — which is overkill for a single-core task race and was the path in a startup `LoadProhibited` crash decode (EXCVADDR 0xbc, portEXIT_CRITICAL).

**How to apply:** locks removed (2026-05). ibat is now smoothed producer-side (EWMA span 8 in `BatteryState`, private) and published as one `std::atomic<float> _ibatSmoothed` (NAN until 8 samples); consumer just loads it. `CoulombCounter` keeps the integrator strictly single-writer (producer), exposes Ah as `std::atomic<float>`, and `markFull()` sets a `std::atomic<bool>` reset flag honored on the next producer update — `ahSinceFull()` returns 0 while the flag is pending. `mean_accumulator_sync.h` was deleted. Pattern for any future BMS/MQTT→loop sharing: single-writer atomic snapshot, no portMUX. Deferred-reset has a ≤1 BMS-cycle lag, fine since termination is re-evaluated ~24 s (paced by iout_mean's MEAN_NUM gate). See [[project_recharge_after_full_periodic_sweep]], [[project_charger_shared_bms_and_flat_vout_cal]].
