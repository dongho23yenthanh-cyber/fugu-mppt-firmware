---
name: project_ota_ble_validated_and_mbuf_exhaustion
description: etc/ota_ble.py BLE-push OTA is now proven end-to-end on the live fugu-flat; plus the NimBLE mbuf/find_device gotchas hit while getting there
metadata: 
  node_type: memory
  type: project
  originSessionId: 25e630cf-14e8-4bd1-9591-04a3631c80aa
---

`etc/ota_ble.py` (BLE-push OTA, no WiFi) was broken: it sent the command `otab`, but the
firmware registers it as **`ota-ble`** (`cli.cpp` `addBoundlessCmd("ota-ble", cmdOtaBle)`) — so
every push timed out waiting for `OTAB READY`. The device's ESP_LOG tag/messages are still
`otab`/`OTAB READY` (host reply-parsing keys off those, unchanged). Fixed + validated **end-to-end
on the live fugu-flat** (2026-05-29): full 1.79 MB push over BLE → `ota_0`, rollback=VALID. Wire
protocol in `src/etc/ota_ble.cpp` is stable across recent commits.

Two non-obvious operational gotchas hit while debugging (NOT in code):

- **NimBLE mbuf exhaustion across reconnects.** Repeated macOS connect/disconnect cycles leave the
  device's NimBLE out of ACL buffers → the *first write* after connecting fails with ATT
  `17 "Insufficient Resource"` (connect+subscribe succeed, then `write_gatt_char` dies). Cure: reboot
  the device for a fresh BLE stack before a real OTA. Distinct from [[project_ble_notify_backpressure]]
  (notify TX FIFO) and [[project_ble_stale_gatt_cache]] (CCCD code-3).
- **macOS `BleakScanner.find_device_by_name` silently misses devices** — it matches the *advertised*
  `local_name`, which CoreBluetooth routinely omits (it fills the cached `d.name` instead). `discover()`
  sees the device fine. ota_ble.py now scans once and matches both names.

A no-BLE image flashed over BLE bricks the BLE path — ota_ble.py now scans the image for
`OTAB CRED`/`(NUS console)`/`BLE_HS` and confirms before pushing such a build. See
[[project_ota_over_ble]].
