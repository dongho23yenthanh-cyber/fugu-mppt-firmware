---
name: voutAuthority asymmetry on shared bus
description: When both chargers terminate, voutAuthority flag causes one to float (26.8V) and the other to floor (26.2V, 0W); confirmed live 2026-06-30 and 2026-07-07. The one with authority CAN produce full power if its calibration offset makes 26.8V setpoint > real bus voltage.
created: 2026-07-07T19:35:27.493Z
metadata:
  node_type: memory
  generator: opencode-claude-memory
  type: project
  originSessionId: ses_0c1eea39bffeUBL7oVzmNGHuRt
---

Confirmed live 2026-06-30 and 2026-07-07: when both chargers terminate on a shared bus, the `voutAuthority` flag in `_updatePackVoltagePinning()` (charger.h:296-317) creates an asymmetry:

- **With Vout authority** (converter had duty/current when termination latched): `vpack_pin` glides to `Vbat_fallback` (26.8V). If bus < 26.8V (cable drop at high current), converter produces power. On 2026-07-07 flat retained authority and produced **full power (400-530W)** all day, NOT float-limited — because flat reads ~0.35V low ([[project_charger_shared_bms_and_flat_vout_cal]]), its 26.8V setpoint was ~27.1V real, above the bus voltage, so MPPT wasn't CV-constrained. On 2026-06-30 flat was at ~400W (float-limited) — the difference is the calibration offset relative to the actual bus voltage.

- **Without Vout authority** (converter was at 0A when termination latched): `vpack_pin` pinned to `Vbat_fallback - vout_offset_max` (26.2V). Bus sits at ~26.75V → converter stays at duty=1, 0W. All solar wasted. On both 2026-06-30 and 2026-07-07, fry was the one that lost authority → 0W for hours.

The asymmetry is a race on who lost authority first. Observed: fry at 0W both times. The one with authority can be at full OR limited power depending on calibration offset vs bus voltage.

**Why:** The `voutAuthority` check (main.cpp:758-761) grants authority based on `iout > 0.5f || bool(termCond)`. When termCond is true, authority is granted even at 0A — but only if the converter had duty > `minAuthorityPwm`. A converter that already reached duty=0 before termination latched never gets authority.

**How to apply:** This is the root cause driving the load-following plan ([[project_load_following_plan]]). Phase 1 fixes it by bypassing the floor when ibat < -0.1A (battery discharging = not full). When diagnosing "why did one charger stop but not the other after termination," check which one has voutAuthority via the console log ("no Vout authority (terminated): yield X -> Y" = lost it).
