*this document is an LLM generated placeholder*

# RF power saving (WiFi + BLE modem-sleep)

Both radios idle hot if left at full duty. Two independent modem-sleep mechanisms cut that.
Core 1 (RT: ADC/MPPT/PWM) is untouched by either — both radios live on core 0.

## WiFi

`WiFi.setSleep(...)` in `src/tele/telemetry.cpp::connect_wifi_async()` (baseline) and
`src/main.cpp::networkLoopTick()` (day/night switch):

- Baseline on connect: `WIFI_PS_MIN_MODEM` — modem sleeps between DTIM beacons (~100 ms),
  console/MQTT/OTA stay responsive. (Default was `WIFI_PS_NONE` — radio always on.)
- Idle > 5 min (not converting: `converter.disabled() || tracker._curPower < 10`, the same
  test that gates WiFi reconnect): deepen to `WIFI_PS_MAX_MODEM` to cut night standby draw.
- Back to `MIN_MODEM` the instant PV power returns. `psMode` is reset on the WiFi-up edge
  because a reconnect re-applies the `MIN_MODEM` baseline.
- Transitions log `WiFi power save: MAX_MODEM (night/idle)` / `MIN_MODEM (active)`.

Why power-gated on PV rather than a clock: robust to NTP failure, handles overcast, reuses a
threshold the firmware already trusts. The 5-min hysteresis stops flapping at dawn/dusk.

Not done: `WIFI_PS_MAX_MODEM` as the *baseline* (adds latency to unsolicited inbound TCP —
telnet/MQTT-cmd/OTA), and `CONFIG_PM_ENABLE` (CPU DFS/light-sleep would jitter the RT loop).

## BLE

Controller modem-sleep via `CONFIG_BT_CTRL_MODEM_SLEEP` / `..._MODE_1` in `sdkconfig.ble`.
BLE-on previously ran the S3 ~9 °C hotter (RF/PLL never sleeping). Advertising is left at the
NimBLE fast default (~30–60 ms) and the device only ever advertises (peripheral, no scanning),
so there's no scan cost to trim.

## Measuring

`etc/chiptemp.py` reads the on-die sensor — the real metric for any RF power change. Compare
before/after at the same ambient; modem-sleep deltas are single-digit °C.
