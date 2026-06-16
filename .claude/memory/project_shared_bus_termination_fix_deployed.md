---
name: project_shared_bus_termination_fix_deployed
description: Shared-bus termination fix deployed to fry/flat (2026-06-15) — terminated+no-authority yields low instead of re-arming Vbat_max; validated live. Stuck-watchdog false-trip also fixed (b310345) + re-OTA'd. Remaining open = charger.h cosmetic cleanups only.
metadata: 
  node_type: memory
  type: project
  originSessionId: b8752a87-6546-4fb2-9a24-94914d84e1f8
---

2026-06-15: Fixed fry/flat trickle-charging a full pack on the shared bat_caravan bus (multi-agent: me=deploy,
codex=main.cpp, sess-3b82dcf7=charger.h owner/validator, cline=issue #56 review). Root cause: a converter that
loses Vout authority on the shared bus (terminated float drives iout<0.5A → authority gate at main.cpp:~757 drops
it) called releaseVoutPinning()→Vbat_max(29V), climbed and re-injected (fry 14W→477W), limit-cycling ~60s.

Fix stack on main (HEAD b310345, OTA'd as c62477c = b310345 + memory note), OTA'd to both:
- 12d5876 charger.h: terminated && !voutAuthority → yield LOW to (Vbat_fallback - vout_offset_max), not Vbat_max;
  + EOC feedback gain ×n_cells (per-cell error was subtracted from PACK voltage — dimensional bug, issue #56).
- 7d62478 main.cpp:757: authority gate = iout>0.5A OR termCond (terminated float keeps authority at ~0A).
- 744995f charger.h: bat_c optional again (a hard assert had made it mandatory → would boot-loop 6 no-bat_c configs).

Validated LIVE: both TERMINATED(float), ibat≈-1.5A (no over-charge), cells 3.58→3.49, old 0↔477W cycle gone,
log shows "no Vout authority (terminated): yield ->26.2/26.44". flat=mcpwm(4103), fry=ledc(2047) — see
[[project_runtime_selectable_pwm_driver]]. The vout_offset_max float-floor ([[project_charger_shared_bms_and_flat_vout_cal]])
also confirmed working (flat converged ibat 5→2A before the yield fix landed).

RESOLVED (b310345, re-OTA'd c62477c): the CV-floor "stuck watchdog" (main.cpp:716) false-tripped on a parked
terminated+no-authority converter (0W at floor) → released pin 26.2→29 every ~14s, immediately re-yielded (0W,
cosmetic). Fixed by adding `&& !bool(mppt.charger.termCond)` to line 716 (a full pack at ~0W is healthy, not stuck;
recharge clears termCond and re-arms the backstop). Re-OTA validated live: ~0W float (flat 0.4W, fry -0.3W), no
oscillation, zero stuck-watchdog releases post-OTA.

FOLLOW-UP "periodic recharging" (c6ac56b, NOT yet OTA'd): user saw full pack getting periodic charge
pulses. Root cause was NOT termination logic (both float ~0W) — it was the repeated debug-build OTAs
themselves: each reboot ran the STARTUP sweep (main.cpp:920 startCondition path, ungated by termCond,
distinct from the periodic sweep at mppt.cpp:71 which was already gated) → ramps duty 0→max → 440-618W
pulse into the full pack before the BMS cell frame arrives, then re-latches term + floats. Fix c6ac56b + babe25b
(NOT yet OTA'd): skip startup sweep while termCond; at boot defer until termCond is actually evaluated
(charger.terminationDecided() — needs cell voltage AND warm ibat 8-frames, NOT just the first cell frame;
c6ac56b's first cut waited only for cell voltage and had a gap where the sweep fired before ibat warmed),
12s timeout fallback → charger holds Vbat_fallback so no overcharge; charger.h adds hasBmsCellSource() +
terminationDecided(). Reset behavior: full pack→no charge (correct); needs charge→sweeps once BMS confirms
cells low; BMS offline→12s then sweep to Vbat_fallback float; no-BMS board→sweeps immediately. Recharging
stops the moment OTAs stop; this is durable reboot/brownout hardening. NVS-persist-termination was considered + REJECTED:
stale flag would refuse to charge a depleted pack after a long power-off; live BMS is the source of truth
and arrives in seconds — defer-until-BMS has no staleness hazard. (If persisting anything, persist the
coulomb counter / ahSinceFull, timestamped + short-lived, not the bare flag.)

ALL OTA'd images are DEBUG builds — CONFIG_HEAP_POISONING_COMPREHENSIVE=y in sdkconfig.defaults:170
(committed, leftover from .bss-corruption debug [[project_tracker_pwmtable_oob_mcpwm]]); 5-10% slower
+16B/alloc. Live converters should run a PRODUCTION build (set HEAP_POISONING_DISABLED/_LIGHT) — user call.

flat PERIODIC AFTERNOON BURSTS (separate from boot pulses, RESOLVED 6-15 18:0x): flat (not fry) did 345-483W
charge bursts into the full pack every ~30-50min while fry floated at 0W. Root cause was CONFIG, not firmware:
(1) flat's cv_float was 3.380 vs fry's 3.350 — on the shared pack flat's higher float target ≈ the resting cell
(3.38), so flat's EOC loop stayed satisfied and parked vpack_pin at the TOP (Vbat_fallback 27.04), priming an
MPPT burst whenever sun returned; fry's lower 3.350 target pulled its pin to the floor (26.20) so it couldn't
push power. (2) flat reads Vout ~0.46V LOW (live: same bus, fry vout_avg 27.02 vs flat 26.56 at equal cells)
[[project_charger_shared_bms_and_flat_vout_cal]], making its setpoints too high in real terms. FIX APPLIED:
set-config charger.conf cv_float 3.350 on flat (matches fry) — but set-config does NOT hot-reload charger
params (cli.cpp:1185 loads into a throwaway struct; only validates), so it needs a `restart` to take effect.
Restarted flat 18:0x: v_term now 3.350 live, ibat collapsed from 27-36A bursts to ~0.2A, vpPin smoothly
descending to floor like fry, no ceiling latches. Definitive confirmation = tomorrow midday (full sun+full pack).
TODO: align repo source config (config/dl/flat + fry charger.conf still cv_float=3.37) so a reflash keeps them
matched; deeper fix = correct flat's vout_rl calibration (+0.46V). NAT: flat=192.168.1.231:234 (4.4), fry=:232.

FOLLOW-UP 6-16 dawn delay + 64-bit wall clock (179879d): BMS went offline ~05:20; termCond stayed true
until ~06:42 when BMS reconnected, blocking the startup sweep and delaying harvest 34–40 min. Fixed by
adding a BMS freshness guard in main.cpp: `full = termCond && batSt.haveValidCellVoltage()`, plus resetting
the BMS boot wait window on fresh→stale transitions so brief outages don't sweep into a full pack. Also moved
`wallClockUs()`/`wallClockMs()` to 64-bit `esp_timer_get_time()` (`time_us`/`time_ms` aliases) to eliminate the
71.5-min `micros()` wraparound that could stall backoff and other time comparisons. `vcell_high_t` kept 32-bit
for cross-core atomicity (180 s expiration < 71.6 min wrap, so unsigned subtraction still works). NOT yet OTA'd.

STILL OPEN (charger.h cosmetic, non-blocking, sess-3b82dcf7/charger.h owner): std::atomic<float> on
vpack_pin/ioutLim/Ibat_lim, vestigial ioutLim (stays NaN → dead Iout_max branch), dead balancingMode branch,
unused vbat arg on _updatePackVoltagePinning.
