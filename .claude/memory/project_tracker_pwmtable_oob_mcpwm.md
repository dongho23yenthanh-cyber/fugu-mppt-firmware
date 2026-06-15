---
name: project_tracker_pwmtable_oob_mcpwm
description: "flat .bss corruption root cause = Tracker pwm tables sized 2048, OOB under MCPWM pwmMax~4103"
metadata: 
  node_type: memory
  type: project
  originSessionId: db39b768-6676-45d3-a86b-c00184bc3fa7
---

The recurring `flat` corruption (wild writes into the global `mppt` .bss: `charger.termCond.p`, `bflow.panelEN`, `limits.Vin_max`) is an out-of-bounds array write in `src/tracker.h`:

- `pwmPowerTable` is `std::array<EWMA_N<80>, 2048>` and `pwmTimeTable` is `std::array<unsigned long, 2048>` (tracker.h:43-44), both indexed by `dutyCycle` with NO clamp (lines 75/76/78/79).
- Call site `mppt.cpp:217`: `tracker.update(power, converter.getCtrlOnPwmCnt(), vin)` passes the raw PWM count as the index.
- LEDC pwmMax≈2047 → index fits [2048]. **MCPWM `bestTiming(39kHz)=4103`** → sweep/track drives `dutyCycle` to ~4103 → `pwmTimeTable[dutyCycle]=wallClockMs()` writes thousands of elements past the arrays. `Tracker` (mppt.h:212) precedes limits/charger/bflow (231/233/234), so the overflow lands exactly on the corrupted fields.

Proof: corrupted values are wallClockMs timestamps, not pointers/scope words. `limits.Vin_max=0x0000AACE=43726 ms` = ~43.7s uptime (matches "fires ~33-54s post-boot"); `termCond.p=0x00374a3b=3,623,483 ms` ≈ 60 min uptime. Varying `dutyCycle` → varying victim field = the "scattered single-field writes".

This is a REGRESSION: flat ran LEDC stably (default `CONFIG_FUGU_WITH_MCPWM=n`) through Jun 8, switched to MCPWM (e6762ce) → pwmMax doubled → OOB. fry tracks at lower duty / sweeps less so it crosses 2048 rarely. The earlier "scope buf[-1]" (319cbaa) and bootLogCapture/MQTT-defer fixes were RED HERRINGS — corruption persisted after each. Found via `elf-archive/index.jsonl` flash log (568a5d8 stable >24h → e6762ce corrupting). Fix: clamp the index in `Tracker::update`, or size the tables to ≥ max MCPWM pwmMax. Related: [[project_mcpwm_validated_on_live_flat]], [[project_flat_dawn_floor_stuck_after_reboot]].
