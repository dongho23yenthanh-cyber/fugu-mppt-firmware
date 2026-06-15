---
name: mcpwm-force-level-latches
description: "mcpwm_generator_set_force_level(gen, 0, hold_on=true) LATCHES the gen output LOW until explicitly cleared"
metadata: 
  node_type: memory
  type: project
  originSessionId: 4fc5792c-5b03-4ecc-b964-bc75839cce5a
---

`mcpwm_generator_set_force_level(gen, level, hold_on=true)` writes a CONTINUOUS force that overrides all generator actions until you call `mcpwm_generator_set_force_level(gen, -1, hold_on=true)`. A timer running with valid comparators + actions will produce NO output on the pin while the force is latched -- registers all look correct (enable=1, sig=160, oen_sel=0, io_mux=0x1a02, timer counter advancing).

**Why:** The IDF function is the right primitive for a "safe state" hold, but the latch survives `mcpwm_timer_start_stop`, comparator updates, and action reconfigurations. Only `set_force_level(gen, -1, true)` clears it.

**How to apply:** Any code that calls `forceShutdown()` (or equivalent) must pair it with `clearForce()` on the next enable. In `src/buck.h::pwmPerturb`, the disabled->enabled transition under `WITH_MCPWM` calls `pwmDriver.clearForce()` for this reason. See also [[mcpwm-action-end-macro-trap]] for another MCPWM API trap on this project.

**Debugging signature:** if your MCPWM gates are flat at 0 V despite (a) timer counter advancing in `mcpwm_dump 0`, (b) `gpiodump` showing the right signal routed to the pin, and (c) `digitalWrite` driving the pin cleanly to 3.3 V, suspect a stale force-level. The `mcpwmtest` (or any minimal IDF example) command on the same pin will work because it never calls `set_force_level`.
