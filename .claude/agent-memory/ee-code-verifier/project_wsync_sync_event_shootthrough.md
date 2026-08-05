---
name: wsync-sync-event-shootthrough
description: Wired-sync (WITH_WSYNC) follower sync event drives LS LOW and HS HIGH simultaneously with zero dead-time — shoot-through on any off-phase sync edge
metadata:
  type: project
---

On a `sync_role=follower`, `initSyncIn()` (src/pwm/mcpwm.h) sets, on the SAME sync trigger:
LS generator → LOW (public API) and HS generator → HIGH (`mcpwm_ll_generator_set_action_on_trigger_event`).
Neither passes through the dead-time submodule for that transition — the DT submodule delays only
the **LS posedge** (RED on gen B), and `pwmMax = periodTicks - dtTicks` protects the *normal*
LS→HS wrap only. So a sync edge arriving at an arbitrary count while LS is conducting produces
LS-off / HS-on with **0 ns** commanded dead-time despite `pwm_deadtime_ns=200`.

**Why:** in steady lock the sync coincides with the follower's own TEZ (LS already off), so it is
invisible on the bench. It only bites on first lock, relock after a wire break, and — the dangerous
case — every noise-induced spurious edge. At a 75 V link one event is ~30–80 ns of cross-conduction
(LS turn-off delay minus HS turn-on delay); a chattering wire repeats it at kHz.

**How to apply:** before running wsync on a live converter, require (a) a GPIO glitch filter on the
sync pin (`gpio_new_flex_glitch_filter`, ~200 ns window, absorbed by `sync_phase_ns`), and (b) arming
the sync input only at duty 0 so the one unavoidable arbitrary-phase jump happens with gates idle.
Related: [[wsync-coupling-network-numbers]].
