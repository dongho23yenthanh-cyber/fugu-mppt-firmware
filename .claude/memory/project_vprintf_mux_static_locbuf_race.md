---
name: project_vprintf_mux_static_locbuf_race
description: vprintf_mux static loc_buf raced across core-0 tasks; mirror showed float garbage. Made it stack-local
metadata: 
  node_type: memory
  type: project
  originSessionId: 82fe4def-cb80-427a-945a-32d3afa76b65
---

`vprintf_mux` in src/logging.cpp built the telnet/BLE/MQTT mirror in a `static char loc_buf[300]`. It runs concurrently on core 0 from the network-loop task (`loopLF` float-heavy status line) AND the MQTT client task (`cmd_input`→`handleCommand`→`UART_LOG`, plus every `ESP_LOGx` in the mqtt event handler). Preemption mid-`vsnprintf` clobbered the shared buffer → mirror emitted leftover IEEE-754 float bytes.

**Symptom (a second time):** UART clean, MQTT/telnet console showed float-byte gibberish (`�B??`) inside e.g. the `help` reply. Distinct from [[project_vprintf_mux_va_list_reuse]] — that was argptr reuse (fixed with va_copy, still in place); this is a shared static buffer. Same mirror path, different bug.

**Why UART is unaffected:** `old_vprintf(fmt, argptr)` consumes argptr directly and never touches loc_buf; only the mirror copy uses loc_buf.

**Fix:** dropped `static` → `char loc_buf[300];` (stack-local). Also fixes re-entrancy when a log callback itself logs. 300 B stack is fine (mqtt task stack is 8192).
