---
name: gpio-isr-install-nested-ipc-deadlock
description: gpio_install_isr_service() must NOT be called inside esp_ipc_call_blocking() to the same core — nested IPC deadlocks boot
metadata: 
  node_type: memory
  type: project
  originSessionId: ff43a803-7bd9-4cd7-aba7-0d4f2bccb896
---

`gpio_install_isr_service()` internally does its own `esp_ipc_call_blocking()` to the calling core
(to `esp_intr_alloc` the GPIO dispatcher on that core). So wrapping it in
`esp_ipc_call_blocking(RT_CORE, ...)` to pin the alert ISR to RT_CORE makes RT_CORE's single `ipc1`
worker wait on itself → **permanent deadlock in setup()**, before `loopRT` exists.

**Why:** Diagnosed 2026-05-28. The uncommitted `pinGpioIsrToRtCore()` change (pinning the
INA226/ADS alert GPIO ISR to RT_CORE per [[project_ina226_timeout_loop_latency_shutdown]]) bricked
fry & flat after OTA. JTAG/GDB backtrace on the bench: `loopTask` blocked in `esp_ipc_call_blocking`
→ `ipc1` blocked inside `gpio_install_isr_service` → `gpio_isr_register` → nested
`esp_ipc_call_blocking(cpu_id=1)`. Hang point = `main.cpp` `pinGpioIsrToRtCore()`, right after the
`Failed to init LCD` log line.

**How to apply:** To run `gpio_install_isr_service()` on a specific core, spawn a *plain task pinned
to that core* (NOT the ipc worker) and join on it — the ipc worker stays free so the internal nested
IPC completes. Same caution for any IDF call that itself uses cross-core IPC.

**Second bug (confirmed on fry 2026-05-28): do NOT pass `ESP_INTR_FLAG_IRAM`.** `attachInterrupt()`
registers arduino-esp32's `__onPinInterrupt` dispatcher, which lives in flash. An IRAM-installed
service keeps firing while the flash cache is disabled (any flash write — coulomb/stats persist,
config save, OTA) and then jumps into that cached dispatcher → `Cache disabled but cached memory
region accessed` panic (backtrace: `__onPinInterrupt` ← `gpio_intr_service` ← flash op via
`spi_flash_op_block_func`/`ipc_task`). The mock-ADC bench never hits this (no `attachInterrupt`).
Install with flags `0`; the alert is just masked for the brief cache-off window. Core affinity comes
from the installing task, not the flag — so RT_CORE pinning still holds. Both bugs lived in the same
uncommitted change; fix committed after the deadlock fix (97f47a8).
