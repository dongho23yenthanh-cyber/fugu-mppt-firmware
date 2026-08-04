---
name: project-bsync-beacon-pwm-sync
description: "bsync service syncs MCPWM clocks via RX-only beacon sniffing; BENCH-VALIDATED single-device (locked ±2µs @70V, survives wifi off); TSF unusable, uses rx-clock/esp_timer bridge + drift feedforward"
metadata: 
  node_type: memory
  type: project
  originSessionId: 3dee211c-6787-40a5-957b-f6ddf1dc59a8
  modified: 2026-08-04T09:06:10.310Z
---

`bsync` service (src/sync/bsync.{h,cpp}) phase-locks MCPWM switching clocks across converters
via common-view beacon sniffing (promiscuous RX-only). Bench-validated 2026-08-04 on the fboost
boost bench (converting 70 V): **locked e=±0.6–2.3 µs**, u settles at exactly 0.5+P·drift,
stays locked through `wifi off` (self-heal re-arms RX-only radio, logs warning). Grid P+½ tick,
G=2P+1 half-tick math; sigma-delta dithers period {P,P+1} (never below P — LS-comparator skip
= shoot-through). Conf bsync.conf (bssid required, channel, phase_us, kp, ki).

**Hard-won findings (would cost hours to rediscover):**
- **STA TSF does NOT run unassociated** — esp_wifi_get_tsf_time()=0 in sniffer mode. Don't
  design on it.
- **rx_ctrl.timestamp is its OWN µs clock**: not TSF-epoch, not esp_timer, but crystal-locked
  to esp_timer at a constant per-session offset (~477 ms observed); hardware-latched (stamps
  land exactly on the 102 400 µs TBTT grid). Bridge to esp_timer via max-filter of
  (rxExt−espAtCb); latency floor cancels chip-to-chip.
- **PI alone cannot lock**: P-term pull-in ~2.5 ppm, wrapped phase error integrates to ~0 →
  stuck cycle-slipping at +13 ppm crystal offset. Fix = feedforward the alpha-beta drift rate:
  u = 0.5 + P·r + kp·e + iAcc.
- Association resets power-save to MIN_MODEM → sleeps through most beacons; onTick re-forces
  WIFI_PS_NONE every second.
- Beacon accept rate only 0.2–3/s on a congested channel next to a switching converter (vs
  10/s TBTT) — alpha-beta prediction bridges it fine.
- `add_ap()` (telemetry.cpp:79) hit the [[project_conffile_inmem_multipair_miscompile]] live:
  wifi-add silently didn't persist; workaround = set-config wifi.conf ssid_X / ssid_X_psk.
- Root `sdkconfig` had CONFIG_FUGU_WITH_MCPWM unset (regenerated at some point) — bsync
  compiles itself out silently without it; re-enabled locally 8-04. Check before fry/flat OTA.

**Two-device sync VERIFIED electronically (8-04):** fboost e=+0.8µs + fbuck e=−0.9µs locked
simultaneously → relative phase 1.7µs (~24° @39kHz), zero beat; fboost auto-relocked from conf
after unattended reboot. fbuck runs unassociated-from-boot (conf channel path). fbuck config
modernized in 8076bba (missing charger.conf had aborted setup → converter uninit → `restart`
panics in ledc_stop; defensive fix still owed).

**Open:** scope shot of both switch nodes; coast/AP-loss test; RX-only noise-floor comparison;
fboost has wifi creds Johnny/fritz on littlefs. Requires pwm_driver=mcpwm
([[project_runtime_selectable_pwm_driver]]).
