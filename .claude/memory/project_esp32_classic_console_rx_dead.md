---
name: project_esp32_classic_console_rx_dead
description: esp32-classic app build accepts no serial-console input (UART0 RX driver S3-gated); configure it via littlefs reflash or telnet
metadata: 
  node_type: memory
  type: project
  originSessionId: e7592dde-4174-4742-8248-a4a255e8c897
---

On **esp32-classic** (not S3) the app firmware's serial console emits logs/status on UART0 fine but
**accepts no input** — no echo, no `OK:<cmd>`. Cause: `console.cpp:141` installs the UART RX driver
(`uartInit(0)`, what `uartRead`→`uart_read_bytes` needs) only `#if CONFIG_IDF_TARGET_ESP32S3`; on
classic it relies on Arduino `Serial.begin()` alone, which in practice doesn't feed `uartRead`. So
you **cannot** configure a classic over the serial console (`wifi-add`, `set-config` are silently
dropped). The S3 bench uses USB-CDC, where input works — masking this.

Workarounds:
- **WiFi without console:** bake creds into littlefs and reflash just the data partition. `wifi.conf`
  format is `ssid_<name>=<SSID>` + `ssid_<name>_psk=<psk>` (telemetry.cpp). Keep creds out of the
  repo: `cp -r config/lab/wokwi_mock_esp32 /tmp/cfg && echo into /tmp/cfg/conf/wifi.conf`, then
  `littlefs-python create /tmp/cfg out.bin --fs-size=0x20000 --name-max=64 --block-size=4096` and
  `esptool --chip esp32 write_flash 0x3b9000 out.bin`. Validated: classic associated with `$LAB_WIFI_SSID2`
  (RSSI -32, got 192.168.1.173) this way — the AP was reachable all along; earlier `wifi-add`s just
  never executed via the dead console.
- **e2e / console once on WiFi:** drive it over **telnet** (`run_e2e --cluster console --telnet
  <ip>:23 --mqtt-host 192.168.1.200`). The telnet path waits for the banner and doesn't reset; serial
  resets on open AND races the ~9 s boot (`test_console_plan` only waits for banner when is_sock).

The **destructive/fuzz** scripts (`fuzz_sequences.py`, `fuzz_extreme.py`) are **serial-only** (hardcode
`SerialTransport`). To fuzz a classic over telnet, monkeypatch: `fe.SerialTransport = lambda *a,**k:
SocketTransport(ip,port=23,is_telnet=True)` + force `Console(eol="\n")`; for `fuzz_extreme` also set
`FUZZ_SWEEP_SPEED=0` so it doesn't reboot (drops telnet). This works — `fuzz_extreme` over telnet found
[[project_telnet_wifi_off_pbuf_uaf]]. (Coredump streaming over telnet stalls ~13 KB; pull the partition
with `parttool.py read_partition --partition-name coredump` over serial instead.)

e2e console cluster over telnet passes every firmware-applicable command. Remaining fails are
test-harness portability, NOT firmware: `console-plan` hardcodes an S3 DRAM peek addr (0x3fc88000,
rejected on classic); `stdin-batch` auto-batch shells `fugu_console.py --telnet` (valid flag is
`--ip`); `mqtt-cmd-input` `identify()` is a stub `raise NotImplementedError()`. See
[[project_esp32_classic_buck_test_uart_hijack]] for the classic test-suite caveat.
