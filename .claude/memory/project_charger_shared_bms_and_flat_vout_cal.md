---
name: project_charger_shared_bms_and_flat_vout_cal
description: "fry/flat/fugu139C chargers all share one BMS (bat_caravan); flat's Vout reads ~0.35V low while fry is well-calibrated"
metadata: 
  node_type: memory
  type: project
  originSessionId: 6fb9ccbe-34cd-4888-86ca-bce04f58f2b5
---

The chargers `fry`, `flat`, and `fugu-esp32s3-9804F49E139C` all subscribe to the **same** BMS over MQTT (`bat_caravan/cell_voltages/max`, `bat_caravan/soc/current`) — they're parallel chargers on one 8S LFP bank. Logs land on `havan.local:pv/fugu_console.log*` (rotated; `.br` are brotli).

Verified against BMS cell-sum over 295 matched minutes (2026-05-20): **fry/BMS = 1.006** (well-calibrated; residual is real cable IR-drop while charging, ~+0.4% at float), **flat/BMS = 0.991** (reads ~0.35 V low at float). Configs differ only in the Vout divider: fry `vout_rl=42137` (hand-trimmed, correct), flat `vout_rl=47e3` (nominal, wrong).

**Why it matters:** the CV "pin" is captured in each unit's own frame so the offset self-cancels there, but NOT for absolute limits — flat will drive the physical pack ~0.35 V above `vout_max` (29 V → ~29.35 V ≈ 3.67 V/cell) on the BMS-unavailable fallback path. flat should be recalibrated (gain ≈ 2.0/0.991, `vout_rl` ≈ 46.2k, or trim against a reference meter like fry was). Configs: `config/dl/{fry.local.192.168.4.2,flat.local.192.168.4.13}/conf/sensor.conf`. Related: [[project_recharge_after_full_periodic_sweep]].
