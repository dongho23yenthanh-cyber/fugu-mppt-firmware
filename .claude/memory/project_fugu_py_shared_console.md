---
name: project_fugu_py_shared_console
description: etc/fugu is a separate git repo (fl4p/fugu-py); host-side transports + line-console live there
metadata: 
  node_type: memory
  type: project
  originSessionId: 034b6070-17fd-497e-bbb4-d9b27195035c
---

`etc/fugu/` is a **separate git repo** (`github.com/fl4p/fugu-py`), cloned into the firmware tree (not a submodule). Edits there are commits to that repo, not the firmware repo.

Host-side device comms is normalized here:
- `transport.py` — `Transport` ABC + `SerialTransport`, `SocketTransport` (telnet :23), `BleTransport` (NUS, bridges bleak's asyncio to the sync read/write/close via a background loop thread; `read()` returns one raw line, ANSI kept). `bleak` is an optional dep.
- `console.py` — `Console(transport)`: background line-reader, ANSI strip, `command(cmd)->Reply` (list of reply lines + `.ok/.rejected/.timed_out`, marker excluded), `drain()`, `wait_ready()`.
- `fugu.py::FuguDevice` — PWM-status-parsing device class used by `etc/ota.py`; now **consumes `Console`** (`_on_line` taps every line for PWM parse, `command_ack` collects via `console.command`). `get_conf_value` returns `m.group(2)` (the value; `group(1)` was the optional log prefix → bug, fixed).

Host-side CLI is **`etc/fugu_console.py`** (was `console_test.py`; `ble_console.py` deleted as redundant). Thin client over `Console` + a transport (dual import `from fugu...` / `from etc.fugu...`): `-c` one-shot, `-i` REPL, or default PASS/FAIL/SKIP PLAN; transport via `-p/--port` (serial, default), `--ble`/`--name`/`--address`, `--ip` (telnet), or `--mqtt BROKER --mqtt-port/-user/-pass [--mqtt-readonly]`. `etc/influx_test.py` imports `Console`+`SerialTransport`+`autodetect_port` from these. Firmware console verb is `svc` (not `service`).

**MQTT transport** (`MqttTransport`, paho-mqtt): device mirrors console output to `pv/log/<hostname>`; firmware now also subscribes `pv/log/<hostname>/cmd` and feeds `handleCommand` (added in `MqttService::onStart`, `src/tele/mqtt.cpp`), emitting the OK/ERR marker via `UART_LOG` (no tag, so it dodges mqttLogCallback's `") mqtt:"` self-filter and reaches the host on the log topic). `writable=False` = read-only monitor (write is a no-op). Hostname is learned from the first log message (substring match on `device`). Broker for the lab device: `mqtt://192.168.1.200:1882`, user `pv`. All four transports verified 15/0/22 against hardware.

NOTE: app-flashing this live device once made it drop off USB/BLE/MQTT briefly (brownout/charging load); it recovered on its own and re-enumerated under a different `/dev/cu.usbmodem*` name.

fugu-py changes committed+pushed to `origin/main` (commit dfde809). Firmware-repo edits (rename, `influx_test.py`, `CLAUDE.md`, `ble_console.py` deletion) not yet committed.
