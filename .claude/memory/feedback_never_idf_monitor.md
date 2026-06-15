---
name: feedback_never_idf_monitor
description: "Never use `idf.py monitor`; use etc/fugu_console.py for all device serial/console I/O"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a52bd621-2a1d-4f9a-8d5e-d8cf3b7c1132
---

Never use `idf.py monitor` to read a device. Use `etc/fugu_console.py` for all console/monitoring — serial (`-p /dev/cu.usbmodem...`), telnet (`--ip`), BLE, or MQTT (`--mqtt`).

**Why:** user instruction (2026-05-29). Also `idf.py monitor` requires an interactive TTY and fails when run non-interactively (scripted/from the agent: "Monitor requires standard input to be attached to TTY").

**How to apply:** to capture boot/test output or send commands, drive `fugu_console.py` (e.g. `--stdin` for scripted command sequences, `-c "<cmd>"` for one-shot). For raw boot-log streaming on a device with no command console (e.g. a RUN_TESTS image), still prefer fugu_console.py over idf.py monitor. See [[project_fugu_py_shared_console.md]].
