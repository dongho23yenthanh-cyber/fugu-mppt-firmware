---
name: sysevt-task-stack-overflow
description: "WiFi-flap reboot loop = sys_evt task (2304B) overflow via vprintf_mux, NOT just the wifi task"
metadata: 
  node_type: memory
  type: project
  originSessionId: 16974906-5610-4ffb-b265-5a3c53230b3a
---

fry reboot-looped on WiFi flap: `LoadProhibited` in `esp_event_loop_run` ← `esp_event_loop_run_task`
(the **sys_evt** default-loop task), the SLIST handler-list walk dereferencing a corrupted node —
classic small-task stack overflow, not a true UAF (esp_event mutex-protects register vs. run).

Cause: once `enable_esp_log_to_telnet()` is active, an enabled-level log emitted from a WiFi/IP event
handler runs `vprintf_mux`'s 300 B `loc_buf` + mirror frames **on the handler task's stack**. The
`sys_evt` task is only `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` B → overflow.

Commit `6f02574` routes **wifi-task** logs to the light default vprintf, but that bypass keys on the
task name "wifi*" and does NOT cover the `sys_evt` task that actually crashed. Defensive fix added:
`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096` in sdkconfig.defaults (+1.8 KB RAM). Both together.

Related: [[bootlog-hook-wifi-stack-overflow]] (same vprintf_mux-on-caller-stack mechanism, wifi task),
[[vprintf_mux_static_locbuf_race]].
