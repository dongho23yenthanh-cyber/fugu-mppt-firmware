---
name: project_fry_adc_hw_healthy_in_isolation
description: "On-target ADC HW self-test (test_adc_hw.cpp) shows fry's ADCs + interrupts are healthy in isolation — stalls are integration/timing, not dead hardware"
metadata: 
  node_type: memory
  type: project
  originSessionId: a52bd621-2a1d-4f9a-8d5e-d8cf3b7c1132
---

`test/test_adc_hw.cpp` (build `RUN_TESTS=1 TEST_ADC_HW=1 idf.py -B build-tests build`) is a minimal on-target ADC liveness test, run ONE BY ONE with per-step ESP_LOGI markers, gated so it skips the PWM/MCPWM tests and forces gate pins 21/14 low at boot → **safe to flash onto a live converter**. It re-runs from `loop()` so a console attached anytime catches a fresh cycle. Hardcodes fry params (internal ADC ch3=vin/ch7=ntc; INA226 0x40 on I2C0 sda42/scl2 @800k, alert pin41).

**Result (2026-05-29, fry on USB /dev/cu.usbmodem1201):** everything PASSES in isolation — internal continuous ADC 923 samples/2s @458sps isGood=1 (DMA conv-done ISR firing); INA226 ACKs, MFR=0x5449/DIE=0x2260, 512 conversions/s, 0 i2cErrors, alert pin idles high, **conversion-ready ALERT interrupt fires 1025×/2s (isrCore=0)**. Board did NOT stall.

**Conclusion:** fry's ADC hardware + both interrupt paths are sound. The production `40000 timeout!` / `no ADC samples for 1000ms` / `0sps` stalls are an **integration/timing/contention** problem, NOT dead silicon. Prime suspects: INA226 **triggered single-shot** (trigger→wait-for-alert, 40ms timeout) inside the RT loop on core 1 while WiFi+converter run, plus the alert GPIO ISR landing on core 0 (the `gpio: ... already installed` line = `pinGpioIsrToRtCore()` race). Refines [[project_ina226_timeout_loop_latency_shutdown]].

**Deploy mechanics:** flash app-only (esptool `write_flash 0x10000 build-tests/fugu-firmware.bin`) to preserve littlefs config + bootloader; reset boots it. Capture with `fugu_console.py` REPL via a pty (`script -q /dev/null ... < <(sleep N)`) — see [[feedback_never_idf_monitor]]. fry still needs production firmware reflashed to return to converter duty.
