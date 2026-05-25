*this document is an LLM generated placeholder*

# PWM test implementation plan

Walks `doc/pwm-test-spec1.md` from the Rig self-test (already coded) to all
12 MCPWM driver tests. Ordered so each milestone reuses prior helpers and
delivers a runnable on-target check before moving on.

## State as of this plan

- Spec: `doc/pwm-test-spec1.md`
- Code: `test/test_pwm.cpp` (Rig-1 + Rig-2), wired in `main/CMakeLists.txt`
  and `test/main.cpp`
- Shared infra in place: `CapEvent` ring (single-channel), `cap_isr`,
  `CapRig` (LEDC + 1× MCPWM_CAP on same pin, INPUT_OUTPUT restoration)

## M1 — Bench verification of Rig-1/2

Goal: confirm the measurement plumbing the rest of the plan depends on.

- `RUN_TESTS=1 idf.py build flash monitor` on a bench board (not `fry`/`flat`).
- Confirm `PWM_TEST_PIN` (default 4) is free on the target board's `board.conf`;
  override with `-DPWM_TEST_PIN=<n>` if not.
- Expected: `test_pwm_rig_freq_path` PASS, `test_pwm_rig_pulsewidth_path` PASS
  for all three duties.
- If FAIL:
  - "CAP ring did not fill" → matrix route to `PWM0_CAPn_IN_IDX` wrong, OE
    not restored after CAP init, or wrong APB clock assumption.
  - Freq off by integer factor → prescaler engaged unexpectedly, or
    `MCPWM_CAPTURE_CLK_SRC_DEFAULT` resolves to something other than APB on
    this IDF version.
  - Duty mismatch → only one edge captured (check `flags.pos_edge` /
    `neg_edge`), or `keep_io_conf_at_exit` clobbering OE between tests.

Block downstream milestones until M1 passes.

## M2 — Multi-channel ring + `CapRig` extension

Goal: prep shared infra so M3+ doesn't churn helpers.

- Extend `CapEvent` with `uint8_t chan_id` (0/1). Update `cap_isr` to take
  channel index via `user_ctx`.
- Split `CapRig` into:
  - `CapTimer` — owns the MCPWM capture timer (one per group)
  - `CapChan` — owns one capture channel + its ISR routing; takes
    `CapTimer&`, `gpio_num`, `chan_id`, and the same OE-restore step
- Keep existing rig-test path working: `CapRig` becomes a thin wrapper that
  composes one of each.
- Ring head check stays single-producer: ISR is per-channel but writes the
  same ring; channel ID disambiguates.

Verification: re-run M1, both rig tests still PASS unchanged.

## M3 — Tests 1, 2, 3 (MCPWM HS-only)

Goal: first real MCPWM driver coverage, single-channel.

- New helper `McpwmLegRig` — owns `MCPWM_SyncLeg` + a `CapTimer` + one
  `CapChan` on HS pin. Pins read from `board.conf` if present, else fixed
  test pins.
- Test 1 (`test_mcpwm_hs_freq`): drive leg at fsw ∈ {20k, 39k, 100k}, reuse
  Rig-1's `(t[N-1]-t[0])/(N-1)` math. Tolerance ±5 Hz.
- Test 2 (`test_mcpwm_hs_duty`): reuse Rig-2's width+period code. Sweep
  D ∈ {0.1, 0.25, 0.5, 0.75, 0.9}.
- Test 3 (`test_mcpwm_pwmmax_arithmetic`): no pin needed; instantiate
  `MCPWM_SyncLeg`, call `init()`, assert `pwmMax == period_ticks − dtTicks`
  with the formulas in spec1 §Tests/3.

Files: append to `test/test_pwm.cpp`, declare in `test/main.cpp`.

## M4 — Tests 4a, 4b (HS↔LS dead-band)

Goal: shoot-through guard. Critical safety test.

- Extend `McpwmLegRig` with a second `CapChan` on LS pin (`io_loop_back=1`
  on both since these are MCPWM-driven, no LEDC mixing).
- Test 4a (`test_mcpwm_deadband_hs_to_ls`): pair (HS-fall, LS-rise) within
  the same period. Over 1024 periods: `mean ≥ dt − 30 ns`, **`min > 0`**.
- Test 4b (`test_mcpwm_deadband_ls_to_hs`): set `cmpLS = pwmMax − 1`
  explicitly (worst case). Pair (LS-fall[k], HS-rise[k+1]).
  `min ≥ (dtTicks + 1) ticks − 30 ns`.
- Safety: SD pin asserted (per spec1 §Safety gate). Add a small `assert_sd()`
  helper that reads `pwm_sd` + `pwm_sd_active_low` from `board.conf` and
  drives the SD pin to its inactive level; warns + continues on bench
  configs without `pwm_sd`.

Risk: pairing logic must walk the ring in time order and group by period
(use HS-rise as the period anchor). Off-by-one on which LS-rise belongs to
which HS-fall will silently inflate or hide the dead-band — write a tiny
host-side unit test for the pairing helper if it gets non-trivial.

## M5 — Tests 5, 6 (LS force off / on)

Goal: rectifier-disable paths.

- Test 5 (`test_mcpwm_ls_force_off`): `setLsOff(cmpHS)` then capture 100 ms;
  assert `g_head == 0` on the LS channel (filter ring by `chan_id`).
- Test 6 (`test_mcpwm_ls_force_on`): `setLsOff(pwmMax − 1)`; expect LS-high
  fraction > `(1 − (dtTicks+1)/period) − 0.001`.
- Reuses M4's two-channel `CapTimer/CapChan`.
- SD asserted (Test 6 worst case for live silicon).

## M6 — Tests 7, 8 (D=0, D=1)

Goal: forced-gate paths and `digitalRead` plumbing.

- Test 7 (`test_mcpwm_d0_hs_low`): call the driver's D=0 path
  (`forceShutdown()` or whatever maps from controller `update_pwm(0,0)`).
  Poll `digitalRead(pinHS)` 100× / 10 ms; PCNT count = 0 over 100 ms.
- Test 8 (`test_mcpwm_d1_hs_high`): mirror via
  `mcpwm_generator_set_force_level(genHS, 1, true)` (or the driver's D=1
  path). Same checks with expected level = 1.
- New helper: `pcnt_count_over(pin, duration_ms)` for the 0-edges
  cross-check. Adds PCNT unit setup (matrix-route from pin → PCNT channel,
  same OE-restore trick as the CAP path).

Risk: PCNT setup adds the same matrix-clobber issue — its `gpio_set_pull`/
`gpio_set_direction` calls will reset OE. Pattern is identical to CAP:
init PCNT after the leg, then `gpio_set_direction(INPUT_OUTPUT)`.

## M7 — Test 9 (TEZ-buffered glitch-free duty step)

Goal: regression guard on `update_cmp_on_tez = 1`.

- Test 9 (`test_mcpwm_glitch_free_duty_step`): sequence three
  `setHsOff(Di × pwmMax)` calls (D ∈ {0.2, 0.5, 0.8}) faster than one
  period. Capture ~64 periods; every observed `(t_rise, t_fall)` pair
  must have width in `{D1·pwmMax, D2·pwmMax, D3·pwmMax}` within ±1 tick.
- Implementation note: write all three duties with no delay between, then
  rearm capture, then wait for fill. The TEZ-buffered commit means we will
  see only the *third* duty if the writes happened within one period — that
  is the expected behaviour. To exercise the "writes spread across periods"
  case, interleave a controlled `esp_rom_delay_us(period_us + 2)` between
  the second and third write so we see at least D2 and D3 in the stream.
  Adjust acceptance to "all observed widths ∈ {Di · pwmMax}, at least 2
  distinct".

## M8 — Test 10 (dead-time linearity sweep)

Goal: confirms `pwm_deadtime_ns → dtTicks` conversion across the production
range.

- Test 10 (`test_mcpwm_deadtime_linearity`): for each
  dt ∈ {50, 100, 200, 500} ns, tear down + re-init `McpwmLegRig` with that
  conf value, re-run M4's Test-4a measurement, record mean dt.
- Acceptance: linear regression slope = 1.0 ±5 %, intercept ≤ 30 ns.
- Reuses M4 helpers entirely; this is mostly a test driver, not new infra.

## M9 — Test 11 (OST fault brake)

Goal: latched hardware shutdown.

- Need a "fault driver" GPIO routed to the fault input pin via matrix —
  no external wire, per spec1 §Tests/11. Use
  `esp_rom_gpio_connect_out_signal(fault_drive_pin, sig, false, false)`
  where `sig` is the GPIO output signal the fault pin reads from.
  Cleaner alternative: use `gpio_set_level(fault_drive_pin, 1)` if both
  pins are tied via internal matrix; the simplest is just to set the
  fault pin via `mcpwm_new_gpio_fault` and then `gpio_set_level` on a
  matrix-linked driver pin.
- Test 11 (`test_mcpwm_ost_brake_latches`): start leg switching, assert
  fault, within 1 µs both gate `digitalRead` = LOW, capture sees no further
  edges. Call `MCPWM_FaultBrake::recover()`; assert switching resumes.
- Skip with logged warning if `pwm_fault_pin` not in `board.conf`.

Risk: matrix-driven fault is fiddly. May fall back to a physical jumper as
acceptable cost; document the decision and note it in Round 2 if so.

## M10 — Test 12 (interleaved-leg phase)

Goal: validate `MCPWM_Converter<N>` phase sync.

- Need two `CapChan` on legs 0 and 1 HS, both `io_loop_back = 1`, same
  `CapTimer` so timestamps share one clock.
- Test 12 (`test_mcpwm_interleaved_phase`): for `N = 2` and `N = 3`,
  compute `Δt = t(HS-rise[leg1]) − t(HS-rise[leg0])`; expect `period/N`
  within ±25 ns over 1024 cycles.
- Capture-channel budget: each MCPWM group on S3 has 3 CAP channels —
  fine for N ≤ 3. N ≥ 4 needs cross-group capture; punt to Round 2 with a
  TEST_IGNORE.

## Cross-cutting: shared helpers to land progressively

| Helper                          | Introduced in | Used by              |
|---------------------------------|---------------|----------------------|
| `CapEvent` (with `chan_id`)     | M2            | all                  |
| `CapTimer` / `CapChan`          | M2            | M3+                  |
| `McpwmLegRig`                   | M3            | M3+                  |
| `assert_sd()` safety gate       | M4            | M4, M6 worst-case    |
| ring pairing (HS-rise anchored) | M4            | M4, M5, M9, M10      |
| `pcnt_count_over(pin, ms)`      | M6            | M6, M7 cross-check   |
| matrix-driven fault assert      | M9            | M9                   |

Keep these in `test/test_pwm.cpp` until the file exceeds ~600 lines; if it
does, split helpers into `test/test_pwm_helpers.h` (header-only,
single-TU-friendly per the `perf.h non-inline ODR` memory).

## What's intentionally **not** in this plan

- LEDC driver coverage beyond the rig — spec1 is MCPWM-only.
- The `vconv` and `mock` drivers — neither emits a physical signal.
- Round-2 items (sub-30 ns dead-time, gate slew, shoot-through current,
  EMI, brown-out, N ≥ 4 interleaving) — all explicitly scope-required.
- Host-stub variant of these tests — they need real MCPWM silicon; the
  host stub is for arithmetic-only tests.

## Stopping rule per milestone

Each milestone is "done" when:
1. Its tests pass on a bench board with no manual intervention.
2. The pass output names the values measured (e.g. "dt mean = 102.4 ns") so
   future regressions show *what* drifted, not just FAIL.
3. Re-running the full `RUN_TESTS=1` suite does not regress prior milestones
   or the non-PWM tests (`test_buck`, `test_charger`, etc.).
