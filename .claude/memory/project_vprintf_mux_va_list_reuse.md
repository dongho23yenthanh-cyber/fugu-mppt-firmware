---
name: project_vprintf_mux_va_list_reuse
description: vprintf_mux reused a spent va_list for the telnet/BLE/MQTT mirror; fixed with va_copy
metadata: 
  node_type: memory
  type: project
  originSessionId: 3bccbbce-2154-49d0-ad68-a67cc3ce52b6
---

`vprintf_mux` in src/logging.cpp consumed `argptr` in `old_vprintf` (UART, first use) then reused the same spent `va_list` in `vsnprintf` to build the telnet/BLE/MQTT mirror buffer — undefined behavior that read garbage (stack float bytes) for any line with variadic args.

**Symptom:** UART/serial console clean, but BLE NUS console showed gibberish (raw IEEE-754 float bytes like `\x80\x3f`). Plain literal lines (e.g. `help`) mirrored fine; the float-heavy `loopLF` status line corrupted. UART was the first/fresh consumer, the mirror was the spent second consumer.

**Why:** a `va_list` is single-use; reusing after any `v*printf` is UB.

**How to apply:** fixed by `va_copy(ap2, argptr)` before the second `vsnprintf`, `va_end(ap2)` after. The other consumer `enqueue_log` reads its arg once, unaffected. Related: [[project_ble_nus_console]], [[project_ble_notify_backpressure]].
