---
name: esphome-ble-proxy
description: ESPHome bluetooth_proxy at 192.168.1.231 reaches fry/flat BLE NUS; their MACs; bypasses macOS bonds
metadata: 
  node_type: memory
  type: reference
  originSessionId: 782f880d-b334-401d-b306-bb9fadde11d7
---

ESPHome `bluetooth_proxy` (active) named **nimble-proxy** at **192.168.1.231:6053** (plaintext native
API, no noise PSK) reaches fry & flat over BLE NUS. Connect from the host with:
`python etc/fugu_console.py --ble-proxy 192.168.1.231 --name fugu-fry -c "mem"` (or `--address <MAC>`).

- fry MAC `70:04:1d:a6:ab:32`, flat MAC `70:04:1d:a4:ea:36`; both **public** address type (0).
- Going through the proxy does GATT on the ESP, so it **bypasses macOS BLE entirely** — sidesteps the
  stale-GATT-cache / load-bearing-bond pain in [[project_ble_stale_gatt_cache]].
- Gotcha: this ESPHome (2026.5.0) only forwards **raw** advertisements; the legacy
  `subscribe_bluetooth_le_advertisements` returns nothing. `EspHomeBleTransport._scan` parses raw AD
  bytes instead. Connection "error 534" (HCI 0x16, terminated-by-local-host) is the **normal**
  disconnect when we call disconnect, not a fault.
- Rapid reconnects (back-to-back `-c` calls) can hang the connect while the device re-advertises;
  the transport retries the GATT bring-up (`connect_retries`, default 2).

Validated 2026-05-28 against real hardware (fry/flat are live converters — only sent read-only cmds).
