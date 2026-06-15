---
name: project_newlib_nano_format
description: newlib-nano printf enabled to save flash; no %hh/%ll/C99 specifiers allowed in format strings
metadata: 
  node_type: memory
  type: project
  originSessionId: 77e1a793-4f64-4017-b8e0-738687edd13c
---

`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` (set in sdkconfig.defaults + sdkconfig, ESP-IDF 5.5; alias `CONFIG_NEWLIB_NANO_FORMAT`). Saves ~37 KB flash (printf/scanf family went ~45 KB → ~8 KB of `nano-*` objects). Float formatting (`%.2f` etc.) still works — `nano-vfprintf_float.o` is linked.

**Why:** firmware image is tight against the ~1.87 MB OTA slot.

**How to apply:** nano printf/scanf does NOT support C99 length modifiers `%hh` (char) or `%ll` (64-bit) — they silently misparse, not a compile error. When adding logging/snprintf:
- `%hhu`/`%hhX`/`%hhi` → drop the `hh`, use `%u`/`%02X`/`%d` (varargs promote `uint8_t`/`int8_t`→`int`).
- 64-bit values: narrow to 32-bit `(long)` + `%ld` if it fits, or split into hi/lo 32-bit words (e.g. EfuseMac: `%lX%08lX` with `(unsigned long)(v>>32)`, `(unsigned long)(v&0xFFFFFFFF)`).
- Also avoid `%z`/`%j`/`%t` and `PRIx64`/`PRIu8` inttypes macros (they expand to `ll`/`hh`).

15 sites were fixed when this was first enabled. Bigger remaining flash levers untaken: [[project_firmware_size_levers]] (-Os, log level→WARN, silent assertions).
