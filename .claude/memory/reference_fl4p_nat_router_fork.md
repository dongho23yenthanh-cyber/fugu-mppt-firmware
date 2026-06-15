---
name: fl4p-nat-router-fork
description: "User's fork of esp32_nat_router_extended with four feature branches for the lockup fix and OTA improvements"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 6cb80ccd-7030-4a48-af84-e488c081ed1f
---

GitHub: `fl4p/esp32_nat_router_extended` (fork of `dchristl/esp32_nat_router_extended`).

Branches off upstream `master`, each independently buildable, each with a
prerequisite `cmd_system: add missing driver/gpio.h include` commit (required
for builds against framework-espidf 5.5.0):

- `liveness-watchdog` — TWDT panic flip, STA reconnect exponential backoff,
  10s-period liveness task that reboots after 60 min stuck-STA / AP-down /
  heap-floor / DNS-task-dead
- `upload-firmware-button` — `POST /uploadfw` raw-binary handler + file picker
  on `/ota`
- `editable-ota-url` — `ota_url` text input on `/advanced`, writes NVS;
  blank = fall back to upstream default
- `all-fixes` — all of the above on one branch (the actual flash target)

**Build notes:**
- esp32-s3 build needs `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` and
  `board_upload.flash_size = 4MB` / `board_build.flash_size = 4MB` in
  platformio.ini if the chip has 4 MB flash (upstream default is 2 MB which is
  too small for `larger.csv`, and PlatformIO would otherwise set the image
  header to 8 MB causing a flash-size-mismatch panic at boot).
- Flash usage ~92.5% of the 1.5 MB OTA slot.
- Build: `pio run -e esp32` or `pio run -e esp32-s3`.

A bench ESP32-S3 unit (MAC 9c:13:9e:f2:8b:50) has been flashed with `all-fixes`
and exercised; the field router at 192.168.1.173 still runs upstream until the
user flashes it (see [[nat-router-lockup-and-fork]]).
