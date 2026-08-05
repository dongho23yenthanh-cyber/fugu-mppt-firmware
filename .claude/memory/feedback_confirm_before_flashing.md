---
name: confirm-before-flashing
description: "Bench units: flash without asking. fry/flat (live converters): still confirm before flash/OTA."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 3dee211c-6787-40a5-957b-f6ddf1dc59a8
  modified: 2026-08-04T15:30:30.370Z
---

Bench/lab devices (fbuck, fboost, mock units): **flash without asking** — user said "always flash, dont ask" (2026-08-04), superseding the 2026-05-28 always-confirm rule. For **fry/flat (live power converters)** keep confirming before every flash/OTA: a bad image there has real consequences (load drop, brick risk pre-rollback-bootloader).

**Why:** Asking before every bench flash blocked iteration during the bsync scope campaign; the user explicitly revoked it mid-session.

**How to apply:** Build → flash bench units directly (state target device + port in the status line). Always pass the port explicitly — `ESPPORT` autodetect can land on the wrong device (e.g. the XIAO beacon node on /dev/cu.usbmodem101, which must never be reset). Related: [[feedback_verify_chip_before_flash]], [[project_no_ota_rollback_no_boot_watchdog]].
