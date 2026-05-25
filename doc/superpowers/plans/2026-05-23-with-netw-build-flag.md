*this document is an LLM generated placeholder*

# WITH_NETW Build Flag Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a compile-time flag `WITH_NETW` (default on) that, when `=0`, strips WiFi, mDNS, MQTT, telemetry, HTTPS OTA, certificates, web server, FTP, and telnet from the firmware. BLE console + BLE OTA must remain functional.

**Architecture:** Three gating layers — (1) source-list exclusion in `main/CMakeLists.txt` drops pure-network `.cpp` files; (2) source `#ifdef WITH_NETW` guards in mixed files (`main.cpp`, `cli.cpp`, `mppt.cpp`, `viz/lcd.cpp`); (3) `sdkconfig.no_netw` fragment layered by the top-level `CMakeLists.txt` disables IDF networking components (mbedtls, esp_http_client, mDNS, optionally esp_wifi/lwIP if arduino-esp32 cooperates).

**Tech Stack:** ESP-IDF 5.5, `espressif/arduino-esp32` as a component, CMake, C++.

**Spec:** `docs/superpowers/specs/2026-05-23-with-netw-build-flag-design.md`

---

## File Structure

| File | Action | Responsibility |
|---|---|---|
| `CMakeLists.txt` (top) | modify | Layer in `sdkconfig.no_netw` when `WITH_NETW=0` |
| `main/CMakeLists.txt` | modify | Define `WITH_NETW=1` macro by default; drop network sources when off |
| `sdkconfig.no_netw` | create | IDF Kconfig overrides disabling network stack |
| `src/main.cpp` | modify | `#ifdef WITH_NETW` around WiFi calls, service regs, network conf reads |
| `src/cli.cpp` | modify | `#ifdef WITH_NETW` around wifi/ip/ota commands + handlers + WiFi.h |
| `src/mppt.cpp` | modify | `#ifdef WITH_NETW` around the `WiFi.isConnected()` guard in `telemetry()` |
| `src/viz/lcd.cpp` | modify | `#ifdef WITH_NETW` around the WIFI FEATURE menu item |

No new headers, no new abstractions. The flag is a coarse on/off across the firmware.

---

## Pre-flight check

Before touching anything, confirm the baseline builds and capture its size.

- [ ] **Step 0.1: Source IDF and check current branch**

Run:
```bash
. ./idf-export.sh
git status
```
Expected: branch `scope-client`, working tree may have unrelated modifications from the spec commit.

- [ ] **Step 0.2: Capture baseline build size**

Run:
```bash
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build 2>&1 | tail -5
ls -la build/fugu-firmware.bin
```
Expected: build succeeds; record `fugu-firmware.bin` size in bytes. This is the **baseline-on** size. Every subsequent regression check (Tasks 1-5) must match this within ~512 bytes — those tasks only add `#ifdef` guards that the preprocessor keeps inactive when `WITH_NETW` is unset, so the binary should be byte-identical or near-identical.

---

## Task 1: CMake plumbing

**Files:**
- Modify: `CMakeLists.txt:5-7` (top-level — add after the existing `WITH_BLE` block)
- Modify: `main/CMakeLists.txt:22-55` (source list), `main/CMakeLists.txt:65-67` (compile-def block)

- [ ] **Step 1.1: Edit `main/CMakeLists.txt` — split network sources, add WITH_NETW define**

Replace the current `idf_component_register(SRCS ... )` block (lines 22-55) with a version that conditionally appends the network `.cpp` files. The new block:

```cmake
set(NETW_SRC
        "../src/tele/telemetry.cpp"
        "../src/tele/ftp_service.cpp"
        "../src/tele/telnet_service.cpp"
        "../src/tele/scope_service.cpp"
        "../src/tele/mqtt.cpp"
        "../src/tele/HAMqttDevice.cpp"
        "../src/tele/home_assistant.cpp"
        "../src/tele/tamp_compress.cpp"
        "../src/web/server.cpp"
        "../src/etc/ota.cpp"
)

# WITH_NETW=1 (default) compiles the WiFi/mDNS/MQTT/telemetry/HTTPS-OTA path. WITH_NETW=0 drops
# all of the above; the firmware then runs UART/USB/BLE consoles only. Reverse polarity to
# WITH_BLE on purpose — backwards-compat default is "network on".
set(WITH_NETW_DEFAULT 1)
if (DEFINED ENV{WITH_NETW} AND NOT $ENV{WITH_NETW})
    set(WITH_NETW_DEFAULT 0)
endif ()

if (NOT WITH_NETW_DEFAULT)
    set(NETW_SRC "")
endif ()

idf_component_register(SRCS
        ${MAIN_SRC}
        "../src/sensor_setup.cpp"
        "../src/mppt.cpp"
        "../src/logging.cpp"
        "../src/util.cpp"
        "../src/adc/adc_esp32_cont.cpp"
        "../src/viz/lcd.cpp"
        "../src/console.cpp"
        "../src/console_ble.cpp"
        "../src/etc/rt.cpp"
        "../src/etc/ota_ble.cpp"
        "../src/math/float16.cpp"
        ${NETW_SRC}
        INCLUDE_DIRS "." "../src"
        REQUIRES
)

if (WITH_NETW_DEFAULT)
    component_compile_definitions("WITH_NETW=1")
endif ()
```

Notes for the editor:
- The existing source list mixes network and non-network files. The diff is: remove the 10 files listed in `NETW_SRC` from the inline `SRCS` list, and append `${NETW_SRC}`.
- Keep the existing comments above the moved files (e.g., the comment about `littlefs_create_partition_image`) attached to whatever still has them; the comment block above `idf_component_register` itself stays.
- The trailing `REQUIRES` (with the commented-out arduino/esp_system/NetworkClientSecure lines) stays — those comments are still relevant documentation.

- [ ] **Step 1.2: Edit top-level `CMakeLists.txt` — layer in sdkconfig.no_netw**

Add this block immediately after the existing `WITH_BLE` block (currently lines 3-7), before the `WITH_SPROFILER` block:

```cmake
# WITH_NETW=0 layers sdkconfig.no_netw on top of the defaults to disable IDF networking
# (mbedtls / esp_http_client / esp_https_ota / mDNS / optionally WiFi+lwIP). Default (unset or
# non-zero) leaves the full network stack in. Keep in sync with the WITH_NETW define in
# main/CMakeLists.txt.
if (DEFINED ENV{WITH_NETW} AND NOT $ENV{WITH_NETW})
    if (DEFINED ENV{WITH_BLE})
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.ble;sdkconfig.no_netw")
    else ()
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.no_netw")
    endif ()
endif ()
```

The reason for the `if-else` is that `SDKCONFIG_DEFAULTS` was either left at its default (`sdkconfig.defaults`) by the existing `WITH_BLE` block being false, or set to `sdkconfig.defaults;sdkconfig.ble` when `WITH_BLE` is on. The `WITH_NETW=0` path must preserve whichever applies and append `sdkconfig.no_netw` last (last fragment wins on conflict — we want our overrides to take precedence).

- [ ] **Step 1.3: Create empty `sdkconfig.no_netw` placeholder**

Run:
```bash
echo "# WITH_NETW=0 overrides. Populated incrementally in Task 6." > sdkconfig.no_netw
```

The file must exist so the CMake config phase doesn't error when `WITH_NETW=0`. Real contents come in Task 6.

- [ ] **Step 1.4: Regression-build with default flags**

Run:
```bash
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build 2>&1 | tail -5
ls -la build/fugu-firmware.bin
```
Expected: build succeeds; `fugu-firmware.bin` size matches the baseline-on from Step 0.2 within ±512 bytes. (No source files changed; CMake is just reorganized. If the size shifts by more than this, the source list reorganization dropped or duplicated a file — diff `main/CMakeLists.txt` against HEAD.)

- [ ] **Step 1.5: Commit**

```bash
git add CMakeLists.txt main/CMakeLists.txt sdkconfig.no_netw
git commit -m "$(cat <<'EOF'
build: scaffold WITH_NETW flag (defaults on)

Splits network .cpp files into a conditional set in main/CMakeLists.txt
and adds the sdkconfig.no_netw layering hook in the top-level. Default
behavior unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Source gates in `main.cpp`

**Files:**
- Modify: `src/main.cpp` — multiple sites (headers, globals, setup, loop, network task, restart)

- [ ] **Step 2.1: Gate network-only headers**

Wrap the network-only `#include`s in `src/main.cpp` lines 20-24 and 30 and 51. The current block (lines 20-30):

```cpp
#include "tele/telemetry.h"
#include "tele/ftp_service.h"
#include "tele/telnet_service.h"
#include "tele/telemetry_service.h"
#include "tele/scope_service.h"
#include "viz/lcd.h"
#include "viz/lcd_service.h"
#include "viz/led.h"
#include "console_ble_service.h"
#include "measure_coil.h"
#include "etc/ota.h"
```

becomes:

```cpp
#ifdef WITH_NETW
#include "tele/telemetry.h"
#include "tele/ftp_service.h"
#include "tele/telnet_service.h"
#include "tele/telemetry_service.h"
#include "tele/scope_service.h"
#endif
#include "viz/lcd.h"
#include "viz/lcd_service.h"
#include "viz/led.h"
#include "console_ble_service.h"
#include "measure_coil.h"
#ifdef WITH_NETW
#include "etc/ota.h"
#endif
```

And line 51 (`#include "tele/home_assistant.h"`) becomes:

```cpp
#ifdef WITH_NETW
#include "tele/home_assistant.h"
#endif
```

Notes:
- `viz/lcd.h`, `viz/lcd_service.h`, `console_ble_service.h`, `measure_coil.h` stay unconditional — they don't pull in WiFi.
- `tele/scope_service.h` is network-only (TCP scope server). `scope.h` (the in-RAM scope buffer) is included transitively elsewhere; do not touch that.

- [ ] **Step 2.2: Gate WiFi-related globals**

`src/main.cpp:80-81`:

```cpp
bool disableWifi = false;
uint32_t wifiReenableMs = 0; // wallClockMs() deadline to auto re-enable WiFi (0 = never)
```

becomes:

```cpp
#ifdef WITH_NETW
bool disableWifi = false;
uint32_t wifiReenableMs = 0; // wallClockMs() deadline to auto re-enable WiFi (0 = never)
#endif
```

(`cli.cpp` will gate its `extern` decls in Task 3, so they stay in lockstep.)

- [ ] **Step 2.3: Gate the WiFi-connect block in `setup()`**

`src/main.cpp:209-228` (the `#ifdef NO_WIFI` block and the `if (!disableWifi)` connect block). The current text:

```cpp
#ifdef NO_WIFI
    disableWifi = true;
#endif

    TeleConf teleConf{};

    if (!disableWifi) {
        connect_wifi_async();
        bool res = wait_for_wifi();
        led.setHexShort(res ? 0x565 : 0x200);
        lcdService.displayMessage(
            res ? ("WiFi connected.\n" + std::string(WiFi.localIP().toString().c_str())) : "WiFi timeout.", 2000);

        try {
            teleConf = ConfFile{"/littlefs/conf/tele.conf"};
        } catch (const std::exception &e) {
            // telemetry host is optional — a malformed tele.conf must not brick the device
            logConfErr("tele.conf", e);
        }
    }
```

becomes:

```cpp
#ifdef WITH_NETW
#ifdef NO_WIFI
    disableWifi = true;
#endif
#endif

    TeleConf teleConf{};

#ifdef WITH_NETW
    if (!disableWifi) {
        connect_wifi_async();
        bool res = wait_for_wifi();
        led.setHexShort(res ? 0x565 : 0x200);
        lcdService.displayMessage(
            res ? ("WiFi connected.\n" + std::string(WiFi.localIP().toString().c_str())) : "WiFi timeout.", 2000);

        try {
            teleConf = ConfFile{"/littlefs/conf/tele.conf"};
        } catch (const std::exception &e) {
            // telemetry host is optional — a malformed tele.conf must not brick the device
            logConfErr("tele.conf", e);
        }
    }
#endif
```

`TeleConf teleConf{};` stays outside the `#ifdef` because `mppt.begin(... teleConf)` on line 257 uses it unconditionally — the default-constructed `TeleConf` has `influxdbHost = nullptr` which the telemetry call site already handles. Verify by reading `src/tele/telemetry.h` — `TeleConf` is a POD-ish struct with `nullptr` defaults. (If it isn't, gate the `mppt.begin` call's `teleConf` argument too — but it should be.)

- [ ] **Step 2.4: Gate service registrations**

`src/main.cpp:276-297`:

```cpp
    // Register the optional non-RT subsystems as services and start the enabled ones. MQTT keeps
    // its mppt/home-assistant wiring here (out of mqtt.cpp) via preStart, re-run on every start.
    MQTT.preStart = [](const ConfFile &mqttConf) {
        mppt.charger.beginMqtt(mqttConf);
        MQTT.onConnected = haMqttSendDiscovery;
    };
    // Periodic HA power publish, throttled inside MqttService::onTick (only ticks while Running).
    MQTT.tickHook = [] {
        if (!mppt.sensorPhysicalI || !mppt.sensorPhysicalU) return; // sensor setup failed, mppt.begin() skipped
        float pow = mppt.sensorPhysicalI->ewm.avg.get() * mppt.sensorPhysicalU->ewm.avg.get();
        haMqttUpdate({.power = mppt.isSweeping() ? NAN : pow});
    };
    g_services.registerService(&MQTT);
    g_services.registerService(&telemetryService);
    g_services.registerService(&ftpService);
    g_services.registerService(&telnetService);
    g_services.registerService(&lcdService);
    g_services.registerService(&scopeService);
#ifdef WITH_BLE
    g_services.registerService(&bleConsoleService);
#endif
    g_services.startEnabledAtBoot(); // network services may fail now; self-heal on WiFi-up edge
```

becomes:

```cpp
#ifdef WITH_NETW
    // Register the optional non-RT subsystems as services and start the enabled ones. MQTT keeps
    // its mppt/home-assistant wiring here (out of mqtt.cpp) via preStart, re-run on every start.
    MQTT.preStart = [](const ConfFile &mqttConf) {
        mppt.charger.beginMqtt(mqttConf);
        MQTT.onConnected = haMqttSendDiscovery;
    };
    // Periodic HA power publish, throttled inside MqttService::onTick (only ticks while Running).
    MQTT.tickHook = [] {
        if (!mppt.sensorPhysicalI || !mppt.sensorPhysicalU) return; // sensor setup failed, mppt.begin() skipped
        float pow = mppt.sensorPhysicalI->ewm.avg.get() * mppt.sensorPhysicalU->ewm.avg.get();
        haMqttUpdate({.power = mppt.isSweeping() ? NAN : pow});
    };
    g_services.registerService(&MQTT);
    g_services.registerService(&telemetryService);
    g_services.registerService(&ftpService);
    g_services.registerService(&telnetService);
#endif
    g_services.registerService(&lcdService);
#ifdef WITH_NETW
    g_services.registerService(&scopeService);
#endif
#ifdef WITH_BLE
    g_services.registerService(&bleConsoleService);
#endif
    g_services.startEnabledAtBoot(); // network services may fail now; self-heal on WiFi-up edge
```

Notes:
- `lcdService` stays registered unconditionally — it drives the LCD, not network.
- `scopeService` is the TCP scope server — gated.
- `startEnabledAtBoot()` stays unconditional; with no network services registered, it just starts whatever (lcd, ble) is enabled and at-boot-startable.

- [ ] **Step 2.5: Gate the `enable_esp_log_to_telnet()` call**

`src/main.cpp:301`:

```cpp
    enable_esp_log_to_telnet();
```

becomes:

```cpp
#ifdef WITH_NETW
    enable_esp_log_to_telnet();
#endif
```

- [ ] **Step 2.6: Gate the chip-overtemp WiFi-disconnect**

`src/main.cpp:523-528`:

```cpp
    if (mppt.ucTemp.last() > 95 && WiFi.isConnected()) {
        ESP_LOGW("main", "High chip temperature, shut-down WiFi");
        flush_async_uart_log();
        vTaskDelay(pdMS_TO_TICKS(200));
        WiFi.disconnect(true);
    }
```

becomes:

```cpp
#ifdef WITH_NETW
    if (mppt.ucTemp.last() > 95 && WiFi.isConnected()) {
        ESP_LOGW("main", "High chip temperature, shut-down WiFi");
        flush_async_uart_log();
        vTaskDelay(pdMS_TO_TICKS(200));
        WiFi.disconnect(true);
    }
#endif
```

- [ ] **Step 2.7: Gate the RSSI in the status log line**

`src/main.cpp:555`:

```cpp
            WiFi.RSSI()
```

is the last argument to the `UART_LOG(...)` call. The format string includes `rssi=%hi` on line 533. The simplest replacement is to drop the field entirely when `WITH_NETW` is off. Do it by introducing a local at the top of the `UART_LOG` block and gating only the value:

Edit the line `WiFi.RSSI()` (line 555) to:

```cpp
#ifdef WITH_NETW
            WiFi.RSSI()
#else
            (int16_t) 0
#endif
```

The format spec still prints `rssi=0` in netw-off builds — minor noise but keeps the format/value list aligned across compilations. (Alternative: gate the whole `UART_LOG` and write a shorter version. Skip that — too invasive for the saving.)

- [ ] **Step 2.8: Gate the network-task body**

`src/main.cpp:671-716` (the body of `loopNetwork_task`). Gate only the WiFi/services bits; **keep the function itself and `loopUart`, `flush_async_uart_log`, `process_queued_tasks`, `g_services.tickAll()`, `loopLF` calls** — those tick BLE / UART / LCD.

Replace lines 681-700 (the `disableWifi` block plus the `wifiLoop`/`startEnabledNetworkServices` block):

```cpp
    if (disableWifi && wifiReenableMs && (int32_t) (wallClockMs() - wifiReenableMs) >= 0) {
        wifiReenableMs = 0;
        disableWifi = false;
        UART_LOG("WiFi re-enabled after timeout");
        connect_wifi_async();
    }

    if (!disableWifi) {
        /* only connect with disabled power conversion
         * ESP32's wifi can cause latency issues otherwise
         */
        wifiLoop((converter.disabled() || mppt.tracker._curPower < 10) && mppt.ucTemp.last() < 80);

        // self-heal: bring up enabled network services on the WiFi-up edge (they fail to start
        // at boot when WiFi isn't connected yet). _wifiConnected() has set up MDNS by now.
        static bool wifiWasUp = false;
        bool wifiUp = WiFi.isConnected();
        if (wifiUp && !wifiWasUp) g_services.startEnabledNetworkServices();
        wifiWasUp = wifiUp;
    }
```

with:

```cpp
#ifdef WITH_NETW
    if (disableWifi && wifiReenableMs && (int32_t) (wallClockMs() - wifiReenableMs) >= 0) {
        wifiReenableMs = 0;
        disableWifi = false;
        UART_LOG("WiFi re-enabled after timeout");
        connect_wifi_async();
    }

    if (!disableWifi) {
        /* only connect with disabled power conversion
         * ESP32's wifi can cause latency issues otherwise
         */
        wifiLoop((converter.disabled() || mppt.tracker._curPower < 10) && mppt.ucTemp.last() < 80);

        // self-heal: bring up enabled network services on the WiFi-up edge (they fail to start
        // at boot when WiFi isn't connected yet). _wifiConnected() has set up MDNS by now.
        static bool wifiWasUp = false;
        bool wifiUp = WiFi.isConnected();
        if (wifiUp && !wifiWasUp) g_services.startEnabledNetworkServices();
        wifiWasUp = wifiUp;
    }
#endif
```

The `scopeService` reference in the cooperative-yield check on line 714 should also be gated:

```cpp
    if (!(scopeService.state() == ServiceState::Running && scopeService.hasClient()))
        vTaskDelay(pdMS_TO_TICKS(1));
```

becomes:

```cpp
#ifdef WITH_NETW
    if (!(scopeService.state() == ServiceState::Running && scopeService.hasClient()))
        vTaskDelay(pdMS_TO_TICKS(1));
#else
    vTaskDelay(pdMS_TO_TICKS(1));
#endif
```

- [ ] **Step 2.9: Gate the `telnetService.beginClose()` block in `systemRestart`**

`src/main.cpp:721-726`:

```cpp
    // Send the telnet FIN first and wait (≤2s) for the client to close, so a FIN lost on a weak link
    // gets retransmitted before the reset wipes the stack (else the client hangs half-open). stopAll()
    // would slam the socket shut, so do this ahead of it.
    telnetService.beginClose();
    for (int i = 0; i < 200 && telnetService.closePending(); ++i) delay(10);
    g_services.stopAll(); // tear down MQTT/telnet/etc. while WiFi is still up (see mqtt_task overflow)
```

becomes:

```cpp
#ifdef WITH_NETW
    // Send the telnet FIN first and wait (≤2s) for the client to close, so a FIN lost on a weak link
    // gets retransmitted before the reset wipes the stack (else the client hangs half-open). stopAll()
    // would slam the socket shut, so do this ahead of it.
    telnetService.beginClose();
    for (int i = 0; i < 200 && telnetService.closePending(); ++i) delay(10);
#endif
    g_services.stopAll(); // tear down enabled services while WiFi is still up (see mqtt_task overflow)
```

`g_services.stopAll()` stays unconditional — it just walks the registered services, which now excludes network services when `WITH_NETW=0`.

- [ ] **Step 2.10: Regression-build with default flags**

Run:
```bash
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build 2>&1 | tail -5
ls -la build/fugu-firmware.bin
```
Expected: build succeeds; size matches the baseline-on from Step 0.2 within ±512 bytes (only added `#ifdef`s, preprocessor skips none of them when WITH_NETW=1 is defined).

- [ ] **Step 2.11: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
gate(main): WITH_NETW around WiFi, services, telemetry

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Source gates in `cli.cpp`

**Files:**
- Modify: `src/cli.cpp` — header include, externs, wifi/ota commands, ip command, command registrations

- [ ] **Step 3.1: Gate the `<WiFi.h>` include and telemetry/OTA headers**

`src/cli.cpp:4` (`#include <WiFi.h>`), `src/cli.cpp:20` (`#include "tele/telemetry.h"`), `src/cli.cpp:21` (`#include "etc/ota.h"`):

Current:
```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <cmath>
```

becomes:
```cpp
#include <Arduino.h>
#ifdef WITH_NETW
#include <WiFi.h>
#endif
#include <cmath>
```

And lines 20-21:
```cpp
#include "tele/telemetry.h"     // connect_wifi_async, add_ap
#include "etc/ota.h"            // doOta
```
become:
```cpp
#ifdef WITH_NETW
#include "tele/telemetry.h"     // connect_wifi_async, add_ap
#include "etc/ota.h"            // doOta
#endif
```

`etc/ota_ble.h` (line 22) stays unconditional.

- [ ] **Step 3.2: Gate the WiFi externs**

`src/cli.cpp:33-34`:
```cpp
extern bool disableWifi;
extern uint32_t wifiReenableMs;
```
becomes:
```cpp
#ifdef WITH_NETW
extern bool disableWifi;
extern uint32_t wifiReenableMs;
#endif
```

- [ ] **Step 3.3: Gate `cmdWifi`, `cmdWifiAdd`, `cmdOta`, `cmdIp`**

Wrap the entire function bodies of `cmdWifi` (lines 210-236), `cmdWifiAdd` (lines 238-246), `cmdOta` (lines 252-259), and `cmdIp` (line 326) inside `#ifdef WITH_NETW` / `#endif`.

Example for `cmdWifi`:

```cpp
#ifdef WITH_NETW
static void cmdWifi(cmd *c) {
    Command cc(c);
    auto arg = cc.getArg(0).getValue();
    if (arg == "on") {
        wifiReenableMs = 0;
        disableWifi = false;
        connect_wifi_async();
    } else if (arg == "off") {
        // "off <minutes>" disables temporarily and keeps the saved ssid for reconnect;
        // bare "off" disables for good and forgets the sticky ssid.
        long mins = cc.countArgs() >= 2 ? cc.getArg(1).getValue().toInt() : 0;
        disconnect_wifi(true);
        disableWifi = true;
        if (mins > 0) {
            wifiReenableMs = wallClockMs() + (uint32_t) mins * 60000;
            UART_LOG("WiFi off for %ld min", mins);
        } else {
            wifiReenableMs = 0;
            nvs.open();
            if (!nvs.readString("wifi_ssid", "").empty())
                nvs.writeString("wifi_ssid", "");
            nvs.close();
        }
    } else {
        CMD_FAIL("wifi: expected on|off [minutes]");
    }
}
#endif
```

Repeat for `cmdWifiAdd`, `cmdOta`, `cmdIp`.

- [ ] **Step 3.4: Gate the command registrations**

`src/cli.cpp:521-523` (currently):
```cpp
    cli.addBoundlessCmd("wifi", cmdWifi); // wifi on | off [minutes]
    cli.addSingleArgCmd("wifi-add", cmdWifiAdd);
    cli.addSingleArgCmd("ota", cmdOta);
```
becomes:
```cpp
#ifdef WITH_NETW
    cli.addBoundlessCmd("wifi", cmdWifi); // wifi on | off [minutes]
    cli.addSingleArgCmd("wifi-add", cmdWifiAdd);
    cli.addSingleArgCmd("ota", cmdOta);
#endif
```

Also find and gate the `ip` and `hostname` registrations. Search for `cmdIp` in the registration block — at the time of writing it's not visible in the excerpts above, but the registrations live in `setupCli()` near the bottom of the file. Use:

```bash
grep -n 'cmdIp\|"ip"' src/cli.cpp
```

If a registration like `cli.addBoundlessCmd("ip", cmdIp);` exists, wrap it. `hostname` stays unconditional — it just edits NVS and doesn't touch WiFi.

- [ ] **Step 3.5: Regression-build with default flags**

Run:
```bash
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build 2>&1 | tail -5
```
Expected: build succeeds; size within ±512 bytes of baseline-on.

- [ ] **Step 3.6: Commit**

```bash
git add src/cli.cpp
git commit -m "$(cat <<'EOF'
gate(cli): WITH_NETW around wifi/ip/ota commands

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Source gate in `mppt.cpp`

**Files:**
- Modify: `src/mppt.cpp:400-402` — the `WiFi.isConnected()` guard at the head of `MpptController::telemetry()`

- [ ] **Step 4.1: Make `MpptController::telemetry()` a no-op when WITH_NETW=0**

Wrap the entire body of `MpptController::telemetry()` with `#ifdef WITH_NETW` / `#endif`. The function starts at `src/mppt.cpp:400` with `void MpptController::telemetry() {`. Find its closing brace:

```bash
awk 'NR>=400 && /^}$/ {print NR; exit}' src/mppt.cpp
```

Insert `#ifdef WITH_NETW` on its own line immediately after the opening `{` (i.e., as a new line after line 400) and `#endif` on its own line immediately before the closing `}` reported by the awk command. The result should look like:

```cpp
void MpptController::telemetry() {
#ifdef WITH_NETW
    if (!WiFi.isConnected() || !tele.influxdbHost || !timeSynced)
        return;

    if (wallClockUs() - _lastPointWrite < 20000) {
        return;
    }
    // ... rest of existing body untouched ...
#endif
}
```

When `WITH_NETW` is undefined, the function compiles as an empty body and the caller-side cost is one indirect call — acceptable.

- [ ] **Step 4.2: Also gate the `#include` of `tele/telemetry.h` if it's at the top of `mppt.cpp`**

Run:
```bash
grep -n 'tele/telemetry\|WiFi\.h\|tele\.h' src/mppt.cpp
```

If `mppt.cpp` `#include`s `tele/telemetry.h` directly (not via `mppt.h`), wrap that include with `#ifdef WITH_NETW`. If `tele` types (`TeleConf`, `tele.influxdbHost`) come via `mppt.h`, the include stays; instead, gate the field access — but since the whole function body is already gated, no further work needed.

- [ ] **Step 4.3: Check for any other WiFi.* call in `mppt.cpp`**

Run:
```bash
grep -n 'WiFi\.' src/mppt.cpp
```

Expected: only the one occurrence at line 401 (now gated). If more appear, gate each.

- [ ] **Step 4.4: Regression-build with default flags**

```bash
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build 2>&1 | tail -5
```
Expected: build succeeds; size within ±512 bytes of baseline-on.

- [ ] **Step 4.5: Commit**

```bash
git add src/mppt.cpp
git commit -m "$(cat <<'EOF'
gate(mppt): WITH_NETW around telemetry()

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Source gate in `viz/lcd.cpp`

**Files:**
- Modify: `src/viz/lcd.cpp:982-1020` — the WIFI FEATURE submenu item

- [ ] **Step 5.1: Gate the `subMenuPage == 8` branch**

`src/viz/lcd.cpp:982-?` (the `else if (subMenuPage == 8) { ... }` block — runs from around line 983 to the closing `}` of that else-if). Wrap the entire `else if (subMenuPage == 8) { ... }` block:

```cpp
            ///// SETTINGS MENU ITEM: WIFI FEATURE /////
            else if (subMenuPage == 8) {
                // ... existing body ...
            }
```

becomes:

```cpp
#ifdef WITH_NETW
            ///// SETTINGS MENU ITEM: WIFI FEATURE /////
            else if (subMenuPage == 8) {
                // ... existing body ...
            }
#endif
```

The block ends with a `}` at the same indentation as the `else if`. The `else if (subMenuPage == 8) {` opens at 12-space indentation; the closing `}` will be at 12-space indentation too. Find the exact line number with:

```bash
awk '/else if \(subMenuPage == 8\)/{depth=1; print NR" {"; next} depth && /\{/{depth++} depth && /\}/{depth--; if(depth==0){print NR" }"; exit}}' src/viz/lcd.cpp
```

The first printed line is the `else if` opener; the second is the matching close brace. Place `#ifdef WITH_NETW` on the line immediately before the `else if` and `#endif` on the line immediately after the matching close brace.

- [ ] **Step 5.2: Check for `enableWiFi` declarations elsewhere**

Run:
```bash
grep -n 'enableWiFi' src/viz/lcd.cpp src/viz/lcd.h
```

Expected: a global `int enableWiFi` declared somewhere in the LCD code. If it's declared in `lcd.cpp` at file scope, gate it with `#ifdef WITH_NETW`. If it's in `lcd.h` and other TUs reference it (unlikely — earlier `grep` showed no other refs), gate it there.

- [ ] **Step 5.3: Regression-build with default flags**

```bash
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build 2>&1 | tail -5
```
Expected: build succeeds; size within ±512 bytes of baseline-on.

- [ ] **Step 5.4: Commit**

```bash
git add src/viz/lcd.cpp src/viz/lcd.h
git commit -m "$(cat <<'EOF'
gate(lcd): WITH_NETW around WIFI menu item

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Populate `sdkconfig.no_netw` until WITH_NETW=0 links

This task is **iterative discovery**. The first `WITH_NETW=0` build will likely fail to link due to references arduino-esp32 holds into the WiFi/lwIP/mbedtls stacks. Each build error tells us which Kconfig symbol still needs disabling. Add to the fragment, rebuild, repeat.

**Files:**
- Modify: `sdkconfig.no_netw` (created empty in Step 1.3)

- [ ] **Step 6.1: First WITH_NETW=0 build attempt — capture failure mode**

Clean state matters here; the previous reconfigure with `WITH_NETW=1` may have cached the wrong sdkconfig:
```bash
rm -rf build
WITH_NETW=0 WITH_BLE=1 idf.py build 2>&1 | tee /tmp/netw0-build1.log | tail -80
```

Three possible outcomes:
1. **Build succeeds.** Skip to Step 6.5 (measurement). Source `#ifdef`s alone removed enough references that the linker dropped the unused code via `--gc-sections`. Unlikely but possible.
2. **Compile errors** in arduino-esp32 source (e.g., `'esp_wifi_init' was not declared`). Means a Kconfig disable is too aggressive — narrow the fragment.
3. **Link errors** (e.g., `undefined reference to esp_https_ota` or `mbedtls_*`). Means a TU still references the component; add the matching `CONFIG_*=n`.

Capture the log so we can refer back to it. The next step's content depends on which mode triggered.

- [ ] **Step 6.2: Iterate — start with the safest disables**

Append to `sdkconfig.no_netw` in this order, rebuilding between each round. Stop the first time the build succeeds.

**Round A — mDNS** (managed component, easy to drop):

```
# mDNS: not used when WITH_NETW=0
CONFIG_MDNS_MAX_INTERFACES=1
# (no CONFIG_MDNS_ENABLED key exists; mDNS is included via REQUIRES. EXCLUDE_COMPONENTS in
# top-level CMakeLists is the alternative if disabling here doesn't drop it.)
```

If `MDNS` symbols still linked, add to top-level `CMakeLists.txt` inside the `WITH_NETW=0` block:
```cmake
list(APPEND EXCLUDE_COMPONENTS mdns)
```

**Round B — esp_https_ota + esp_http_client**:

```
# CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP is not set
```

If still failing, add to EXCLUDE_COMPONENTS: `esp_https_ota` and `esp_http_client`.

**Round C — mbedtls / esp-tls**:

These are heavy. Try first:
```
# CONFIG_MBEDTLS_TLS_ENABLED is not set
```
This may break arduino-esp32 if it has TLS-using TUs compiled in (NetworkClientSecure). If so, EXCLUDE_COMPONENTS approach won't work either — leave mbedtls in.

**Round D — the big one: WiFi driver**

```
# CONFIG_ESP_WIFI_ENABLED is not set
```

If arduino-esp32 then fails to compile (its `WiFi.cpp` will), you have hit Risk #1 from the spec. Two fallbacks:
- (a) Don't disable `ESP_WIFI`; accept the smaller savings.
- (b) Layer in arduino-esp32-specific Kconfigs that make `WiFi.cpp` a stub — check `managed_components/espressif__arduino-esp32/Kconfig.projbuild` for an `ENABLE_WIFI` or similar.

Run after each round:
```bash
rm -rf build
WITH_NETW=0 WITH_BLE=1 idf.py build 2>&1 | tee /tmp/netw0-buildN.log | tail -40
```

- [ ] **Step 6.3: Document what's actually in the fragment**

Once the build succeeds, prepend a header comment to `sdkconfig.no_netw` that lists exactly which symbols are disabled and the discovered reason (e.g., "MBEDTLS left on — arduino's NetworkClientSecure forces it"). Keep this lean.

- [ ] **Step 6.4: Verify WITH_NETW=1 still builds**

Sanity check that we didn't break the default path:
```bash
rm -rf build
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build 2>&1 | tail -5
ls -la build/fugu-firmware.bin
```
Expected: size matches baseline-on from Step 0.2.

- [ ] **Step 6.5: Measure flash savings**

```bash
rm -rf build
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build > /tmp/build-netw1.log 2>&1 && ls -la build/fugu-firmware.bin
mv build build.netw1
WITH_NETW=0 WITH_BLE=1 idf.py build > /tmp/build-netw0.log 2>&1 && ls -la build/fugu-firmware.bin
```

Record the two sizes. The difference is the savings. Spec target was ≥200 KB; below that, the iteration in Step 6.2 didn't go far enough — revisit.

- [ ] **Step 6.6: Commit**

```bash
git add sdkconfig.no_netw CMakeLists.txt
git commit -m "$(cat <<'EOF'
build: sdkconfig.no_netw fragment for WITH_NETW=0

<one-sentence note on which IDF symbols ended up disabled and the
measured flash savings>

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

Fill in the `<...>` placeholder before committing — the actual disable list emerges from the iteration.

---

## Task 7: On-device verification

**Files:**
- None modified

- [ ] **Step 7.1: Pick a bench unit for the no-netw flash**

A mock-ADC device (hostname `fugu-esp32s3-*`) — NOT `fry` or `flat` (those drive real hardware over real solar). Confirm via the welcome banner or the `ip` command (last network-on build) — or, if you have physical access, use the serial port:

```bash
. ./idf-export.sh   # also sets $ESPPORT if a board is plugged in
echo $ESPPORT
```

- [ ] **Step 7.2: Build and flash the WITH_NETW=0 image**

```bash
rm -rf build
WITH_NETW=0 WITH_BLE=1 idf.py build
idf.py -p $ESPPORT flash monitor
```

Expected boot log:
- Banner shows firmware version.
- No `esp_netif_init` failure / assert.
- `setup() done.` reaches.
- `svc list` (over UART) shows no MQTT / Telemetry / FTP / Telnet / Scope services. Should list: lcd, ble_console (if WITH_BLE).
- No periodic `wifiLoop` / mDNS / MQTT logs.

Exit monitor (`Ctrl-]`).

- [ ] **Step 7.3: BLE console smoke test**

In a separate terminal:
```bash
python etc/fugu_console.py --ble -i
```

Expected: connects to the device, REPL starts, `help` lists commands (no `wifi`, `wifi-add`, `ip`, `ota` entries; `otab`, `svc`, `set-config`, etc. present), `svc list` matches what was seen over UART.

- [ ] **Step 7.4: BLE OTA smoke test**

In one terminal, serve the current firmware on a port the device can't reach (since WiFi is off, but we'll push over BLE):

```bash
cd build
python -m http.server 9000 &
```

In another, push the image over BLE using the existing flow documented in CLAUDE.md (the `otab` command path). See `etc/ota.py --ble` if it exists, or the `BleTransport`-based push in `etc/fugu/`. If a documented push script doesn't exist for direct BLE OTA, manually issue `otab begin <size> <sha256>` over the BLE REPL and stream — but this should already be a well-trodden path per the project memory notes.

Expected: OTA completes, device reboots, comes back up on the new image.

- [ ] **Step 7.5: MPPT smoke test**

Over BLE or UART console:
```
sweep
```
Expected: sweep runs without crash. (Power numbers will be whatever the mock ADC reports — irrelevant.)

```
mppt
```
Expected: tracker re-engages.

- [ ] **Step 7.6: Reflash the WITH_NETW=1 build to keep the bench unit useful**

```bash
rm -rf build
WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build
idf.py -p $ESPPORT flash
```

Expected: device boots, WiFi reconnects to the saved SSID, telnet/MQTT come back.

- [ ] **Step 7.7: Final commit (if anything was tweaked during verification)**

If Steps 7.1-7.6 surfaced bugs that required fixes, commit them. Otherwise no commit.

```bash
git status
# if dirty:
git add <files>
git commit -m "<short description>"
```

---

## Acceptance criteria

- `WITH_BLE=1 WITH_BINARY_TELE=1 idf.py build` produces a binary the same size as before this change (±512 bytes).
- `WITH_NETW=0 WITH_BLE=1 idf.py build` produces a binary that links and is ≥200 KB smaller. (Target ≥400 KB if `ESP_WIFI_ENABLED=n` worked; ≥200 KB if only the application + mbedtls/http went.)
- A bench device flashed with the `WITH_NETW=0` image boots, runs the BLE console, accepts an OTA push over BLE, and runs `sweep` / `mppt` without crashes.
- `svc list` on a `WITH_NETW=0` image contains no MQTT / Telemetry / FTP / Telnet / Scope entries.
- `git log` since the spec commit shows one commit per task (7 total, plus any verification fixups).

---

## Rollback

If anything serious surfaces, the entire change is a series of additive `#ifdef`s plus two CMake blocks. `git revert` of the task commits (or the whole range) leaves the firmware exactly as it was. The new `sdkconfig.no_netw` file is only loaded when `WITH_NETW=0`, so its presence in the tree is harmless after a revert.
