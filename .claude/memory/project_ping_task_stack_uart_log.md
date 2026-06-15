---
name: project_ping_task_stack_uart_log
description: "lwip ping callbacks UART_LOG in the internal ping task; default 2048 stack overflows, bump to 6144"
metadata: 
  node_type: memory
  type: project
  originSessionId: 54a2a0ae-fc84-42c0-96ed-e0277123f5d2
---

The `ping` console command (WITH_NETTOOLS, cli.cpp) drives the lwip ping app; its `on_ping_success/timeout/end` callbacks run in lwip's **internal ping task**, not the console task. They call `UART_LOG`, which fans out through `vprintf_mux` (300 B stack-local `loc_buf` + telnet/MQTT/BLE mirrors). The default `ESP_TASK_PING_STACK` (2048+extra) overflowed after the first reply ("stack overflow in task ping"). Fix: `pc.task_stack_size = 6144`.

**Why:** anything that calls `UART_LOG`/`ESP_LOGx` from a task with a small stack risks this — the mux path is stack-heavy. Same failure class as [[project_bootlog_hook_wifi_stack_overflow]] (3072 B wifi task) and the [[project_vprintf_mux_static_locbuf_race]] / [[project_vprintf_mux_va_list_reuse]] line of bugs.

**How to apply:** when a callback/task you don't own logs via the mux, size its stack ≥6 KB, or marshal the data out and log from the console/net task instead.
