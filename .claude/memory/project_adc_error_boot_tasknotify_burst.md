---
name: project_adc_error_boot_tasknotify_burst
description: "\"E (xxxx) main: ADC error\" at boot (fry-brk1 regression) root cause = TaskNotification::wait() ==1 starved by notify burst during delay(1000); fixed pdTRUE/!=0"
metadata: 
  node_type: memory
  type: project
  originSessionId: d061d734-5dc8-41b0-bb19-f40d18973f83
---

The fry-brk1 regression — all devices `E (....) main: ADC error` shortly after boot (bench ~1.9s, fry ~9.3s) then a backoff — root cause is **`TaskNotification::wait()` in `src/etc/rt.h`**, not the ADC code itself.

Chain: `loopRT` calls `adcSampler.begin()`→`start()` (sets the no-sample watchdog's `lastDataUs_`), then hits `delay(1000)` at `main.cpp` (active because `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` — boot log warns of it). During that 1s the loop doesn't drain `read()`, so the continuous-ADC conv-done ISR piles up ~thousands of task notifications. The old `wait()` did `ulTaskNotifyTake(pdFALSE,…) == 1` (decrement-by-one, true only when value is *exactly 1*) → returned false with a burst pending → `hasData()` false → `read()` never called → `lastDataUs_` stays at start-time → the new no-sample watchdog ([[project_adc_nosample_watchdog_deadlock]], 81c9c10/fry-brk1) trips on the first `isGood()`.

Confirmed on bench (/dev/cu.usbmodem1301) with DBG instrumentation: at the trip `reads=0 hits=0 stale=1103ms` (read() literally never ran). The watchdog merely *exposed* a pre-existing latent bug; pre-watchdog (fry-adcOk-wifiSOF) the same burst just cost a few spin iterations, harmlessly.

Fix (one line): `return ulTaskNotifyTake(pdTRUE, …) != 0;` — clear-on-exit binary semaphore, any pending count = one wakeup (matches rt.h's own cited FreeRTOS "as-binary-semaphore" reference). Affects all ADCs (esp32-cont, ina226, ads, mock) — strictly more robust (old `==1` also dropped a sample whenever ≥2 frames queued between iterations). Verified: 3 clean reboots, no ADC error/backoff, sampling live ~60k sps.

DBG lesson reconfirmed: `%lld` in ESP_LOG corrupts args under newlib-nano ([[project_newlib_nano_format]]) — first instrumentation pass printed garbage (`reads=0 hits=1103` impossible); redo with 32-bit `%ld`/`%lu` casts.
