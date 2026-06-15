---
name: project-wokwi-esp32-setup
description: "how to run this firmware in Wokwi (classic ESP32 target) — BLE off, separate build dir, esp32 littlefs override, and the telnet-forward caveat"
metadata: 
  node_type: memory
  type: project
  originSessionId: 8f3751a3-c262-460a-bad4-487c1a773f7b
---

`diagram.json` is a classic-ESP32 devkit-c-v4 board, but the project defaults to esp32s3. Running it on Wokwi requires (re-verified 2026-05-29):

1. **BLE off** — Wokwi's BT emulation hangs IDLE0, trips TWDT. The old `WITH_BLE=0` env is now **rejected** by `CMakeLists.txt:5` (Kconfig migration). Disable via a fragment: `printf 'CONFIG_FUGU_WITH_BLE=n\n' > sdkconfig.wokwi`, build with `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wokwi"` (`fugu_kconfig_bool` scans that chain).
2. **Separate build dir** `build-esp32/` — `IDF_TARGET=esp32 idf.py -B build-esp32 set-target esp32 build` (`. ./idf-export.sh` sets IDF_TARGET=esp32s3, override on the line).
3. **esp32 littlefs override** — `CMakeLists.txt:89` hardwires the esp32 littlefs to `config/fugu1/fugu1_esp32` (real ADC + real wifi → never joins Wokwi-GUEST). To run the mock, temp-edit line 89 to `config/lab/wokwi_mock_esp32` (adc=fake, ssid_Wokwi=Wokwi-GUEST, telnet enabled). The old "auto-picks wokwi_mock_esp32" claim is WRONG.
4. **IRAM overlay** — see [[project_esp32_iram_overlay]].
5. **`wokwi.toml` points at `build/`** (NOT build-esp32, despite [[reference_wokwi_cli_token]]). After building: `mv build build.s3.stash && ln -s build-esp32 build`. The symlink (esp32 target) matches diagram.json, so wokwi-cli's `./build` autodetect doesn't conflict.
6. **BLE-off build was broken** — `cli.cpp` coredump path calls `bleConsoleAwaitTxDrain()`, but `main/CMakeLists.txt:66` sets `BLE_SRC ""` when BLE off, so `console_ble.cpp` (holds the no-op stub) isn't compiled → undefined-reference link error. Fix: guard the call with `#ifdef WITH_BLE`. One-liner (left uncommitted in working tree 2026-05-29 since cli.cpp had unrelated WIP).

**Telnet-forward caveat (the wall):** for network console, run gateway with an explicit forward — `~/Downloads/wokwigw_v2.0.1_macOS_ARM64/wokwigw-darwin_arm64 --forward 2323:10.13.37.2:23` (device gets deterministic DHCP 10.13.37.2; the wokwi.toml `[[net.forward]]` did NOT take effect via wokwi-cli alone) — then `wokwi-cli --timeout 200000 --timeout-exit-code 0 .`. BUT the gateway's TCP forward **RSTs sustained telnet connections after 1-2 commands** (sim wifi rssi ~-90 dBm). So a socket-transport e2e test (e.g. `test_stdin_batch.py --telnet localhost:2323`) can't reliably complete on Wokwi — firmware boots/joins-wifi/starts-telnet/answers-first-command, then the forward resets. For console testing in Wokwi, drive the UART via wokwi-cli stdin instead of the telnet forward.

CI token: [[reference_wokwi_cli_token]]. `~/.wokwi/user.tok` is the VSCode-extension license, not the CLI token.
