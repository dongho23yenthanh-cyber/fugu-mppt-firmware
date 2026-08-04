---
name: BMS MQTT topic publish rates
description: bat_caravan publishes ibat (soc/current) every ~4s and cell_voltages/max every ~12s on separate MQTT topics; different rates matter for firmware control-loop design
created: 2026-07-07T08:43:40.916Z
metadata:
  node_type: memory
  generator: opencode-claude-memory
  type: reference
  originSessionId: ses_0c4446f77ffeyIL75KQ2dF5GYP
---

The BMS (bat_caravan) publishes ibat and cell voltages on **separate MQTT topics at different rates**:
- `bat_caravan/soc/current` (ibat): every ~4s
- `bat_caravan/cell_voltages/max`: every ~12s

Observed 2026-06-28 by subscribing to `bat_caravan/#` for 10s. The `CoulombCounter` already has `maxDt = 30s` as a dropout threshold, so slower BMS deployments are expected.

**Why this matters for firmware design:** The charger's `_updatePackVoltagePinning()` EOC feedback gates on `vcell_high_t` (cell voltage frame timestamp). A load-following loop that targets ibat≈0 must gate on ibat frames instead, since ibat updates 3× more frequently than cell voltage. Mixing the two rates would either overshoot (acting on stale cell-voltage data with fresh ibat) or undershoot (waiting for a cell-voltage frame to update an ibat-driven setpoint).

**How to apply:** When adding ibat-driven control loops, add a separate `ibat_t` timestamp to `BatteryState` (set in `updateBatCurrent()`) and gate on that, not on `vcell_high_t`. The load-following plan ([[project_load_following_plan]]) Phase 2 depends on this.
