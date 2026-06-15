---
name: no-ota-rollback-no-boot-watchdog
description: "A bad OTA image bricks permanently — rollback disabled, no watchdog covers setup(); fry & flat needed physical reflash"
metadata: 
  node_type: memory
  type: project
  originSessionId: ff43a803-7bd9-4cd7-aba7-0d4f2bccb896
---

A firmware image that hangs anywhere in `setup()` becomes a **permanent silent brick** — no
auto-revert, no watchdog reset. Confirmed 2026-05-28 when the GPIO-ISR deadlock
([[project_gpio_isr_install_nested_ipc_deadlock]]) was OTA'd to fry & flat: both printed "OTA
Succeed, Rebooting…" then went silent for hours.

**Why (three independent gaps):**
- **No rollback.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is not set and `src/` never calls
  `esp_ota_mark_app_valid_cancel_rollback()`. `doOta()` uses `esp_https_ota()`, which sets the boot
  partition; the new image boots as `ESP_OTA_IMG_UNDEFINED` = valid forever, no previous-slot fallback.
- **No watchdog covers setup().** TWDT watches only the idle tasks (`CHECK_IDLE_TASK_CPU0/1=y`, 5s),
  and a *yielding* block in setup() keeps idle alive → never barks. `CONFIG_ESP_TASK_WDT_PANIC` is
  off, so a bark wouldn't reset anyway. `esp_task_wdt_isr_user_handler()` (main.cpp) defers restart
  via `enqueue_task` to a core-0 task drained from `loop()` — which never runs until setup() returns.
  `loopRT` (the only RT task) is created only *after* the blocking calls. INT_WDT (300ms) doesn't
  fire on a yielding wait.
- setup() also blocks on `wait_for_wifi()` at boot (`setupNetworkAtBoot`), a long-standing pattern.

**RESOLVED 2026-05-28 (commits 97f47a8 + 9ed2078):** rollback enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`) + `esp_ota_mark_app_valid_cancel_rollback()` in
`lfMarkOtaValid` once RT loop is healthy (>20s uptime, sampler alive); one-shot esp_timer boot
watchdog (30s) over setup() in `main.cpp`. A future bad OTA now reboots and reverts instead of
bricking. **Note rollback needs the NEW bootloader flashed** (it's a bootloader feature) — an
app-only OTA/flash keeps the boot watchdog but not auto-revert.

**Recovery of the originally-bricked units:** physical/serial reflash only (bricked devices never
reach the network — no OTA recovery). They are wired to **havan** (`/dev/ttyACM0`, USB-Serial-JTAG);
esptool venv + firmware binaries staged at `havan:~/fugu_flash/`. Flash bootloader+parttable+ota_data+app,
**omit littlefs (0x3b9000) + nvs (0x9000)** to preserve each unit's real config. One unit (self-ID
hostname **fry**, MAC 34:85:18:99:85:a4, 16MB flash) recovered + verified (CV charging, stable on MQTT).
The second unit still needs wiring. fry/flat are LIVE converters — see [[project_fry_flat_nat_mapping_swapped]];
hostname/IP naming is tangled (user called the recovered one "flat" but it self-reports "fry").
