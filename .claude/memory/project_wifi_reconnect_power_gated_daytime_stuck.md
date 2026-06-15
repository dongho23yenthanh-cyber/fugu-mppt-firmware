---
name: project_wifi_reconnect_power_gated_daytime_stuck
description: "A daytime reboot leaves a converter stuck at 0.0.0.0 — power-gated wifiLoop won't reassociate while converting >10W"
metadata: 
  node_type: memory
  type: project
  originSessionId: 610ad048-dd59-45f2-b211-fd6ba7b257cb
---

WiFi (re)association is power-gated at `main.cpp:863`:
`wifiLoop((converter.disabled() || mppt.tracker._curPower < 10) && mppt.ucTemp.last() < 80)`.
`wifiLoop()` only calls the blocking `wait_for_wifi()` when that flag is true (associating injects RT latency).

**Consequence (cost me an investigation on 2026-05-29, flat):** if a converter reboots during the day in good
sun, it resumes converting >10W immediately, so the connect window never opens and it sits at
`Local IP Address: 0.0.0.0` — unreachable except BLE — until solar drops below 10W in the evening, then
auto-reconnects. `discover.py` returns `[]` and the havan telnet log goes silent for that host.

**Why to apply:** when a converter "drops off WiFi" mid-day, first check uptime over BLE — if it's low, it
rebooted (likely [[project_ina226_timeout_loop_latency_shutdown]]) and is simply gated out of reconnecting, not
a router/AP fault. flat also runs weak WiFi RSSI (−75..−83 dBm vs fry's −45). To recover now you must briefly
idle the converter (`dc 0`) to open a window — a power decision, ask first. Related: [[project_stick_wifi_reboot_preempts]].
