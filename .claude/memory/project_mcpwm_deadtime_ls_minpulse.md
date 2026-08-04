---
name: mcpwm-deadtime-ls-minpulse
description: "MCPWM HiLi dead-time path validated on fbuck; \"missing\" LS gate signal at ~0 A is the 300 ns minLS keep-alive pulse"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2df7d10d-22b3-4858-bc6e-224d6790bd83
  modified: 2026-08-04T09:33:18.364Z
---

2026-08-04, fbuck lab bench (usbmodem, hostname fugu-esp32s3-184082188534): reported "no LS
gate-drive signal" was NOT a bug. At near-zero output current the diode-emulation logic pins LS
at pwmRectMin=80 counts (~500 ns @160 MHz tick); the 200 ns MCPWM dead-time
(board.conf::pwm_deadtime_ns=200 → 32 RED ticks) shrinks the actual gate pulse to ~300 ns —
easy to miss on a scope. Pinning it wide (`dc <hs> <ls>`, e.g. `dc 1506 500`) made it visible;
user confirmed signal.

This also **hardware-validates the HiLi + dtTicks>0 dead-time path-swap config** in
src/pwm/mcpwm.h (1-tick FED on HS to claim path 1, RED on LS): fry/flat run dtTicks=0, so fbuck
is the first board exercising it. Verified against IDF mcpwm_gen.c semantics too.

Related: [[project_mcpwm_dt_pair_two_calls]], [[project_bsync_beacon_pwm_sync]].

Side cruft on fbuck: converter.conf has ignored keys `boost` and `vout_max` (unknown-key
warnings at boot), and the device boots into fixed duty 1506 ("not performing tracking").
