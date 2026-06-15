---
name: project_stick_wifi_reboot_preempts
description: "real cause of \"connects to other wifi\" is a reboot on AP loss, not wait_for_wifi reconnect timing"
metadata: 
  node_type: memory
  type: project
  originSessionId: 4c2bf733-cfe7-429d-8698-0f8269427cee
---

Tested the stick-to-wifi fix on fry+flat (2026-05-22) by restarting the router via `GET http://192.168.1.173/scan` (router itself down ~9s, confirmed by pinging it). Both devices **rebooted ~3s after losing the AP** (uptime back to ~0, "setup() done", calibration redone), came up while the router was still down, grabbed the only available AP (the weak ~-83 "other wifi"), and **stayed there after the router returned** — the device has no roam-to-better-AP-while-connected logic. That reboot is the actual mechanism of the user's symptom.

The reboot **preempts the stick logic**: on fresh boot `prevSsid` is empty, so `wait_for_wifi`'s stick path never runs and `wifiMulti.run()` just picks the strongest *available* AP. My `wait_for_wifi` backoff-vs-switch_delay fix ([[project ...]] in telemetry.cpp) is correct in isolation but moot while the device reboots first.

Reboot is **pre-existing, not from my change**: at t<6s after disconnect my edited path is byte-identical to the old code.

**Confirmed via a diagnostic build** (one-shot `ESP_LOGE("main","RESET_REASON=%d", esp_reset_reason())` in loopNetwork_task, gated on `wifiUp && millis()>9000` so it mirrors to MQTT): the OTA reboot logs `RESET_REASON=3` (SW), and the post-/scan reboot logs **`RESET_REASON=4` = ESP_RST_PANIC on both fry and flat**. So losing the AP makes the device **crash/panic** (assertions ARE compiled in — CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y level 2; PANIC_PRINT_REBOOT → fast reboot). This is DISTINCT from the chronic TWDT reboots seen 2026-05-21 (those logged `systemRestart`'s "Rebooting in 200ms" = `esp_task_wdt_isr_user_handler` main.cpp:719, 5s CPU0-idle TWDT).

**Remote service bisection done** (2026-05-22, all over MQTT; key gotcha: the device only panics if it was actually associated with .173 when /scan hits — after a panic-reboot it lands on the weak AP, so each test needs a clean `restart` first to re-acquire .173, confirmed by rssi ~-41 vs weak ~-78). Results: panic STILL occurs (RESET_REASON=4) with tele OFF, and again with tele+scope+ftp+telnet+lcd ALL off (only mqtt up). So the optional services are NOT the cause. MQTT service code is clean on disconnect (MQTT_EVENT_DISCONNECTED just clears a flag + removes log cb; publish hook guards on isConnected()). **Conclusion: the panic is in the core WiFi/network stack path on AP loss (Arduino-esp32/esp_wifi/lwip/MDNS or the wait_for_wifi reconnect calls), not firmware service code.** No ESP_ERROR_CHECK in the firmware net path; only firmware asserts are telemetry.cpp:280 (ruled out, tele off) and mqtt.cpp:202 (dispatch, not hit on disconnect) — so it's likely a CPU exception (LoadProhibited/stack) or an abort inside IDF.

**ROOT CAUSE FOUND + FIXED (2026-05-22) via serial backtrace on the 139C bench unit** (MAC 9c:13:9e:f4:04:98; .173's SSID is `$LAB_WIFI_SSID`/`$LAB_WIFI_PSK`; trick to force a device onto the weak .173 AP for testing: temporarily point the stronger SSIDs `$LAB_WIFI_SSID2`/`$LAB_WIFI_SSID3` at bogus names via `set-config wifi.conf ssid_X zz_off`, reboot, restore after). Decoded backtrace = `vApplicationStackOverflowHook → esp_system_abort → panic_abort`: **a stack overflow in `mqtt_task`**. On WiFi teardown esp-mqtt's transport floods `transport_base` errors FROM mqtt_task; the firmware's log mirror (`mqttLogCallback`, src/tele/mqtt.cpp) republishes them via `esp_mqtt_client_publish` on that same task → write to dead socket → another transport error → re-enters the mirror → unbounded recursion → overflows the task stack. The pre-existing `mqtt_cfg.task.stack_size=8192` (and the `") mqtt:"`-only skip filter) were insufficient (transport_base logs aren't tagged "mqtt"). **Fix:** re-entrancy guard in `mqttLogCallback` — record `xTaskGetCurrentTaskHandle()` around the publish and drop any log produced while already publishing on that task. Verified on 139C: after the fix, `/scan` no longer panics — N stays continuous, device rides through the outage and `wait_for_wifi` reconnects to the same AP ($LAB_WIFI_SSID).

**Regression test (2026-05-22):** `etc/e2e-test/test_stick_to_wifi.py` — taps the serial console, fires a configurable `--restart-url` webhook, asserts N stays continuous (no reboot), reconnect, and reconnect-SSID==`--ssid` (rssi-proximity fallback). Validated on a freshly-flashed bench S3 (MAC 9c:13:9e:f2:8b:50 on /dev/cu.usbmodem1101, dry_mock littlefs): 3× `GET http://192.168.1.173/scan` outages all PASS (N climbed continuously, stayed on $LAB_WIFI_SSID -86, never jumped to $LAB_WIFI_SSID2 -52). Bench recipe: `wifi-add $LAB_WIFI_SSID:$LAB_WIFI_PSK`, then `set-config wifi.conf ssid_$LAB_WIFI_SSID2/ssid_$LAB_WIFI_SSID3 zz_off`, reboot to land on $LAB_WIFI_SSID (-87), then restore $LAB_WIFI_SSID2/$LAB_WIFI_SSID3 as roam-bait (running WiFiMulti keeps only $LAB_WIFI_SSID, so only a *reboot* jumps to strong $LAB_WIFI_SSID2 -55 — confirmed). Control channel MUST be serial (or telnet not via the restarted AP): status line streams over UART through the outage so reboot detection has no blind spot.

**CORRECTION 2026-05-29 — the "resolved" claim is too strong.** flat (build 63f599b, May 28, which
*does* contain the mqtt_task guard fix) **still reboots on a real router shutdown** (uptime 300s after
the user powered its AP off). Why the earlier verification missed it: `/scan` only bounces the router's
*upstream STA* — its **soft-AP stays up the whole time**, so the fugu never disassociates; it only loses
the MQTT socket, the narrow path the guard fixes. A real **AP power-off** gives a genuine
`STA_DISCONNECTED` → a *different*, never-exercised crash path. `wifiLoop` is **power-gated**
(main.cpp:863, reconnect only when `_curPower<10`), so at ~475W the crash is NOT in the reconnect/MDNS.end()
path — it's in the **background** net stack (esp-mqtt auto-reconnect / AsyncUDP telemetry to a dead gateway /
mDNS netif-down) while the RT loop keeps converting. Root cause still UNKNOWN — needs a backtrace.
`test_stick_to_wifi.py` has the same `/scan` blind spot. Coredump-to-flash now enabled to catch it
([[project_coredump_to_flash_enabled]]).

Status (was claimed resolved, NOT actually for real AP loss): all three devices (fry, flat, 139C) run the fix and were verified to **survive /scan** on .173 (N stays continuous through the ~70s outage, reconnect to the same AP, no reboot). The RESET_REASON diagnostic was removed before the production build OTA'd to fry/flat (139C still has it — bench unit). Configs backed up to config/backup-{fry,flat}/ (live drift e.g. fry vout_rl=42137); 139C wifi backup in /tmp/139c_wifi_backup.txt. Committed on scope-client (a4cf19d): src/tele/mqtt.cpp (the fix) + src/tele/telemetry.cpp (wait_for_wifi stick timing). (The frequent "Rebooting in 200ms" lines in the 2026-05-21 log were just a test image stuck in a reboot loop per the user — NOT a real chronic TWDT issue; disregard that earlier theory.)
