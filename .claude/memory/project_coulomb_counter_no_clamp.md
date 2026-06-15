---
name: coulomb-counter-no-clamp
description: "Don't clamp CoulombCounter at 0 — it releases recharge_dod too early in multi-cycle sibling-charging scenarios"
metadata: 
  node_type: memory
  type: project
  originSessionId: cbe37a39-2028-4f95-b47b-66319b0544f7
---

`CoulombCounter` in `src/etc/coulomb_counter.h` integrates `-ibat` from MQTT (shared BMS) and is read via `ahSinceFull()` by the `recharge_dod` release check in `Li_ChgTerminationCondition::shouldRelease` (`charger.h`). With multiple chargers on one shared BMS (fry/flat on bat_caravan), sibling charging after this charger's `markFull()` pushes the counter negative — that's *semantically correct*: ahSinceFull = ∫(-ibat) dt = signed net Ah since markFull. Release fires when net discharge reaches `recharge_dod * Cbat`, at which point the pack is genuinely `recharge_dod` below the markFull baseline regardless of intermediate sibling charging.

**Don't try to "fix" this by clamping the integrator at 0.** I tried that 2026-05-25 — a clamp wipes accumulated discharge each time a sibling charge cycle would have pulled the counter negative, so a multi-cycle D→+C→D→+C→D sequence releases at far less than `recharge_dod` actual net discharge. Releases too early instead of too late.

**Why:** sibling charging after markFull is the same as the pack being recharged — it correctly reduces apparent DoD. Original integrator is right.

**How to apply:** if recharge_dod doesn't fire in practice on this hardware, the de-facto release path is the vfloor backstop in `shouldRelease()` (band `RECHARGE_VFLOOR_BAND`). Tune that band (currently 0.10; was 0.05 and was releasing too high on the LFP curve at vcell_high<3.32V), or sync `markFull()` across chargers via MQTT so per-charger baselines don't drift. Don't clamp the integrator. See also [[project_charger_shared_bms_and_flat_vout_cal]] for the multi-charger context.
