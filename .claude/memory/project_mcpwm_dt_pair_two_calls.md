---
name: mcpwm-dt-pair-two-calls
description: "IDF mcpwm_generator_set_dead_time configures one output path; asymmetric configs need a call per generator, not one"
metadata: 
  node_type: memory
  type: project
  originSessionId: d4ceb270-1324-43fd-9eb4-1d2cc8a41800
---

`mcpwm_generator_set_dead_time(in, out, cfg)` configures **one** of the
operator's two dead-time paths (path 0 = RED, path 1 = FED) and, in its
non-bypass branch, calls `swap_out_path` to route **one** output to it. A
single call leaves the OTHER output reading whatever path it happened to be
reading — so asymmetric configs (delay on one gen only, complementary pairs,
mode changes) silently mirror both pins from the same source.

**Always make two `set_dead_time` calls — one per generator.** Even when the
second would otherwise be a bypass: bypass calls skip `swap_out_path` AND undo
the previous call's bypass flag.

For HS-direct + LS-with-RED-delay (buck diode-emulation pattern), the working
sequence is:
```c
mcpwm_dead_time_config_t hs = {.posedge_delay_ticks = 0, .negedge_delay_ticks = 1};
mcpwm_dead_time_config_t ls = {.posedge_delay_ticks = dt, .negedge_delay_ticks = 0};
mcpwm_generator_set_dead_time(genHS, genHS, &hs);  // claim path 1 (FED) for HS
mcpwm_generator_set_dead_time(genLS, genLS, &ls);  // claim path 0 (RED) for LS
```

The 1-tick FED on HS is a ~6.25 ns workaround to force non-bypass mode so
swap_out_path runs. It adds 6.25 ns to HS-on, negligible in our application.

**Why:** the IDF docs (programming guide §Dead Time) don't say two calls are
mandatory. The signal is `mcpwm_gen.c`'s internal comment about needing the
TRM topology diagram and the fact that every test in `test_mcpwm_gen.c` does
two calls. We filed espressif/esp-idf#18654 asking for a doc note.

**How to apply:** any MCPWM driver code touching `set_dead_time` must mirror
this pattern. See `src/pwm/mcpwm.h::MCPWM_SyncLeg::init`. The on-target test
(`test_mcpwm_deadband_hs_to_ls` / `4b` / Test 10 linearity) catches a
regression on the next bench run.
