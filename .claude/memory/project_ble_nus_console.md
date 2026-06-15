---
name: project_ble_nus_console
description: "BLE NUS serial console (BleConsoleService) implemented + fully verified on hardware incl. BLE client command round-trip via etc/ble_console.py"
metadata: 
  node_type: memory
  type: project
  originSessionId: 78fe9b3a-36a1-46b6-8e39-70760fbd6f40
---

A BLE (NimBLE) Nordic UART Service serial console is planned. Full plan lives in repo root `ble.md`.
WebBLE (Web Bluetooth from `etc/config-tool/conf-editor.html`) is a first-class client, so security
defaults to **Just Works** (not MITM passkey) — configurable `none|justworks|passkey`.

**Why:** wireless console without Wi-Fi for field provisioning/diagnostics. ESP32-S3 is BLE-only
(no classic SPP).

**How to apply:** IMPLEMENTED + HARDWARE-VERIFIED (2026-05-20) as `BleConsoleService` in
`src/main.cpp` over `src/console_ble.{h,cpp}` (NimBLE NUS), gated by compile-time `WITH_BLE`. Flashed
to esp32s3: boots, advertises, service start/stop clean, RT loop fine; binary 1,803,360 B / ~4% free
in the 1.87 MB OTA slot (tight). Also fixed a real `sdkconfig.defaults` bug found while building: it
had both `CONFIG_PARTITION_TABLE_CUSTOM` and `...TWO_OTA=y` (CLAUDE.md drift trap) — removed TWO_OTA
so fresh sdkconfigs keep the littlefs partition. BLE client round-trip now VERIFIED via `etc/ble_console.py` (bleak): connect over NUS, send
commands, get clean responses. Two fixes found during BLE testing: (1) serialize bleWrite with a
recursive_mutex (concurrent notify() from console echo + log-mirror tasks garbled output);
(2) `esp_log_level_set("NimBLE", ESP_LOG_WARN)` in bleConsoleBegin — NimBLE logs "notify;" at INFO
on every notify, and since logs mirror to BLE that was a feedback storm. Client gotchas: the RX
characteristic needs **write-with-response** (encrypted char drops write-no-response on some stacks);
and macOS keeps a stale LE bond after a reflash wipes the device bond → "Peer removed pairing
information" (CBError 14) — `blueutil --unpair` is a no-op for LE; must GUI "Forget This Device". Build/flow:
`WITH_BLE=1 idf.py build` (top CMakeLists layers `sdkconfig.ble`); then **verify
`build/fugu-firmware.bin` < 1,871,872 B** (OTA slot) — flash fit is the main open risk, not yet
built on target. Logging was generalized to a callback array (`addLogCallback`/`removeLogCallback`);
MQTT now uses `removeLogCallback`. Console: `service start ble`. Backend detail: arduino-esp32 BLE
lib selects NimBLE via `CONFIG_NIMBLE_ENABLED` (ESP-IDF alias of `CONFIG_BT_NIMBLE_ENABLED`).

Original design notes — the **service architecture** landed (2026-05-20): `src/service.h`
defines `Service` (`onStart`/`onStop`/`onTick`, status, per-service log level) + `ServiceManager
g_services`; see CLAUDE.md "Service architecture". Concrete wrappers that touch globals live in
`src/main.cpp` (mqtt is the exception — `MqttService` itself, with a `preStart` hook). So wrap the
NUS as a `BleConsoleService` deriving from `Service`, registered via `g_services.registerService()`
in `setup()`, config in its own `ble.conf` (`enabled`/`log_level`/`ble_security`/`ble_passkey`) —
NOT the old `board.conf` `ble` keys. Construct with `requiresNetwork=false` (no Wi-Fi precondition,
unlike ftp/telnet/scope/mqtt). Key constraint: ~315 KB flash
headroom on the 1.87 MB OTA slot, so gate the BLE stack behind a compile-time `WITH_BLE` env switch
and verify `build/fugu-firmware.bin` still fits. Also pin BT controller + NimBLE host to core 0
(RT is core 1). Still requires a multi-sink logging change (`logging.cpp` single `logCallback` →
array) so BLE + MQTT both receive logs.
