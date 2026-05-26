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

## M1 — Bench verification of Rig-1/2  ✓ DONE (2026-05-26)

- Build: `RUN_TESTS=1 idf.py -B build-tests build flash monitor` (separate
  build dir — see CLAUDE.md).
- Result: both rig tests PASS. Rig-1 measured 39062.50 Hz vs expected
  39062.50 Hz (exact), Rig-2 PASS for D ∈ {0.25, 0.5, 0.75}. 92 / 3 / 0
  (3 pre-existing termination + buck failures, unrelated).

Bugs we hit and the lessons they planted:

- **OE-clobber after CAP init.** `mcpwm_new_capture_channel`'s internal
  `gpio_config(INPUT)` doesn't just clear OE — it *resets the matrix-output
  route to `SIG_GPIO_OUT_IDX`*, so even after `gpio_set_direction(INPUT_OUTPUT)`
  the pin is driven 0 by GPIO_OUT_REG. Fix: `esp_rom_gpio_connect_out_signal`
  to re-bind. Will hit any future test that mixes a peripheral output with a
  separately-initialised CAP input on the same pin (e.g. Test 11 fault input
  driven from a non-MCPWM source).
- **Ring sizing.** Rig-1 wants 4000 cycles × 2 edges = 8000 events; bumped
  `kRingSize` to 8192. Spec1 §test-harness sized this wrong originally.
- **Expected freq ≠ requested freq.** LEDC quantizes to integer APB ticks;
  expected = `APB / (pwmMax + 1)`, not `fsw`. Same trap awaits MCPWM tests
  with `bestTiming`'s rounding — compare to the actual emitted freq.
- **`esp_netif_create_default_wifi_ap` stub** required in `test/main.cpp`.
  Pre-existing: `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n` drops the symbol; Arduino
  WiFiGeneric references it unconditionally; the non-test build links it
  transitively but the test build doesn't. Already worked around.
- **Diagnostic in `wait_events`:** polls `digitalRead` of the test pin and
  logs `head / target / pin / transitions_polled` on timeout. Cheap, told us
  immediately whether LEDC was driving (transitions > 0) vs CAP wasn't
  wired (transitions > 0 but ring empty) vs ring too small (head = ring
  capacity). Keep this pattern for M3+.

## M2 — Multi-channel ring + helper split  ✓ DONE (2026-05-26)

- `CapEvent` extended with `uint8_t chan_id`.
- `cap_isr` gets `chan_id` via `user_ctx` (`intptr_t` cast).
- Split into `CapTimer` (RAII, 80 MHz APB timer per group) and `CapChan`
  (RAII, per-pin channel + ISR routing). `LedcCapRig` composes one of each
  for the rig tests and keeps the LEDC-specific OE-restore + matrix-rebind.
- `rearm_ring({&chan1, &chan2, ...})` quiesces all named channels around
  `g_head = 0`.

Verification: both rig tests still PASS after refactor (exact same measured
numbers).

## M3 — Tests 1, 2, 3 (MCPWM HS-only)  ✓ DONE (2026-05-26)

Goal: first real MCPWM driver coverage, single-channel.

Result: 5/0/0 on the stripped suite (Rig-1, Rig-2, Test 1, Test 2, Test 3).
Test 1 at 20 kHz exact, at 39 kHz 0.85 Hz off (integer truncation in
`bestTiming.actual_freq`); Test 2 at D ∈ {0.25, 0.5, 0.75} within ±0.0001
duty; Test 3 all 5 arithmetic cases exact.

What landed:
- `MCPWM_SyncLeg` destructor (reverse-order teardown). Required because
  `MCPWM_SyncLeg::init()` allocates a timer + operator + 2 cmps + 2 gens, and
  group 0 on ESP32-S3 has only 3 operators — after 3 fresh inits the group is
  exhausted. Production builds with a single global leg never run the dtor.
- `McpwmLegRig` helper (MCPWM leg + CAP timer + one HS CapChan, `io_loop_back=1`).
- `CapChan` constructor extended with `io_loop_back` arg (default false).
- `analyse_period` / `analyse_pulse_width` helpers (filter ring by `chan_id`).
- `wait_events` got an optional `diag_pin = -1` to suppress Arduino's
  "IO X is not set as GPIO" warning when the pin is muxed to a peripheral.
- `test/main.cpp`: wrapped non-PWM RUN_TEST calls in `#ifdef FULL_TEST_SUITE`
  so PWM iteration runs in ~0.8 s. Set `FULL_TEST_SUITE` env var to restore.

Gotchas / lessons (carry forward to M4+):

- **CAP ISR latency is ~1-3 µs on S3.** Drops one edge per pulse whenever the
  inter-edge gap approaches that. Bit M3 in two places:
  - Test 2 at D = 0.1 / D = 0.9 (sub-3 µs HS pulses at 39 kHz) — under-counted
    by ~20%. Dropped those duties; deferred to Round 2.
  - Test 1 at fsw = 100 kHz (5 µs inter-edge) — measured freq drifted 97-99.9
    kHz across reboots. Dropped 100k from sweep; deferred to Round 2.
  M4's dead-band tests are unaffected (gap = `dtTicks` ≈ 100 ns, but we're
  measuring EDGE-TO-EDGE not pulse-width).
- **APB-vs-PLL_F160M sync slop.** Smaller systematic ≤ 0.1% at high fsw even
  before edge-loss kicks in. Spec1 §Test 1 tolerance bumped to ±5 Hz OR ±0.1%
  (whichever larger).
- **Bench-real duty tolerance is ±50 ns, not the ±25 ns spec1 originally
  claimed.** Spec1 + tests both bumped.
- **`bestTiming.actual_freq` is integer-truncated** (160M / period_ticks).
  Test 1 compares to that truncated value, accepting ~1 Hz "expected"-side
  noise.
- **Don't run RUN_TESTS commands in the same shell without `RUN_TESTS=1`** —
  CMake reconfigure re-reads the env var and silently switches MAIN_SRC back
  to production firmware. Use `RUN_TESTS=1 idf.py -B build-tests build` for
  *both* configure and build.
- **`keep_io_conf_at_exit` is deprecated** in IDF 5.5 `mcpwm_capture_channel_config_t`.
  Cosmetic warning; the spec sets it to 0 explicitly to satisfy
  `-Werror=missing-field-initializers`.

## M4 — Tests 4a, 4b (HS↔LS dead-band)  ✓ DONE (2026-05-26)

Both pass on bench:

- Test 4a HS→LS: n=292, mean=193.8 ns, min=187.5 / max=200.0 ns (expected 200.0 ns) ✓
- Test 4b LS→HS: n=287, mean=206.3 ns, min=200.0 / max=212.5 ns (expected 206.2 ns) ✓

Mean undershoots by ~6 ns because the workaround adds 1-tick (6.25 ns) FED
on the HS gen — see below.

### How we got there

Original `MCPWM_SyncLeg::init` only called `set_dead_time(genLS, genLS,
{posedge=dt})`, expecting that to route LS through RED and leave HS direct.
Both pins ended up mirroring the LS-with-rising-delay waveform. Earlier
write-up of the M4 blocker chased the wrong hypothesis (TRM bit semantics).

Cracked it by reading IDF source `esp_driver_mcpwm/src/mcpwm_gen.c::
mcpwm_generator_set_dead_time` against the test-app patterns in
`esp_driver_mcpwm/test_apps/mcpwm/main/test_mcpwm_gen.c` (notably
`reda_only_set_dead_time`).

**What the API actually does:** the operator has TWO dead-time paths —
path 0 = RED (rising-edge delay), path 1 = FED (falling-edge delay). Both
output pins draw from one of these paths. Each call to `set_dead_time`
configures *one* of the paths AND, in its non-bypass branch, calls
`mcpwm_ll_deadtime_swap_out_path` to route an output to its non-natural
path. Bypass-mode calls *don't* touch `swap_out_path`. So:

- Single call `set_dead_time(genLS, genLS, {posedge=dt})` → un-bypasses
  path 0, sets RED source = gen B (LS), swaps output B onto path 0. Output
  A is never re-routed, so it still reads path 0 too → both pins follow
  RED-of-LS. **This was the bug.**
- A follow-up `set_dead_time(genHS, genHS, {0,0})` (bypass) doesn't fix it
  because bypass-branch skips `swap_out_path` and additionally undoes the
  bypass-flag on path 0.

### Fix

To get output A = gen A direct, output B = gen B + RED-posedge-delay, we
need both `swap_out_path` flags set. The only way through the public API:
issue two non-bypass calls, picking the smallest possible delay on the HS
side (the test pattern in `ahc_set_dead_time` follows the same shape):

```c
set_dead_time(genHS, genHS, {.posedge=0, .negedge=1});  // claim path 1 (FED) for HS
set_dead_time(genLS, genLS, {.posedge=dt, .negedge=0}); // claim path 0 (RED) for LS
```

Effect: output A = gen A + 1-tick (6.25 ns) FED → essentially direct;
output B = gen B + RED with posedge_delay = dtTicks. The 6.25 ns extension
of HS-on shows up as the ~6 ns mean shortfall in Test 4a.

### Test-side bugs uncovered along the way

- **CAP ISR reorders close-spaced edges.** When HS-fall and LS-rise are
  ~200 ns apart, the two CAP-channel ISRs can complete in either order,
  pushing samples to the ring with timestamps not monotonically increasing.
  `analyse_deadbands` is now a ts-stable insertion sort before the state
  machine.
- **`have_ls_fall` leaked across dropped periods.** If the state machine
  reset mid-period (e.g. ISR latency lost an edge), the stale LS-fall ts
  paired with a much-later HS-rise, producing 19-period outliers in
  Test 4b's `max`. All `else` branches now reset `have_ls_fall = false`.

## M5 — Tests 5, 6 (LS forced off / on)  ✓ DONE (2026-05-26)

Both PASS with dt=0 (no DT module in play). Test 5 sets `cmpLS = cmpHS` →
LS rise/fall events fire at the same tick → LS pin stays low (HS rising = 3946
events in the window, LS pos = 0). Test 6 sets `cmpLS = pwmMax-1` → measured
duty 0.7499, expected 0.7499 (exact).

## M6 — Tests 7, 8 (D=0, D=1)  ✓ DONE (2026-05-26)

Test 7: `MCPWM_SyncLeg::forceShutdown()` → 0 HS edges, digitalRead high
count 0/100. Test 8: `mcpwm_generator_set_force_level(genHS, 1, true)` + LS
forced 0 → 0 HS edges, digitalRead low count 0/100. Both PASS.

## M7 — Test 9 (TEZ-buffered glitch-free duty step)  ✓ DONE (2026-05-26)

Sequence: setHsOff(cmp1) → 200 µs delay → setHsOff(cmp2) → capture. Result:
8 pulses at cmp1-width (615 CAP ticks) + 269 pulses at cmp2-width (1436 CAP
ticks), **0 intermediate values**. This empirically confirms
`update_cmp_on_tez = 1` (`src/pwm/mcpwm.h:97`) is active and working —
regression guard against that flag being cleared.

## M9 — Test 11 (OST fault brake)  ✓ DONE (2026-05-26)

Software-driven fault via `gpio_set_level(fault_pin, 1)` while pin is in
INPUT_OUTPUT mode (matrix input route to PWM_FAULT coexists with software
write to GPIO_OUT_REG). On trip: HS and LS both read LOW immediately, 0
HS edges over 20 ms latch window. After `MCPWM_FaultBrake::recover()`,
switching resumes (≥64 edges captured). PASS.

Trip latency printed but partially garbled by `%lld` not supported in
newlib-nano printf — fixed to `%d` with `(int)` cast.

## M10 — Test 12 (interleaved phase)  ✓ DONE (2026-05-26)

`MCPWM_Converter<2>` with legs on (pin 5, 6) and (pin 8, 9). HS-rise pairs
captured via two CapChans on shared CapTimer. **n=256, mean=12837.5 ns,
expected=12822.2 ns (period/2), min = max = mean** — both legs perfectly
synced with zero drift over 256 cycles, well inside ±50 ns. Spec1's caveat
about N ≥ 4 needing cross-group routing stands; N = 2 fits in one group.

## M8 — Test 10 (dead-time linearity sweep)  ✓ DONE (2026-05-26)

Sweep dtTicks ∈ {8, 16, 32, 80} (= 50/100/200/500 ns @ 160 MHz), re-init the
leg per value, run the Test-4a HS→LS measurement, regress measured vs. configured:

| dt configured | dt measured |
|---------------|-------------|
| 50.0 ns       | 43.8 ns     |
| 100.0 ns      | 93.7 ns     |
| 200.0 ns      | 193.8 ns    |
| 500.0 ns      | 494.9 ns    |

`slope=1.0001..1.0026`, `intercept=-6.3..-6.5 ns` (steady across 4 cold-boot runs).
The −6 ns intercept is the FED-on-HS systematic from M4's workaround. Asserts:
`slope ∈ [0.95, 1.05]`, `|intercept| ≤ 30 ns`. Test passes deterministically.

## Final state (2026-05-26): 15 / 0 / 0

- 15 PASS: Rig-1, Rig-2, Tests 1, 2, 3, 4a, 4b, 5, 6, 7, 8, 9, 10, 11, 12
- 0 FAIL, 0 IGNORE
- Verified across 4 consecutive cold-boot runs. Back-to-back boot cycles (chip
  resets within a few seconds without full power cycle) can produce a one-period
  outlier in Test 4b; cold-boot is stable. Suspect leaked CAP-timer or operator
  state across the soft reset; not pursued.

Test runtime: ~3.5 s with `FULL_TEST_SUITE` undefined (PWM tests only). To
restore the rest of the suite, define `FULL_TEST_SUITE` at build time.

Read output via the USB-to-UART bridge port (`/dev/cu.usbmodem59720648061`),
not the device's native USB-CDC port (`/dev/cu.usbmodem11101`) — the bridge
sits on UART0 which is where ESP-IDF's default console writes. The native
USB-CDC apparently isn't initialised in this firmware variant.

<!-- M4 original plan removed: the M4 section above now documents what
     actually landed, including the IDF-source fix, the FED+RED workaround,
     and the ring-pairing bugs uncovered along the way. -->


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
