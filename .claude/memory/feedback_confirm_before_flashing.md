---
name: confirm-before-flashing
description: "Always ask for explicit confirmation before flashing/OTA-ing any device — bench units included, not just fry/flat."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 6509c06f-da91-4fcc-bb1a-5c0d8efaa58a
---

Get an explicit "go" from the user **before** every flash or OTA, including bench/mock devices (e.g. 139C), not only the real converters.

**Why:** On 2026-05-28 I USB-flashed 139C (a mock-ADC bench unit) without asking; the user's "before flashing confirm" arrived while the flash was already running. Even low-risk bench flashes should be confirmed — the user wants control over the exact moment any device is written.

**How to apply:** When a build is ready and the next step is flash/OTA, pause and confirm first (state target device + what gets written, e.g. "app + ota_data only, littlefs preserved"). Especially load-bearing for fry/flat (real power converters; a bad OTA bricks them — no rollback). Related: [[feedback_verify_chip_before_flash]], [[project_no_ota_rollback_no_boot_watchdog]].
