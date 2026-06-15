---
name: project_ble_modem_sleep_thermal
description: BLE ran the S3 hot because BT controller modem sleep was off; fixed in sdkconfig.ble
metadata: 
  node_type: memory
  type: project
  originSessionId: 81c9ad8c-6c04-4357-9eb4-12f0b1d282e1
---

Enabling BLE made the ESP32-S3 run hot (user saw 70°C vs 50°C; measured on the internal temp sensor: BLE-off ~55°C, BLE-on ~64°C, ~9°C penalty). Root cause: the BT controller had **no modem sleep** (`# CONFIG_BT_CTRL_MODEM_SLEEP is not set`), so the RF front-end + radio PLL stayed powered continuously, even while only advertising.

**Fix (in `sdkconfig.ble`, the WITH_BLE overlay):**
- `CONFIG_BT_CTRL_MODEM_SLEEP=y` + `CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y` (mode 1 is the only mode the S3 controller supports) → `SLEEP_MODE_EFF=1`
- `CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y` (safe default; `EXT_32K_XTAL` saves more but needs a populated 32.768 kHz crystal)
- TX power +9 dBm → 0 dBm: `CONFIG_BT_CTRL_DFT_TX_POWER_LEVEL_N0=y` (note: 0 dBm is the **N0** enum, there is no P0). Console is near-field; secondary effect.

After flashing, BLE-on plateaus at ~56°C — modem sleep recovered nearly the whole penalty. The fast 7.5–15 ms connection interval in `console_ble.cpp` (requestConnParams) only matters while a client is connected; left as-is for console latency.

**Gotcha:** `sdkconfig` is untracked and overrides the defaults overlay, so editing only `sdkconfig.ble` won't take effect until `sdkconfig` is regenerated — patch both (the live `sdkconfig` keys + the overlay), then `idf.py reconfigure` recomputes the `_EFF` symbols. `_EFF` symbols are derived; never set them by hand.

**Measuring chip temp without a rebuild:** the status line (`src/main.cpp` UART_LOG) prints `%.0f℃%.0f℃` = NTC (nan on mock) then `ucTemp` (chip). Helper `etc/chiptemp.py <port> <secs> ["cmd;cmd"]` parses the 2nd value over USB-CDC. Toggle BLE at runtime with `set-config ble.conf enabled 0` + `restart` (allow ~2 min for thermal settling). Send console commands only after the USB-CDC port is ready (~1 s post-open) or they're dropped.

Related: [[project_ble_nus_console]], [[project_ble_notify_backpressure]].
