---
name: adc-nosample-watchdog-deadlock
description: ADC_ESP32_Cont no-sample watchdog (isGood) deadlocked the continuous DMA — gating read() on the very flag read() clears
metadata: 
  node_type: memory
  type: project
  originSessionId: 16974906-5610-4ffb-b265-5a3c53230b3a
---

Commit `81c9c10` ("adc/esp32-cont: no-sample watchdog") regressed fry's internal continuous ADC
(vin/ntc, esp32adc1) to **0 samples** → MPPT halted. Bisect: `1ad5cf5` (good) → HEAD (bad);
`81c9c10^` (f585633) sampled fine (vin=74V).

**The deadlock:** `ADC_Sampler::_updateAdc` (sampling.h) did `if (!adc->isGood()) return AdcError;`
*before* calling `adc->read(cb)`. For the StreamedCallback (continuous-DMA) backend, `read()` is the
ONLY place that drains the DMA AND refreshes `lastDataUs_` (the watchdog timestamp). So once a single
transient >1 s gap tripped `isGood()` (the chaotic WiFi-reconnect-crash storm gave it one), `read()`
was gated off forever → the watchdog could never clear → permanently "dead." `resetPeripherals`
(adc-restart) couldn't reliably break out either.

**Fix:** in `_updateAdc`, drain `read()` BEFORE the `isGood()` check for StreamedCallback (a live DMA
self-clears), then report `AdcError` after — independent of `hd` so the safety halt is preserved.
Verified on fry: vin/ntc sample, calibration completes, MPPT runs, 0 steady-state ADC errors.

**Lesson:** a no-data watchdog must never gate the operation that clears it. Drain first, judge after.

Residual (minor): ~15 s boot-transient `ADC error` spam + one `stopAndBackoff(16)` before the first
conv-done — the 1 s grace from `start()` is tight vs. RT-loop start + calibration. Recovers cleanly.

Related: [[esp32cont-read-throws-and-no-sample-watchdog]] (the review that prompted the watchdog),
[[ina226-timeout-loop-latency-shutdown]].
