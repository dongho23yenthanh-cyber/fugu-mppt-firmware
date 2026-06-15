---
name: project_coredump_to_flash_enabled
description: "Coredump-to-flash (ELF) enabled + 64K coredump partition appended after littlefs; deploy needs a serial flash, retrieval on flat needs a console streamer"
metadata: 
  node_type: memory
  type: project
  originSessionId: dc570856-777a-497c-98e1-e784d515c042
---

Enabled 2026-05-29 to catch the AP-loss reboot ([[project_stick_wifi_reboot_preempts]]) and the
~45-min field reboots ([[project_ina226_timeout_loop_latency_shutdown]]) without serial.

**What changed:** `partitions.csv` gets `coredump,data,coredump,,64k` **appended AFTER littlefs**
(offset 0x3d9000 on 4MB) — chosen so every existing offset (nvs/ota_0/ota_1/littlefs_test/littlefs)
is byte-identical, preserving each device's littlefs config + OTA images. ~92K flash still free.
`sdkconfig.defaults`: `ESP_COREDUMP_ENABLE_TO_FLASH=y` + `DATA_FORMAT_ELF=y` + `CHECK_BOOT=y` +
`STACK_SIZE=1024`. Builds clean, app 1.70MB in the 1828K slot (9% free).

**Deploy caveat (important):** a partition-table change **cannot be OTA'd** — OTA only writes the app
slot, the table lives at 0x8000. So coredump silently no-ops until the device is **serial-flashed**
once (bootloader+parttable+app, omit littlefs to keep config). fry/flat are remote live converters →
needs physical serial access. Until then their flash has no `coredump` partition and
`esp_core_dump_image_get()` fails.

**flat DONE 2026-05-29** (serial, /dev/cu.usbmodem1201, MAC 70:04:1d:a4:ea:34, vout_rl=47e3, NVS
hostname=flat): full 4MB backup first → `flash-backups/flat-full-20260529-114130.bin` (restore-all
safety net), then flashed bootloader+table+otadata+app (NOT littlefs 0x3b9000 → config survived,
verified converting ~440W after reboot, calibration intact). `coredump info` → `none
(ESP_ERR_INVALID_SIZE)` = partition live + empty (armed). flat now also has the rollback bootloader.
fry still NOT done (needs the same serial flash). Empty-state return is INVALID_SIZE, not the
"partition not found" you'd see pre-flash.

**Retrieval:** console command `coredump [info|get|erase]` added in cli.cpp (guarded by
`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` — not the umbrella `_ENABLE`, because `esp_core_dump_image_check()`
is flash-only; the umbrella would link-break a UART-only coredump build). `coredump get` base64-streams the raw partition image between
`==COREDUMP-BEGIN size=N==`/`==COREDUMP-END==` markers over MQTT/telnet/BLE (no serial needed).
Host decode: collect lines → `base64 -d` → dump.bin →
`esp-coredump info_corefile --core-format raw -c dump.bin build/fugu-firmware.elf` (use `--core-format raw`
because the streamed bytes are the partition image incl. its coredump header, not a bare ELF).
On a serial unit you can also just `idf.py coredump-info`.

**`CHECK_BOOT=y`** logs core-dump presence at boot, but early boot logs aren't mirrored to telnet
([[project_bootlog_hook_wifi_stack_overflow]]) so that line is serial-only.

**E2E validated 2026-05-29 on 139C bench** (full wipe + flash, dry_mock cfg): added `crash null|abort|stack`
console command (cli.cpp, deliberate panic, explicit subtype required) → dump-to-flash → `coredump get`
→ host decode → backtrace. Both `esp-coredump info_corefile --core-format raw -c dump.bin <elf>` AND
`xtensa-esp32s3-elf-addr2line -pfiC -e <elf> <PCs>` work. Test harness: /tmp/coredump_e2e.py (re-discovers
the USB-CDC port across reboots — the port number changes every reset).

**Two gotchas found:**
1. `COREDUMP_STACK_SIZE=1024` was too small for the ELF dump — left **8 bytes free** and **double-faulted
   during save** (dump still saved check=ok, but fragile). Bumped to **2304** (commit ead28ed) → 1192 B
   free, clean. **fry/flat were flashed BEFORE this bump (1024)** → should be app-reflashed with the
   ead28ed build so the real AP-loss crash (likely stack-related) dumps reliably.
2. `esp-coredump` hard-fails on an app-SHA mismatch with NO skip flag (only --off/--parttable-off).
   Mismatch happens if the ELF isn't the exact build that's flashed (flash-time SHA recompute / a rebuild
   since flashing). `addr2line` on the backtrace PCs is the SHA-independent fallback and always works.
   So when pulling a real dump from fry/flat, keep the exact build artifacts that were flashed.
