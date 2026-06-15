---
name: bootlog-hook-wifi-stack-overflow
description: "Hooking esp_log->vprintf_mux early in setup() overflows the 3072B wifi-task stack during WiFi connect (\"stack overflow in task wifi\"); keep enable_esp_log_to_telnet LATE"
metadata: 
  node_type: memory
  type: project
  originSessionId: ff43a803-7bd9-4cd7-aba7-0d4f2bccb896
---

Routing `esp_log` through `vprintf_mux` (via `esp_log_set_vprintf(&vprintf_)` in
`enable_esp_log_to_telnet`) **from the start of `setup()`** bricks any device that connects to WiFi:
the `wifi` task (stack **3072 B**) emits a heavy logging burst during connect, and `vprintf_mux` uses
a **300 B `loc_buf` stack buffer** + callback frames — that blows the wifi task stack →
`***ERROR*** A stack overflow in task wifi has been detected.` → reboot loop, hung in `setup()`
before any service (telnet/MQTT/BLE) starts → no remote recovery, serial reflash only.

**Why:** Diagnosed 2026-05-28 via flat's serial console after an OTA bricked it. The boot-log backlog
([[project_no_ota_rollback_no_boot_watchdog]]) wanted to capture the `setup()` body, so the hook was
moved early — that's what overflowed the wifi task. The mock-ADC bench never connects to a real AP,
so it booted clean and hid the bug; fry on the older build (late hook) was fine.

**How to apply:** keep `enable_esp_log_to_telnet()` **late** (after `registerServices()`, the proven
ordering) — the WiFi connect burst then uses the light default vprintf, not `vprintf_mux`. The
boot-log backlog consequently captures only from there on (post-setup), not the `setup()` body —
accept that.

**Late-hook is NOT a full fix — post-setup reconnects still overflow (2026-05-29).** Once the hook
is active (after setup), ANY later WiFi reconnect logs on the wifi task via vprintf_mux → same 3072B
overflow → reboot. Seen on a wiped esp32s3 bench unit whose boot WPA 4-way handshake didn't complete
first try (`Reason 15 4WAY_HANDSHAKE_TIMEOUT`): the post-setup retry storm reboot-looped it until a
clean boot-handshake finally stuck. This is very likely the mechanism behind the chronic fry/flat
reboot-on-AP-loss ([[project_stick_wifi_reboot_preempts]]). Correct password — the overflow, not the
PSK, was breaking the connect.

**There is NO `CONFIG_ESP_WIFI_TASK_STACK_SIZE` Kconfig symbol in IDF 5.5** (esp_wifi/Kconfig has no
wifi-task-stack option; the 3072B is internal/`WIFI_INIT_CONFIG`).

**FIXED 2026-05-29 (commit 6f02574, logging.cpp).** `vprintf_()` now checks `pcTaskGetName(nullptr)`
(guarded by `xPortCanYield()` for ISR safety) and routes the `"wifi"` task to the light default
`old_vprintf` (UART only), bypassing `vprintf_mux`. Verified on an esp32s3 bench unit: 3 wifi off/on
cycles + a forced reconnect burst (`init->auth->assoc->run`) with NO overflow, uptime monotonic,
reconnected to $LAB_WIFI_SSID. The late-hook ordering in main.cpp:454 is now belt-and-suspenders; the
hook could move early again if setup-body capture is wanted (not changed).

**Meta-lesson:** do NOT OTA the bleeding-edge build (untested on real converter HW + WiFi) to a live
converter. fry runs the proven `gba7db13`; flat got `g909f684` (with this early hook) and bricked.
For converter OTAs use the build a sibling already runs, dry-run first, and confirm the after-table.
