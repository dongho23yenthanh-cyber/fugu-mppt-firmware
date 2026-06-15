---
name: project_runtime_selectable_pwm_driver
description: LEDC+MCPWM both compile-in; pick at runtime via converter.conf::pwm_driver (default ledc). FUGU_WITH_LEDC Kconfig strips LEDC. OTA caveat for flat/fry.
metadata: 
  node_type: memory
  type: project
  originSessionId: b8752a87-6546-4fb2-9a24-94914d84e1f8
---

2026-06-15: `buck.h` now compiles BOTH gate drivers and selects at runtime — `converter.conf::pwm_driver`
= `ledc` | `mcpwm`, **default `ledc`** (an MCPWM board must opt in; `config/fmetal` set to mcpwm).
Two driver members + `useMcpwm` branch via private `drv*()` helpers (no virtual dispatch); single-driver
builds (`#if HAVE_MCPWM`/`HAVE_LEGACY`) fold to the old code. New Kconfig **`FUGU_WITH_LEDC`** (default y);
off strips libesp_driver_ledc + esp32-hal-ledc (~11 KB) and disables fan PWM + the `anaw` command (LEDC's
only other users; the live fan path is digitalWrite on/off, unaffected). Both-drivers image is ~+5 KB.

Hardware-validated on an esp32s3 bench: runtime select brings up ledc (pwmMax=2047) vs mcpwm (4103);
12 changed-area tests pass (test_buck + test_charger). Commits 068600c (gate) + 7666c6d (driver) + 169368f (tests).

**⚠ OTA caveat:** a both-drivers image defaults to LEDC. fry/flat run MCPWM — before OTAing such an image
to them, `set-config converter.conf pwm_driver mcpwm` on EACH (persists across app-OTA on littlefs) or they
boot the wrong driver on a live converter. Verify with get-config first.

Test-construction gotcha hit while writing the tests: [[project_conffile_inmem_multipair_miscompile]].
Charger float fix that motivated the session: [[project_charger_shared_bms_and_flat_vout_cal]] (vout_offset_max).
