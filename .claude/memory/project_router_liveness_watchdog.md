---
name: project_router_liveness_watchdog
description: nimble-ble-proxy NAT router has a self-recovery liveness watchdog (reboots on LAN loss)
metadata: 
  node_type: memory
  type: project
  originSessionId: b6a70e89-844c-4796-94b3-a108b61665f5
---

The NAT router/BLE-proxy at **192.168.1.231** (repo `/Users/fab/dev/ha/nimble-ble-proxy`,
ESP32-S3) has a **liveness watchdog** as of 2026-06-11: new component `components/liveness_wdt/`,
wired in `main.cpp` after `wifi_sta::start_and_wait_for_ip()`. A task probes upstream LAN
reachability (STA default gateway:80 + broker 192.168.1.200:1882) via non-blocking connect+select;
a cycle fails only if BOTH are unreachable (avoids spurious reboots from one filtered/down target);
after `threshold` consecutive failed cycles it `esp_restart()`s.

**Why:** the IDF Task-WDT (30s) is task-based and never fires on a WiFi/coex network livelock where
tasks live but IP is dead (the OTA-transfer wedge — [[project_ota_transfer_wedges_nat_router]]).

**Config:** `CONFIG_NBP_LIVENESS_WDT=y`, `_INTERVAL_SECS=30`, `_THRESHOLD=6` (~3min) in
`sdkconfig.defaults` + Kconfig. Runtime: `GET /liveness` (state json),
`POST /liveness?enabled=0|1&interval=N&threshold=N` (NVS-persist, live off-switch),
`POST /liveness?test=1` (force reboot to prove the path). `last_ok_s:null` = still in 90s boot grace.

**How to apply / gotchas:**
- New IDF component + new CONFIG symbol added together: first `idf.py build` leaves the component
  lib EMPTY (its CMake `if(CONFIG_...)` evaluates the symbol stale) — caller `#if` sees it true →
  undefined-reference link errors. Fix: `idf.py reconfigure` then build.
- A `.cpp` whose top-level `#ifdef CONFIG_*` guard precedes any esp header compiles to NOTHING —
  IDF does NOT force-include sdkconfig.h. Put `#include "sdkconfig.h"` first. (Both bit me here.)
- OTA to this router is `curl --data-binary @build/nimble_ble_proxy.bin http://192.168.1.231/update`
  (reboots on success). The pre-fix image lacked OTA-quiesce so the direct upload can crawl/drop
  under coex contention — **just retry**; the 2nd attempt flew (107KB/s, 12s). Verify after:
  `/appinfo` elf_sha256 changes + `/liveness` returns 200. fry/flat ride the reboot on the AP.
