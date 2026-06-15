---
name: verify-chip-before-flash
description: "Before flashing any non-fugu firmware to a device on this Mac, identify the chip and currently-running firmware — the user has multiple ESP32-S3 units that look identical"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 6cb80ccd-7030-4a48-af84-e488c081ed1f
---

Before invoking `pio run -t upload` or `esptool write_flash`, ALWAYS:
1. Confirm chip type via `esptool.py chip_id` — esp32 vs esp32-s3 matters
   (architecture mismatch = brick).
2. Boot the device and read the serial banner to see what firmware is on it
   (`Project name:` line in app_init log) — `fugu-firmware` vs
   `esp32_nat_router_extended` etc. Flashing the wrong firmware to a fugu unit
   wipes the power-controller code on a live solar charger.

**Why:** During the NAT-router-fix session on 2026-05-23, the user said "device
is connected" expecting the NAT router but had plugged in a fugu unit
(flat-equivalent) as `/dev/cu.usbmodem1101`. Esptool said ESP32-S3 instead of
the expected ESP32, and the boot banner said `fugu-firmware`. Catching this
before flashing avoided bricking the unit.

**How to apply:** When the user says "test on real device" / "flash the
firmware" without naming which device, run `chip_id` + read 5 seconds of serial
banner first. If chip/firmware mismatch the expected target, stop and confirm
via AskUserQuestion before flashing.
