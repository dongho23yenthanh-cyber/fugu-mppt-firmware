---
name: project_ble_stale_gatt_cache
description: "BLE \"Writing is not permitted\" (ATT code 3) on CCCD = stale macOS GATT cache after firmware GATT-layout change, not a firmware bug"
metadata: 
  node_type: memory
  type: project
  originSessionId: 7c3934b0-2ee2-4c66-9b09-b0da59978dac
---

Symptom: `fugu_console.py --ble` to a bonded device fails at subscribe with
`Failed to update the notification status for characteristic NN: CBATTErrorDomain Code=3 "Writing is not permitted."`

Root cause: **stale macOS GATT cache for the bonded peer**, NOT a firmware bug. macOS caches the
attribute table per bonded peer; after an OTA changes the GATT layout (e.g. the OTA-over-BLE commit
added the 4th char 6E400004, shifting handles), the Mac writes the *cached* CCCD handle, which no
longer maps to the writable TX CCCD on the new firmware → ATT code 3 (distinct from code 5/15 =
insufficient auth/enc, which would be the justworks-encryption path). Verified: the firmware NUS
notify/CCCD path is correct (under NimBLE `PROPERTY_NOTIFY`→`BLE_GATT_CHR_F_NOTIFY`, CCCD auto-created;
manual BLE2902 only under Bluedroid). With a *fresh* (unbonded) connect, `start_notify` succeeds on
flat and the 508B bench device — proves the path.

Fix now: unpair the device (`blueutil --unpair <mac>`) to drop the stale bond+cache, then reconnect
(re-bonds + rebuilds GATT fresh). Durable fix TODO: emit a GATT Service-Changed indication so bonded
centrals auto-invalidate their cache after a BLE OTA (verify the NimBLE mechanism). Renaming the
device does NOT help — the cache is keyed by bond/identity, not name.

Side finding: **fry is a weak-signal BLE peer from the Mac** (never seen in a 35s cold scan; flat ~-92
dBm). The bond is load-bearing — it let macOS re-establish the link to a weak peer; after unpair, a
cold scan can't reliably catch fry. See [[project_ble_nus_console]], [[project_fugu_py_shared_console]],
[[project_ota_over_ble]].
