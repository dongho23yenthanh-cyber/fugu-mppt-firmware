---
name: ""
metadata: 
  node_type: memory
  originSessionId: 93b09df2-2b18-4541-8739-662839485c81
---

`E (...) main: ADC error`, `W (...) adc_esp32: no ADC samples for N ms`, and
`W (...) mppt: backoff ... [stopAndBackoff]` in a device's log mean the device is **NOT healthy** —
even if it's still responding and sensor sample-counts are increasing.

**Why:** the no-sample watchdog self-heals (`resetPeripherals: restarting continuous ADC`), so the
continuous ADC recovers and `num=`/`sps` keep climbing *after each stall*. So a `sensor` read showing
vin/ntc counts going up can look fine while the ADC is actually stalling >1 s, throwing `ADC error`,
and backing off the converter for 5 s repeatedly. I declared fry "healthy" on `main` purely from
"vin num climbing + hostname + rssi" and missed the recurring `main: ADC error` + backoffs. Climbing
counts are necessary but NOT sufficient.

**How to apply:** after flashing/restoring any converter (esp. fry/flat), capture the **log stream**
(serial via etc/fugu_console.py, or `ssh havan.local tail pv/fugu_console.log`) for ~30 s and grep for
`ADC error`, `no ADC samples`, `backoff`, `stopAndBackoff`, `Loop latency`. Watch the status line over
time: healthy = leaves `st=START`, `sps` stable, converter actually tracking, and **zero** ADC errors.
Only then call it healthy. Relates to [[adc-nosample-watchdog-deadlock]] (the fix stops the *permanent*
latch but the underlying DMA stall can still trip the watchdog) and
[[ina226-timeout-loop-latency-shutdown]].
