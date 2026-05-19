# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build / Flash / Monitor

This is an ESP-IDF project (v5.5+) using `espressif/arduino-esp32` as a component. Target is `esp32s3` (also supports `esp32`).

Before any `idf.py` invocation, source ESP-IDF into the shell. The repo ships a helper:

```bash
. ./idf-export.sh          # sources ../../esp/idf5.5/export.sh, sets IDF_TARGET=esp32s3, autodetects $ESPPORT
idf.py set-target esp32s3  # only needed once per build dir
idf.py build
idf.py -p $ESPPORT flash monitor
```

The top-level `CMakeLists.txt` invokes `littlefs_create_partition_image(... FLASH_IN_PROJECT)` so the **board config (currently `config/lab/wokwi_mock`) is flashed together with the firmware**. To target a different board, either edit that path in `CMakeLists.txt` before `idf.py flash`, or skip it and provision the littlefs partition separately (see below).

**OTA update over Wi-Fi:** `./ota.sh` builds, serves `build/` over HTTP on port 9000, then runs `etc/ota.py` which tells the device (via MQTT/serial) to pull the new image. The device must already have network and an `ota.conf`/MQTT broker configured.

## Provisioning (board configs)

Hardware behavior (pins, ADC selection, voltage divider ratios, current sense factors, limits, MQTT broker, charger params, coil inductance) is **not compiled in**. It lives as `.conf` files on the device's `littlefs` partition under `/littlefs/conf/`. Folders under `config/` are the source-of-truth images:

- `config/fmetal/` — Fugu2 board (the "standard" hardware)
- `config/fugu1/` — original Fugu (ADS1015 or internal ADC variants)
- `config/lab/dry_mock`, `config/lab/wokwi_mock`, `config/lab/dry_int` — bench/sim setups
- `config/psu_12v`, `config/solar-boost` — alternate topologies

To flash a config without rebuilding the firmware:

```bash
export ESPPORT=/dev/cu.usbmodem...
./provision.sh fmetal       # builds image with littlefs-python, writes via parttool.py
```

The set of conf files the firmware reads at boot (from `src/main.cpp::setup()`): `board.conf`, `sensor.conf`, `limits.conf`, `coil.conf`, `converter.conf`, `charger.conf`, `tracker.conf`, `mqtt.conf`, `tele.conf`, `pprof.conf`. Each `ConfFile` is a flat key=value parser (`src/conf.h`).

`set-config <file>.conf <key> <value>` from the serial/telnet/MQTT console edits these in place without re-flashing — useful when iterating. FTP is also enabled when Wi-Fi is up (Filezilla, 1 connection, no passive mode).

## Tests

Unit tests live under `test/` and reuse the same firmware build, swapping `main.cpp` for `test/main.cpp` via `main/CMakeLists.txt`:

```bash
RUN_TESTS=1 idf.py build flash monitor
```

To swap in a single alternate entry point without touching CMakeLists:

```bash
MAIN_SRC=../test/test_buck.cpp idf.py build
```

There's a tiny `test/host-stub/` for host-side compile checks, but the canonical test path is on-target via Unity.

## Architecture

### Dual-core task layout

ESP32-S3 cores are split deliberately (see `loopRT` in `src/main.cpp`):

- **Core 1 (RT_CORE)** — `loopRT` task at priority 20. ADC sampling, protection checks, MPPT/PD controllers, PWM updates. **Nothing else may run here**: `sdkconfig.defaults` pins Arduino, Wi-Fi/LWIP, MDNS, MQTT, and the esp_timer task to core 0; the timer ISR is on core 1 for low-latency callbacks. `loopRT` checks `xPortGetCoreID()` and `#error`s if config drifts.
- **Core 0** — Arduino `loop()` calls `loopNetwork_task`: UART/telnet console, Wi-Fi reconnect, FTP, telemetry (InfluxDB over UDP), MQTT, LCD, `loopLF` (low-frequency status line + LED).

The RT loop never sleeps voluntarily — it blocks waiting for the next ADC sample inside `adcSampler.update()`. Don't add `vTaskDelay` to the RT path.

### Control-loop pipeline (each new ADC sample)

`src/main.cpp::loopRTNewData` → `mppt.update()` (`src/mppt.cpp`):

1. **ADC sampler** (`src/adc/sampling.h` — `ADC_Sampler`) schedules async reads round-robin across channels and per-sensor IIR notch + moving median + EWM filters (`src/math/`). `Vout` is always sampled last so the controller reacts to it with minimum latency.
2. **Protection** (`mppt.protect`, `mppt.protectLf`) — hard cutouts on Vout OV, Iout OC, Vin UV, temperature; `stopAndBackoff(seconds)` disables the converter on violation.
3. **PD controllers** (`src/pd_control.h`) — five units: `VinCTRL`, `IinCTRL`, `VoutCTRL`, `IoutCTRL`, `PowerCTRL` (thermal derate). The smallest response wins each tick; if negative, MPPT halts and duty decreases proportionally.
4. **MPPT tracker** (`src/tracker.h`) — three phases: global sweep (0→max duty, captures peak), fast P&O around the captured MPP, then slow P&O. Re-sweep every 30 min or on major power change.
5. **PWM update** via `SynchronousConverter` (`src/buck.h`) — uses Vin/Vout and coil L to compute when the LS rectifier may turn on (diode emulation, CCM↔DCM), with a slow fade-in and reverse-current pullback. `L0` in `coil.conf` is undershot by 5% (`InductivityDcBias`) to model core saturation. See `doc/Diode Emulation.md`.

### Sensor abstraction

`AsyncADC<float>` (`src/adc/adc.h`) is the interface; implementations: `ADC_ADS` (ADS1x15), `ADC_INA226`, `ADC_ESP32_Cont` (continuous internal ADC DMA), `ADC_Fake` (sinusoidal mock). `setupSensors()` in `main.cpp` builds the `Sensor` instances for `vin`/`iin`/`iout`/`vout`/`ntc` from `sensor.conf`: `<chn>_adc` picks the backend, `<chn>_ch` picks the channel (255 = absent), and a missing current sensor is replaced by a `VirtualSensor` computing it from the other side and `power_conversion_eff`. `LinearTransform{factor, midpoint}` scales raw ADC → physical units (auto-derived from `*_rh`/`*_rl` for voltage dividers).

### Charger / battery termination

`src/charger.h::Li_ChgTerminationCondition` implements the LFP/Li termination line between `cv_min` (e.g. 3.37V/cell float) and `cv_eoc` (e.g. 3.65V/cell at low current). When a BMS is reachable over MQTT it publishes the highest cell voltage; the charger uses that instead of pack voltage / N_cells. If BMS data is stale (>180s, `VCELL_EXPIRATION_TIME_SEC`), `Vbat_fallback` is used.

### Console & debugging surfaces

Single command dispatcher `handleCommand()` in `src/main.cpp` handles input from UART, USB-CDC, telnet, **and** MQTT (same string protocol). Notable commands: `+N`/`-N` (PWM step), `dc N` (manual duty, switches to `manualPwm` mode), `sweep`, `mppt` (re-enable auto), `sync on/off/forced`, `bf 0/1` (backflow switch), `fan N`, `set-config`/`get-config`, `ota <url>`, `rt-stats`, `sensor`, `wifi-add ssid:psk`, `restart`. See `doc/Console.md`.

In-firmware debug: `rtcount(label)` macros (`src/etc/rt.h`) accumulate per-section timings; `sprofiler` is a sampling profiler (only useful with OpenOCD attached, configured by `pprof.conf::sprofiler_hz`); `scope` streams raw ADC over TCP for noise debugging.

## Conventions that aren't obvious from skimming

- `component_compile_options(-Werror=missing-field-initializers)` and `-Werror=attributes` are on — designated initializers must cover every field or you'll fail the build.
- `-fexceptions` is on (`CMakeLists.txt` and menuconfig). Throwing from setup is OK; throwing from the RT loop is not — wrap in try/catch and call `stopAndBackoff`.
- `IRAM_ATTR` is required on anything called from the ADC continuous-mode ISR (sdkconfig has `CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y`).
- C++20 features beyond `gnu++17` are off because enabling `gnu++20` drags in `<clocale>` via `nvs.hpp` and breaks linking — see the comment in `main/CMakeLists.txt`.
- `FUGU_BAT_V` env var at build time hardcodes battery max voltage (14.25/28.5/57); leave unset to read from `charger.conf`.
- `partitions.csv` defines two OTA slots (`ota_0`/`ota_1`) at ~1.87 MB each plus a 128 KB `littlefs` data partition at the end of flash. Don't grow OTA slots without re-checking 4 MB headroom.
- When adding a sensor: register in `setupSensors()`, **`vout` must remain the last sensor added** (lowest latency for over-voltage protection).
- **No `<sstream>` / `std::stringstream` / `std::stringbuf`.** The xtensa-esp-elf 14.2 toolchain patches `<sstream>` so `basic_stringbuf`'s constructor calls an undefined `basic_stringbuf_nop()` — a deliberate Espressif anti-bloat hook. Any TU that constructs a `std::stringbuf` (directly or via `stringstream`/`ostringstream`) will fail to link with `undefined reference to 'basic_stringbuf_nop'`. Use `snprintf` / `UART_LOG(fmt, …)` / `std::string` concatenation instead.
- `sdkconfig` is untracked and not in `.gitignore` — it drifts. If the build fails with `Failed to create littlefs image for partition 'littlefs'`, the partition-table choice has flipped from `CUSTOM` to `TWO_OTA` (joltwallet then looks up `littlefs` in ESP-IDF's built-in `partitions_two_ota.csv` and finds nothing). Fix by setting in `sdkconfig`: `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"`, and `# CONFIG_PARTITION_TABLE_TWO_OTA is not set`. The values in `sdkconfig.defaults` are correct — only the regenerated `sdkconfig` drifts.
- if you want to `git revert` but there are local dirty files, do a `git stash` before and `git stash pop` after
- whenever I ask you to create a markdown or other documentation file, put `*this document is an LLM generated placeholder*` in the first line