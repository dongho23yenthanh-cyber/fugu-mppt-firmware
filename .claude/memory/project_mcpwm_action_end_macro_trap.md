---
name: mcpwm-action-end-macro-trap
description: "ESP-IDF MCPWM_GEN_*_EVENT_ACTION_END() expands to {} sentinel; trips -Werror=missing-field-initializers"
metadata: 
  node_type: memory
  type: project
  originSessionId: 4fc5792c-5b03-4ecc-b964-bc75839cce5a
---

ESP-IDF 5.5 `driver/mcpwm_gen.h` defines `MCPWM_GEN_{TIMER,COMPARE,BRAKE}_EVENT_ACTION_END()` as `(mcpwm_gen_*_event_action_t){}`. Using these with the variadic `mcpwm_generator_set_actions_on_*_event(gen, action, END())` triggers project-wide `-Werror=missing-field-initializers` because the `{}` leaves `direction` and `action` uninitialized in the sentinel.

**Why:** Standard IDF examples use the variadic form, but the macro design conflicts with the project's strict-init policy in `component_compile_options`. Won't be fixed upstream.

**How to apply:** Use the singular non-variadic variants instead: `mcpwm_generator_set_action_on_timer_event(gen, MCPWM_GEN_TIMER_EVENT_ACTION(...))` (and `_on_compare_event`, `_on_brake_event`). One action per call, no sentinel needed. See `src/pwm/mcpwm.h`. Related: [[newlib-nano-format]] is a similar IDF/toolchain gotcha.
