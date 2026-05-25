*this document is an LLM generated placeholder*

# WITH_NETW build flag — design

## Goal

Add a compile-time flag `WITH_NETW` (default **on**) that, when set to `0`, strips all WiFi/network code (WiFi, mDNS, telemetry, MQTT, HTTPS OTA, certificates, web server, FTP, telnet) from the firmware to reduce image size. BLE console + BLE OTA must remain functional.

## Motivation

The OTA partition has ~2% free on the `WITH_BLE=1 WITH_BINARY_TELE=1` build (34 KB). Boards used only via BLE — bench units, isolated installs — don't need the WiFi stack at all. Dropping it should free 200–500 KB depending on how aggressively the toolchain lets us disable the lower layers (see Risks).

## Non-goals

- Runtime toggle. `WITH_NETW` is compile-time only; no console command to enable network at runtime.
- New polarity convention. Existing flags (`WITH_BLE`, `WITH_MCPWM`, …) default off. `WITH_NETW` defaults on for backwards compatibility — the inversion is documented in the CMake comment.
- BLE OTA changes. Existing `otab` flow over `console_ble` stays as-is.

## Architecture

Three layers of gating, mirroring how `WITH_BLE` is wired:

### Layer 1 — sdkconfig fragment

New file `sdkconfig.no_netw`, layered in by the top-level `CMakeLists.txt` only when `WITH_NETW=0`:

```cmake
if (DEFINED ENV{WITH_NETW} AND NOT $ENV{WITH_NETW})
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.no_netw")
endif ()
```

The fragment disables (in priority order — stop at the first one that breaks the link):

1. `CONFIG_MBEDTLS_TLS_ENABLED=n` (and dependent CONFIG_ESP_TLS_*)
2. `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=n` plus drop `esp_http_client` / `esp_https_ota` use via source `#ifdef`
3. mDNS off via `CONFIG_MDNS_…` (mdns is a managed component; may need EXCLUDE_COMPONENTS)
4. `CONFIG_ESP_WIFI_ENABLED=n` — biggest win, but only if arduino-esp32 will still compile

### Layer 2 — `main/CMakeLists.txt` source list

When `WITH_NETW` is unset or non-zero, add `component_compile_definitions("WITH_NETW=1")`. When `WITH_NETW=0`, omit that define and exclude the following from `MAIN_SRC`:

| File | Why |
|---|---|
| `src/tele/telemetry.cpp` | UDP InfluxDB telemetry |
| `src/tele/mqtt.cpp` | MQTT client |
| `src/tele/HAMqttDevice.cpp` | Home Assistant discovery |
| `src/tele/home_assistant.cpp` | HA wiring |
| `src/tele/ftp_service.cpp` | FTP server |
| `src/tele/telnet_service.cpp` | Telnet server |
| `src/tele/scope_service.cpp` | Scope TCP stream |
| `src/tele/tamp_compress.cpp` | Used only by binary telemetry path |
| `src/web/server.cpp` | HTTP server |
| `src/etc/ota.cpp` | HTTPS OTA (cert bundle) |

Kept unconditionally (just gated by `WITH_BLE` as today):
- `src/console_ble.cpp`, `src/etc/ota_ble.cpp`

### Layer 3 — source-level `#ifdef WITH_NETW`

In files that mix RT/control logic with WiFi calls, gate inline:

#### `src/main.cpp`

- Global state: `disableWifi`, `wifiReenableMs`
- Headers: `<WiFi.h>`, `tele/*` headers, `web/server.h`
- Conf reads in `setup()`: skip `mqtt.conf` and `tele.conf` reads. (`ftp.conf` / `telnet.conf` / `scope.conf` are read by their service `.cpp` files — already excluded by Layer 2 — no extra gating needed. WiFi credentials are in NVS, not a conf file.)
- Service registrations (`g_services.add(...)`) for MQTT/Telemetry/FTP/Telnet/Scope
- The MQTT `preStart` lambda
- `connect_wifi_async()` / `wait_for_wifi()` call in `setup()`
- Inside `loopLF()` and the chip-overtemp branch: `WiFi.isConnected()` / `WiFi.disconnect()` calls
- Inside `loopNetwork_task`:
  - The `disableWifi` re-enable timer block
  - The `wifiLoop(...)` call
  - The wifi-up-edge `startEnabledNetworkServices()` self-heal
- **Keep `loopNetwork_task` itself.** It is the only ticker for `g_services.tickAll()` (BLE service) and `loopLF()`.
- The `WiFi.RSSI()` printout in the status line: replaced with a no-op or BLE-state print

#### `src/cli.cpp`

Gate out command registrations and their handlers:
- `wifi` (on/off/[minutes])
- `wifi-add`
- `ip`
- `ota` (the `<url>` HTTPS path — `otab` is the BLE OTA, keep)
- Header `<WiFi.h>` and `extern uint32_t wifiReenableMs;`

#### `src/mppt.cpp`

Gate the telemetry-publish block at line ~401 (`WiFi.isConnected() || !tele.influxdbHost` guard and the `lp.point(...)` calls). Include is via `tele/telemetry.h` — that header is still compiled, but contains no `#ifdef`s by us; calls into it are gated at the call site.

#### `src/viz/lcd.cpp`

Gate the `enableWiFi` menu entry / toggle (lines ~993–1005).

#### `src/temperature.h`

Comment-only mention of WiFi. No code change.

## Polarity & defaults

| Env state | `WITH_NETW` macro | Behavior |
|---|---|---|
| unset | `=1` | network code compiled in (current behavior) |
| `WITH_NETW=1` | `=1` | same |
| `WITH_NETW=0` | undefined | network stripped, sdkconfig.no_netw layered |

This is reverse polarity to `WITH_BLE`, which defaults off. A comment in `main/CMakeLists.txt` documents this asymmetry next to the `if (...)` block.

## Component dependency surface

`main/CMakeLists.txt`'s `idf_component_register(... REQUIRES ...)` currently lists no networking components explicitly (they come transitively via arduino). No REQUIRES change needed; the sdkconfig fragment is what actually disables them.

## Risks

1. **arduino-esp32 may not compile with `CONFIG_ESP_WIFI_ENABLED=n`.** Its `WiFi.h`, `Network.h`, `MDNS.h` are part of the arduino archive and have static initializers. If the build fails, the fragment must be narrowed to disable only mbedtls / esp_http_client / esp_https_ota / mdns, leaving the WiFi driver + lwIP linked but unused. Estimated savings drops from 400–500 KB to 200–300 KB.
2. **lwIP often cannot be fully disabled** while arduino is present (arduino's `NetworkClient` references lwIP sockets). Same fallback as risk #1.
3. **Static initializer order.** The arduino `WiFi`/`Network` globals may run their constructors at startup and touch netif. If `CONFIG_ESP_NETIF_ENABLED=n` while arduino tries to call `esp_netif_init()`, boot crashes. Mitigation: keep `esp_netif` enabled even when WiFi is off, or accept the more conservative fallback.

These risks resolve at first build of `WITH_NETW=0`. The plan must include a build-and-link gate before further work proceeds.

## Verification

1. **Baseline build:** `WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build` — record `fugu-firmware.bin` size. Must match pre-change size within a few hundred bytes (only added CMake `if ()` blocks, no compile-def changes when `WITH_NETW` is unset).
2. **NETW-off build:** `WITH_NETW=0 WITH_BLE=1 idf.py build` — must link. Record new size; expect ≥200 KB savings.
3. **Flash to bench unit (mock-ADC, e.g. `fugu-esp32s3-*`):**
   - UART console: banner appears, `help`, `svc list` shows no MQTT/Telemetry/FTP/Telnet/Scope.
   - BLE console: connect, run commands, run `otab` push to confirm BLE OTA still works.
   - MPPT: `sweep`, then `mppt` — confirm tracker runs.
   - LCD: confirms no crash, WiFi menu entry absent.
   - No boot-time crash or `assert_failed` from netif/lwIP.
4. **Regression check:** flash `WITH_NETW=1` (default) build to a network-attached unit (e.g. `fry` or `flat` via OTA, **with care** — they drive real hardware). Confirm WiFi reconnects, telemetry hits InfluxDB, MQTT publishes.

## Open questions

None — risks above are build-time discoveries, not design decisions.
