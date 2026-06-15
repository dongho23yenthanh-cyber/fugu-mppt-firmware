---
name: project_ota_over_ble
description: OTA firmware push over BLE (no WiFi) — protocol contract + partition budget
metadata: 
  node_type: memory
  type: project
  originSessionId: 231fc1fd-f7da-4b63-81d1-1e4e7dedbdce
---

OTA-over-BLE push path (no WiFi), added 2026-05-20. Host pushes the image; device flashes via native
esp_ota (not esp_https_ota). Files: `src/etc/ota_ble.cpp/.h`, `etc/ota_ble.py` (bleak), wired into
`src/console_ble.cpp` (FW char + tick) and `src/cli.cpp` (`otab` command).

Protocol contract (firmware ↔ python, keep in sync):
- New NUS char `6E400004-…` = FW data, WRITE_NR. Control via console `otab begin <size> <sha256hex>` /
  `end` / `abort`. Status as ESP_LOGI lines mirrored to the client: `OTAB READY/CRED <G>/PROG <w>/<sz>/
  OK/FAIL`. `CRED <G>` = cumulative byte offset the host may stream up to (credit window = RING_CAP 32KB).
- Data plane: FW onWrite (host task) only copies into a PSRAM ring; `otaBleTick` (net loop, in
  `bleConsoleLoop`) drains to flash in 4KB slices + streaming SHA-256, then verifies vs host digest.
- Concurrency: ALL OTA state mutation runs on the net loop. Disconnect calls `otaBleRequestAbort()`
  (flag), consumed by tick — never free OTA state on the host task. Ring + active flag guarded by
  `ringMutex`; esp_ota_write done outside the lock.

Budget: WITH_BLE image is now **~3% (≈48KB) free** in the 0x1c9000 (1871872 B) OTA slot. Any further
BLE/feature growth risks overflow — size-check every WITH_BLE build. See [[project_ble_nus_console]],
[[project_arduino_esp32_ble_version_cap]].

Verified end-to-end on hardware 2026-05-20: 1.78MB pushed over BLE in ~57s (~32KB/s), SHA verified,
device booted the OTA'd slot (ota_0→ota_1); incomplete image correctly rejected without changing boot
partition. Two gotchas hit:
- **No PSRAM on the bench board** (Total PSRAM 0, ~62KB free internal heap, fragmented) → 32KB ring
  alloc failed (`OTAB FAIL no-mem`). RING_CAP is now 8KB; BLE throughput never bottlenecks on it.
- **macOS CoreBluetooth caches GATT** per (stable) ESP32 public address, so after adding the FW char
  the Mac kept serving the old RX/TX-only service (`BleakCharacteristicNotFoundError 6e400004`). Bust
  it with `blueutil -p 0 && blueutil -p 1`. Any future GATT change on this device needs the same.
- Completion: device reboots right after queuing `OTAB OK`, so that notify usually never drains; the
  host (etc/ota_ble.py) waits for PROG==size, sends `end`, then treats the disconnect + a successful
  re-advertise as success.
