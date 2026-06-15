---
name: project_shared_bus_termination_fix_deployed
description: Shared-bus termination fix deployed to fry/flat (2026-06-15) — terminated+no-authority yields low instead of re-arming Vbat_max; validated live. Open follow-up = stuck-watchdog false-trip.
metadata: 
  node_type: memory
  type: project
  originSessionId: b8752a87-6546-4fb2-9a24-94914d84e1f8
---

2026-06-15: Fixed fry/flat trickle-charging a full pack on the shared bat_caravan bus (multi-agent: me=deploy,
codex=main.cpp, sess-3b82dcf7=charger.h owner/validator, cline=issue #56 review). Root cause: a converter that
loses Vout authority on the shared bus (terminated float drives iout<0.5A → authority gate at main.cpp:~757 drops
it) called releaseVoutPinning()→Vbat_max(29V), climbed and re-injected (fry 14W→477W), limit-cycling ~60s.

Fix stack on main (HEAD 744995f), OTA'd to both:
- 12d5876 charger.h: terminated && !voutAuthority → yield LOW to (Vbat_fallback - vout_offset_max), not Vbat_max;
  + EOC feedback gain ×n_cells (per-cell error was subtracted from PACK voltage — dimensional bug, issue #56).
- 7d62478 main.cpp:757: authority gate = iout>0.5A OR termCond (terminated float keeps authority at ~0A).
- 744995f charger.h: bat_c optional again (a hard assert had made it mandatory → would boot-loop 6 no-bat_c configs).

Validated LIVE: both TERMINATED(float), ibat≈-1.5A (no over-charge), cells 3.58→3.49, old 0↔477W cycle gone,
log shows "no Vout authority (terminated): yield ->26.2/26.44". flat=mcpwm(4103), fry=ledc(2047) — see
[[project_runtime_selectable_pwm_driver]]. The vout_offset_max float-floor ([[project_charger_shared_bms_and_flat_vout_cal]])
also confirmed working (flat converged ibat 5→2A before the yield fix landed).

OPEN FOLLOW-UP (benign, codex owns): the CV-floor "stuck watchdog" (main.cpp:716, stuck = headroom && noPower &&
!manualPwm) false-trips on a parked terminated+no-authority converter (0W at floor) → release pin 26.2→29 every
~14s, immediately re-yielded. Power stays 0W (no over-charge), purely cosmetic. Fix = add `&& !bool(mppt.charger.termCond)`
to line 716. Other non-blocking cleanups logged: std::atomic<float> on vpack_pin/ioutLim, vestigial ioutLim, dead
balancingMode branch, unused vbat arg.
