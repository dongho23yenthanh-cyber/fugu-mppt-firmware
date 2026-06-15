---
name: project_mdns_uaf_on_wifi_deinit
description: WiFi.disconnect(true) deinits the netif; mDNS group-leave UAFs the freed netif on reconnect unless MDNS.end() runs first
metadata: 
  node_type: memory
  type: project
  originSessionId: fed3e06b-dfd3-423e-bfeb-917068e6e5c2
---

`WiFi.disconnect(true)` (the `wifioff=true` form) **deinits the STA netif**. esp-mdns is hooked to
netif events, so the teardown posts a group-leave / `pcb_if_deinit` to the lwip **tcpip thread**,
which calls `esp_netif_is_netif_up()` on the already-freed netif → `LoadProhibited` (EXCVADDR a
small offset like 0x000000e6). The crash fires asynchronously on the tcpip thread, typically right
after a later reconnect logs `Connected to WiFi …` (backtrace: esp_netif_is_netif_up ← join_group ←
pcb_if_deinit_lwip ← tcpip_thread).

Fix: end mDNS **while the netif is still up** before deiniting WiFi — `MDNS.end()` unhooks the
event handlers and leaves the group cleanly; the reconnect's `_wifiConnected()` then rebuilds mDNS
from scratch like boot. Implemented as `disconnect_wifi(bool off)` in telemetry.cpp (MDNS.end() then
WiFi.disconnect(off)).

**Why:** this path was dormant — the old terminal `wifi off` cleared NVS and never reconnected, so
the deinit→reinit→reconnect sequence never ran. The `wifi off <minutes>` temporary-disable feature
([[project_stick_wifi_reboot_preempts]] context) newly exercises it; `wifi on` after `wifi off` hits
it too.

**How to apply:** never `WiFi.disconnect(true)` while mDNS is running and a reconnect may follow —
go through `disconnect_wifi()`. Reproduce on a live converter only by idling it first (`dc 0`) so
the wifiLoop connect-gate (`_curPower<10 && temp<80`) opens; e2e regression is
etc/e2e-test/test_wifi_off_timeout.py (panic-aware, watches through the reconnect).
