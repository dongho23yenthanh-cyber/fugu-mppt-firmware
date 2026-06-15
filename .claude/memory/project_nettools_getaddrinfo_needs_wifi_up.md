---
name: project_nettools_getaddrinfo_needs_wifi_up
description: lwip getaddrinfo/sockets/http crash when WiFi never came up; gate nettools cmds on WiFi.status()==WL_CONNECTED
metadata: 
  node_type: memory
  type: project
  originSessionId: 54a2a0ae-fc84-42c0-96ed-e0277123f5d2
---

The `curl`/`ping`/`nslookup`/`tcpconnect` console commands (WITH_NETTOOLS, cli.cpp) call
`getaddrinfo` / lwip sockets / `esp_http_client`, which post a message to the **lwip tcpip
thread**. That thread only exists once WiFi has come up. On a device that never connected (no
creds / power-gated / wiped bench unit), the call **hard-faults in `tcpip_send_msg_wait_sem`**
(panic → reboot), not a clean error. Fixed by gating all four on `nettoolsNetUp()` =
`WiFi.status() == WL_CONNECTED` (cli.cpp), returning "network down (WiFi not connected)".
`netstat` is safe (only reads `WiFi.*`, no lwip).

**Why:** found by flashing the nettools build to a wiped bench unit and running `ping 8.8.8.8`
with no WiFi configured — instant crash. Reasoning that "tolerate=True → SKIP offline" assumed a
graceful reject; the firmware crashed instead.

**How to apply:** any console/non-RT code that resolves names or opens sockets must check the
network is actually up first — don't assume the tcpip stack is initialized. Same family as
[[project_ping_task_stack_uart_log]] (other nettools hardware-test surprise).
