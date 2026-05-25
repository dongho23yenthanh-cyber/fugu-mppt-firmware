*this document is an LLM generated placeholder*

# PWM driver test spec — driver-agnostic, zero-hardware

Scope: validate any class implementing the PWM-driver surface in
`src/pwm/vconv.h` (`init_pwm(ch, pin, freq)`, `update_pwm(ch, duty)`,
`update_pwm(ch, hpoint, duty)`, `stop(ch, idleLevel)`, `pwmMax` member),
using only ESP32-S3 self-observation — no scope, no PicoScope, no signal
generator, **no physical jumpers**.

Targets `src/pwm/ledc.h` (LEDC, currently in production), `src/pwm/mcpwm.h`
(MCPWM HiLi + InEn, in development), and `src/pwm/mock.h`. References:
`doc/mcpwm-sync-buck-driver.md` for the MCPWM design (incl. `update_cmp_on_tez = 1`
confirmed in source).

Round 2 (external instrumentation) is enumerated at the end and is **not** in
scope here.

## Driver capability matrix

Each driver advertises a small set of caps; tests are gated on them and either
run or skip-with-reason at runtime.

| Cap                       | LEDC | MCPWM HiLi | MCPWM InEn | mock |
|---------------------------|:----:|:----------:|:----------:|:----:|
| `pwm_drives_pin`          |  Y   |     Y      |     Y      |  N   |
| `has_two_independent_ch`  |  Y   |  Y (HS+LS) |  Y (IN+EN) |  Y   |
| `is_complementary`        |  N   |     Y      |     Y      |  N   |
| `has_hw_deadtime`         |  N   |     Y      |     N*     |  N   |
| `has_hw_brake_ost`        |  N   |     Y      |     Y      |  N   |
| `has_glitch_free_update`  | N**  |     Y      |     Y      |  Y   |
| `has_hpoint`              |  Y   |     N      |     N      |  Y   |
| `period_ticks_is_2^n−1`   |  Y   |     N      |     N      |  Y   |

\* InEn delegates dead-time to the external half-bridge driver chip — the MCPWM
side just emits IN and an EN window.
\** LEDC `ledc_update_duty` latches at the next overflow but two independent
channels update independently — there is no atomic *paired* update across HS
and LS; the dead-band between HS-off and LS-on is whatever margin the
controller (`buck.h`) leaves between `cmpHS` and `cmpLS`.

Test-to-driver applicability is derived from the cap matrix; the per-test
"applies to" line below restates it for convenience.

| #   | Test                                | LEDC | MCPWM HiLi | MCPWM InEn | mock |
|-----|-------------------------------------|:----:|:----------:|:----------:|:----:|
| 1   | HS frequency = `pwm_freq`           |  Y   |     Y      |     Y      |  N   |
| 2   | HS duty matches commanded count     |  Y   |     Y      |     Y      |  N   |
| 3   | `pwmMax` math vs source-clock       |  Y   |     Y      |     Y      |  Y   |
| 4a  | HS→LS dead-band (mid-period)        |  N***|     Y      |     N      |  N   |
| 4b  | LS→HS dead-band (period wrap)       |  N***|     Y      |     N      |  N   |
| 5   | LS forced off (cmpLS = cmpHS)       |  Y   |     Y      |     N      |  N   |
| 6   | LS forced on (sync-rect saturated)  |  Y   |     Y      |     N      |  N   |
| 7   | D = 0 → HS fully low                |  Y   |     Y      |     Y      |  N   |
| 8   | D = 1 → HS fully high               |  Y   |     Y      |     Y      |  N   |
| 9   | Glitch-free duty step (atomic)      |  N   |     Y      |     Y      |  Y   |
| 10  | Dead-time setting actually applied  |  N   |     Y      |     N      |  N   |
| 11  | OST fault brake latches gates LOW   |  N   |     Y      |     Y      |  N   |
| 12  | `stop()` honours `idleLevel`        |  Y   |     Y      |     Y      |  Y   |
| 13  | Glitch-free HS step on LEDC         |  Y   |     N      |     N      |  N   |

\*** LEDC has no concept of an HS↔LS pair the *driver* enforces — the dead-band
test for an LEDC-backed buck belongs in a controller test (`test_buck.cpp`),
not in this PWM-driver spec.

## Measurement primitives — internal GPIO matrix loopback

Zero external hardware. The ESP32-S3 GPIO matrix lets any GPIO drive an output
*and* feed its level to any peripheral input simultaneously.

**Path A — MCPWM-driven pin, MCPWM_CAP same-pin loopback.** The capture-channel
config flag `mcpwm_capture_channel_config_t::flags.io_loop_back = 1` (see IDF
`driver/mcpwm_cap.h` — *"For debug/test, the signal output from the GPIO will
be fed to the input path as well"*) shorts the pin's output into the CAP input
internally. 12.5 ns timestamps, no rewire.

**Path B — any-pin output, MCPWM_CAP cross-pin loopback.** Both `gpio_num` in
the capture-channel config and the underlying matrix routing accept any
input-capable GPIO; on S3 the three capture inputs per group
(`PWM0_CAP{0,1,2}_IN_IDX = 166..168`) are matrix-routable. So an **LEDC**-driven
pin can be timestamped by MCPWM_CAP at 12.5 ns without a jumper — declare the
LEDC pin output, then create a CAP channel naming the same pin (the matrix
routes both directions).

**Path C — PCNT for edge-counting any pin.** `PCNT_SIG_CH*_IN*_IDX` signals
are matrix-routable; LEDC's HS pin can be fed to PCNT directly.

**Path D — GPIO ISR for boolean / presence tests.** Set the PWM output pin
to `GPIO_MODE_INPUT_OUTPUT`, attach an interrupt. Useful for D=0 / D=1 / brake
latch checks; **not** for accurate duty (ISR latency 1–3 µs at 156 kHz total
edge rate eats a core and silently loses edges on any non-IRAM preemption).

**Path E — `digitalRead()` for static-level assertions.** Same `INPUT_OUTPUT`
mode; sufficient for D=0 / D=1 / `stop(..., idleLevel)`.

### Resolution and noise floor

- MCPWM capture timer (S3): APB-only source (`MCPWM_CAPTURE_CLK_SRC_APB`,
  confirmed in `soc/clk_tree_defs.h`) → **80 MHz, 12.5 ns/tick**, 32-bit
  free-running counter (~53.7 s wrap), 3 channels per group sharing one
  timer (zero inter-channel offset).
  - Unit check: 80 MHz × 12.5 ns = 1. OK.
- PCNT: APB-clocked; comfortably counts ≫ 1 MHz. Edge-count over a 1.000 s
  gate gives ±1 count → **~26 ppm at 39 kHz**.
- GPIO ISR: ~1–3 µs jitter; safe up to ~200 kHz edge rate before loss; presence
  only.
- `digitalRead`: ~µs polling; steady-state assertions only.

Internal-loopback systematic offset: the GPIO matrix adds a small propagation
delay (~5–10 ns) on every captured edge. It is *common-mode* across all edges
on the same pin and **cancels** for pulse-width and dead-band measurements
(differences). It only matters for absolute-phase comparisons across different
pins, where the prior ±30 ns tolerance already swallows it. Repeat-and-average
over N cycles knocks the random component down by √N; 1024 cycles at 39 kHz
(≈ 26 ms) gives a ~0.4 ns std-error on means.

| Path                 | Time resolution | Use for                                  |
|----------------------|-----------------|------------------------------------------|
| MCPWM_CAP same-pin   | 12.5 ns         | freq, duty, dead-band on MCPWM-driven    |
| MCPWM_CAP cross-pin  | 12.5 ns         | freq, duty on LEDC pin                   |
| PCNT                 | n/a (count)     | edge-count over gated window             |
| GPIO ISR             | ~1–3 µs jitter  | presence / polarity / brake-latch        |
| `digitalRead`        | ~µs polling     | D=0, D=1, `stop(..., idleLevel)`         |

## Core invariants — apply to every driver that owns a real pin

### 1. HS frequency = configured `pwm_freq`

- **Applies to:** LEDC, MCPWM HiLi, MCPWM InEn.
- **Method.** PCNT counts HS rising edges over a 1.000 s wallclock window
  (`esp_timer_get_time`). Expected count = `pwm_freq`; tolerance ±2 counts.
  Cross-check via MCPWM_CAP timestamps: `(t_rise[N−1] − t_rise[0]) / (N−1)` with
  N=4000 (≈103 ms at 39 kHz). Both methods agree within ±5 Hz.
- **Why this tolerance.** Both LEDC's `ledc_find_suitable_duty_resolution` and
  MCPWM's `bestTiming` round `period_ticks` to integer, so the actual frequency
  can differ from the requested by `0.5 · src_clk / period_ticks²`. At 39 kHz
  with 12-bit duty that is ~10 Hz — the ±5 Hz cross-check tolerance is on the
  *measurement*, not on the rounding error itself (which is captured by Test 3).
- **Sweep.** `fsw ∈ {20 kHz, 39 kHz, 100 kHz}`.

### 2. HS duty matches commanded count (within quantization)

- **Applies to:** LEDC, MCPWM HiLi, MCPWM InEn.
- **Method.** Call `update_pwm(0, D × pwmMax)` for D ∈ {0.1, 0.25, 0.5, 0.75, 0.9}.
  MCPWM_CAP both edges; over N=1024 periods compute `mean(t_fall − t_rise)`.
  Compare to `(D × pwmMax) / actual_freq` (LEDC) or `(D × pwmMax) / resolution_hz`
  (MCPWM).
- **Tolerance.** ±1 APB tick (12.5 ns) + ±1 timer tick (≤ 12.5 ns on MCPWM,
  much smaller on LEDC) ≈ ±25 ns. At 39 kHz that's ±0.001 duty-fraction.
- **Failure modes guarded.** Off-by-one in `pwmMax` after dead-time reservation;
  wrong source-clock assumption (e.g. APB vs PLL_F160M); the LEDC `duty`
  semantics being a duration count, not a fraction.

### 3. `pwmMax` matches the documented source-clock arithmetic

- **Applies to:** LEDC, MCPWM HiLi, MCPWM InEn, mock.
- **Method.** Pure host-side / on-target arithmetic, no edges needed. Call
  `init_pwm(0, pin, fsw)`, then assert `pwmMax` matches the per-driver formula:
  - **LEDC / mock:** `pwmMax = (2 << (resolution − 1)) − 1`, with
    `resolution = min(log2(80 MHz / fsw), 14)`. For 39 kHz → `div = 2051` →
    `resolution = 11` → `pwmMax = 2047`.
  - **MCPWM (HiLi):** `pwmMax = period_ticks − dtTicks` where
    `period_ticks ≈ round(resolution_hz / fsw)`, `resolution_hz = 160 MHz / presc`,
    `dtTicks = round(pwm_deadtime_ns × resolution_hz × 1e−9)`. For 39 kHz with
    `dt=100 ns`: `presc=1, period_ticks=4103, dtTicks=16` → `pwmMax = 4087`.
  - **MCPWM (InEn):** same, with `dtTicks = 0`.
- **Failure modes guarded.** Silent prescaler engagement, wrong source clock
  (160 MHz vs 80 MHz), `dtTicks` rounding with the wrong base (must use
  `resolution_hz`, not `pwm_freq × period_ticks`).

### 4. HS↔LS dead-band (safety-critical for HiLi)

- **Applies to:** MCPWM HiLi only. **Skip** for LEDC (driver owns no LS
  relationship — controller responsibility), MCPWM InEn (chip-internal DT),
  mock.

#### 4a. HS → LS (mid-period)

- **Method.** Two MCPWM_CAP channels with `io_loop_back = 1`, one on each
  gate pin. Both edges enabled. Within one period:
  `dt_HSLS = t(LS-rise) − t(HS-fall)`.
- **Acceptance.** Over 1024 periods: `mean ≥ pwm_deadtime_ns − 30 ns`,
  `min > 0`. `mean` tolerance ±25 ns. Any sample with `min ≤ 0` (LS rises
  before HS falls) is an **immediate FAIL** — that's shoot-through.

#### 4b. LS → HS (period wrap)

- **Method.** `dt_LSHS = t(HS-rise[k+1]) − t(LS-fall[k])`. The driver reserves
  this in software by shaving `dtTicks` off `pwmMax`, so the worst case is at
  commanded `cmpLS = pwmMax − 1`; set that explicitly.
- **Acceptance.** `min(dt_LSHS) ≥ (dtTicks + 1) ticks − 30 ns`.
- **Why both transitions must be tested.** The MCPWM operator has **one shared
  dead-time submodule** (`mcpwm_dead_time_config_t` posedge / negedge on the
  same instance). The driver delays the LS posedge only and reserves the wrap
  in software; getting just one direction right is not enough.

### 5. LS forced off (sync rect disabled)

- **Applies to:** LEDC, MCPWM HiLi. Skip MCPWM InEn (LS is the chip-driven EN
  window — driver doesn't independently set it).
- **Method.** Call `update_pwm(1, cmpHS)` (HiLi) or equivalent LEDC-side path
  that zeroes the LS on-time. MCPWM_CAP on LS pin: **0 rising edges in 100 ms**.
  PCNT cross-check: count = 0.

### 6. LS forced on (sync rect saturated)

- **Applies to:** LEDC, MCPWM HiLi. Skip MCPWM InEn.
- **Method.** Drive `update_pwm(1, pwmMax − 1)`. Expect LS-high for almost the
  full period (less the LS→HS dead-band on HiLi). Measure with MCPWM_CAP:
  `mean(t_fall − t_rise on LS) > (1 − (dtTicks + 1)/period) − 0.001` of one
  period.
- **Safety.** Half-bridge driver SD pin asserted before running this — see
  Safety Gate.

### 7. D = 0 → HS fully low

- **Applies to:** LEDC, MCPWM HiLi, MCPWM InEn.
- **Method.** Per `doc/mcpwm-sync-buck-driver.md`, "D = 0 and D = 1 are reached
  by forcing both gates, not by setting `cmpHS = 0`" (the latter creates a
  one-tick glitch). The driver-agnostic path is `update_pwm(0, 0)` or the
  driver's stop / force path. Verify with `digitalRead(pinHS)` polled 100× over
  10 ms: all reads = 0. Cross-check with PCNT: count = 0 over 100 ms.

### 8. D = 1 → HS fully high

- **Applies to:** LEDC, MCPWM HiLi, MCPWM InEn.
- **Method.** Mirror of #7. `update_pwm(0, pwmMax)` (LEDC) or the driver's
  force-high path (MCPWM). `digitalRead(pinHS) == 1` across 100 polls; PCNT
  count = 0 (no edges); MCPWM_CAP: zero events.

### 9. Glitch-free duty step (atomic update at period boundary)

- **Applies to:** MCPWM HiLi, MCPWM InEn, mock. Skip LEDC.
- **Method.** Sequence three writes faster than one period:
  `update_pwm(0, D1·pwmMax)` → `update_pwm(0, D2·pwmMax)` →
  `update_pwm(0, D3·pwmMax)`, where each D is well-separated (e.g. 0.2 / 0.5 /
  0.8). MCPWM_CAP timestamps every HS rising and falling edge for ~64 periods.
- **Acceptance.** Every observed `(t_rise, t_fall)` pair has
  `t_fall − t_rise ∈ {D1·pwmMax, D2·pwmMax, D3·pwmMax}` (within ±1 tick capture
  noise) — **no intermediate values, no missed periods**.
- **Why this matters.** `src/pwm/mcpwm.h:97` sets
  `flags.update_cmp_on_tez = 1` — comparator writes are double-buffered and
  commit atomically at the next TEZ. This test is the empirical regression
  guard against that flag being cleared (which would silently re-introduce the
  LEDC-style ordering dance and the cmpLS < cmpHS race during decreases).

### 10. Dead-time linearity in `pwm_deadtime_ns`

- **Applies to:** MCPWM HiLi only.
- **Method.** Sweep `pwm_deadtime_ns ∈ {50, 100, 200, 500} ns` (in-memory
  `ConfFile` swap per the existing `test_buck.cpp` pattern), re-`init()` the
  leg, re-run Test 4a. Measured mean dead-time scales linearly with the conf
  value: slope = 1.0 within ±5 %, intercept ≤ 30 ns.
- **Coverage caveat.** `bestTiming()` runs with `presc = 1` up to
  `fsw ≤ 160 MHz / 65535 ≈ 2.44 kHz`; well below that and the test runs in the
  same regime as production. Prescaler-engaged dead-time rounding (very high
  fsw) is Round 2.

### 11. OST fault brake latches both gates LOW

- **Applies to:** MCPWM HiLi, MCPWM InEn. Skip LEDC, mock.
- **Method.** Configure `pwm_fault_pin` to a free GPIO. Drive that GPIO **from
  another GPIO routed via the internal matrix** to its active level — no
  external jumper, just `gpio_matrix_out` from a software-controlled GPIO into
  the fault input signal. Within one APB cycle of fault assertion: both gate
  pins read LOW; MCPWM_CAP records no further edges; PCNT count freezes. Call
  `MCPWM_FaultBrake::recover(leg.oper())` and confirm switching resumes.
- **Skip** if `pwm_fault_pin` is not configured, with a logged warning.

### 12. `stop(channel, idleLevel)` honours the idle level

- **Applies to:** LEDC (native), MCPWM (mapped to `forceShutdown` + force-level
  for the requested idle), mock.
- **Method.** Run the channel, call `stop(0, 0)` then `digitalRead(pin) == 0`;
  re-init, run, call `stop(0, 1)` then `digitalRead(pin) == 1`. 100 polls each,
  no edges via MCPWM_CAP / PCNT for 100 ms after the call.
- **Why this is its own test.** LEDC's `ledc_stop` documents the idle level
  contract; MCPWM has no native equivalent and the driver shim must implement
  it. A driver swap that silently drops this contract bricks any code relying
  on a known-safe idle level (e.g. PD-controller bring-up).

### 13. Glitch-free HS step on LEDC

- **Applies to:** LEDC only.
- **Method.** `ledc_update_duty` latches at the next overflow. Same sequence
  as Test 9 but only the HS channel. Acceptance: same — no intermediate
  pulse widths. Failure here means the LEDC driver code is bypassing
  `ledc_update_duty` or is mis-ordering set/update calls.

## Test harness (Unity / `RUN_TESTS=1`)

Tests live in `test/test_pwm_drivers.cpp`. Build via
`RUN_TESTS=1 idf.py build flash monitor`. Per the existing pattern in
`test/test_buck.cpp`, drive each driver's conf via in-memory
`std::unordered_map<std::string,std::string>` so each test can re-init with
different parameters.

Skeleton:

```
template <class Driver>
struct DriverCaps {
    static constexpr bool pwm_drives_pin;
    static constexpr bool is_complementary;
    static constexpr bool has_hw_deadtime;
    static constexpr bool has_hw_brake_ost;
    static constexpr bool has_glitch_free_update;
    static constexpr bool has_hpoint;
};
// Specialise for PWM_ESP32_ledc, MCPWM_SyncLeg (two: HiLi, InEn), PWM_Mock.
```

Per test:

1. `TEST_SKIP_IF(!DriverCaps<D>::has_<cap>);` at the top.
2. Build the conf map (`pwm_freq`, `pwm_driver_logic`, gate pins from board's
   `board.conf`, `pwm_deadtime_ns`, ...).
3. Instantiate the driver and call `init_pwm(...)` (LEDC/mock) or `init(...)`
   (MCPWM). Capture `pwmMax`.
4. Set up MCPWM_CAP channels on the relevant pin(s) **after** the driver is
   running:
   - For MCPWM-driven pins: `flags.io_loop_back = 1`, `gpio_num = pinHS/pinLS`.
   - For LEDC-driven pins: `flags.io_loop_back = 0`, `gpio_num = pinHS`; the
     matrix already routes the LEDC output, and the CAP input attaches via
     `esp_rom_gpio_connect_in_signal(pin, PWM0_CAP{n}_IN_IDX, false)`.
   - Both edges enabled (`pos_edge = 1`, `neg_edge = 1`).
5. ISR pushes `{timestamp_apb_ticks, channel, edge}` into a lock-free ring
   (size 4096; ≥1024 cycles × 4 edges).
6. Drive duty / stop / etc. as the test requires; wait until ring fills
   (≤100 ms at 39 kHz).
7. Stop capture, analyse the ring on core 0 (not in ISR), assert with
   `TEST_ASSERT_*`.
8. Teardown: `forceShutdown()` (MCPWM) or `stop(ch, 0)` (LEDC); delete capture
   channels and timer; clear the GPIO matrix routing.

**RT-path discipline.** Tests run from the Unity setup task on core 0 — not on
the RT loop — so `vTaskDelay` / `esp_timer_get_time` waits are fine. The MCPWM
operator + its capture ISR run on hardware independently. No exceptions cross
the capture ISR boundary; `ESP_ERROR_CHECK` in init is OK at test-setup time
only. ISR callback marked `IRAM_ATTR`.

## Safety gate (run before every test)

1. Refuse to flash unless the operator confirms the target. Hard-code refusal
   for the `fry` / `flat` hostnames in test/main.cpp (they drive live solar
   converters — see CLAUDE.md).
2. Assert the half-bridge driver SD pin (`pwm_sd` if configured) to disable
   the FETs **before** the test pokes any gate. With SD asserted, the driver
   IC ignores the gate signals — the test exercises the MCU peripheral, not
   the silicon. Bench / `dry_*` configs without `pwm_sd` are allowed to run
   without this step but should log a warning.
3. If a SD pin exists but is asserted-low (driver enable held high), the test
   harness needs to know the polarity — read from `board.conf::pwm_sd_active_low`.

## Pass / fail summary

| #   | Invariant                          | Method                  | Tolerance                       |
|-----|------------------------------------|-------------------------|---------------------------------|
| 1   | HS freq = fsw                      | PCNT 1 s + MCPWM_CAP    | ±5 Hz @ 39 kHz                  |
| 2   | HS duty matches commanded count    | MCPWM_CAP, 1024 cyc     | ±0.001 duty                     |
| 3   | `pwmMax` matches source-clock math | host arithmetic         | exact                           |
| 4a  | HS→LS dead-band                    | MCPWM_CAP, 1024 cyc     | mean ±25 ns; min > 0            |
| 4b  | LS→HS dead-band (wrap)             | MCPWM_CAP, 1024 cyc     | min ≥ (dtTicks+1) − 30 ns       |
| 5   | LS force-off                       | MCPWM_CAP + PCNT        | 0 edges / 100 ms                |
| 6   | LS force-on (sync rect)            | MCPWM_CAP               | LS-high > (1 − dt/period)−0.001 |
| 7   | D=0 → HS low                       | digitalRead + PCNT      | 0 edges / 100 ms                |
| 8   | D=1 → HS high                      | digitalRead + PCNT      | 0 edges / 100 ms                |
| 9   | Glitch-free duty step              | MCPWM_CAP, ≥64 periods  | no out-of-set pulse widths      |
| 10  | Dead-time linearity                | sweep + Test 4a         | slope 1 ±5 %, intercept ≤30 ns  |
| 11  | OST brake latches LOW              | matrix-driven fault in  | both LOW < 1 µs                 |
| 12  | `stop()` idleLevel honoured        | digitalRead + 100 ms    | exact level, 0 edges            |
| 13  | LEDC glitch-free HS step           | MCPWM_CAP, ≥64 periods  | no out-of-set pulse widths      |

## Round 2 / scope-required (NOT in this spec)

- **Absolute dead-time below ~30 ns.** MCPWM_CAP quantizes at 12.5 ns; GPIO
  matrix has propagation jitter we can't fully characterise from inside.
  PicoScope (≥250 MS/s) needed for sub-30 ns.
- **Gate slew / Miller plateau / VDS overshoot.** Purely analogue.
- **Actual shoot-through current.** Logic-level dead-time can be right while
  gate-driver propagation still produces simultaneous conduction. Needs
  DC-coupled high-bandwidth current probe in the bridge mid-point.
- **`dtTicks` rounding when prescaler engages** (very-high-fsw regime, not used
  in production 20–100 kHz range).
- **Inter-leg phase relationship** (`MCPWM_Converter<N>` with N ≥ 2). Possible
  on-board but tight on capture channels per group; cleaner with a 4-channel
  scope.
- **Brown-out / reduced-VDD behaviour** — external supply control needed.
- **EMI / gate-node ringing** — antenna problem, scope only.
- **Capture-path systematic offset characterisation** — would need a calibrated
  external delay reference to turn the ±30 ns tolerance into an absolute number.

## Feasibility verdict

**Achievable, zero-hardware, driver-agnostic.** With internal GPIO-matrix
loopback (MCPWM `io_loop_back = 1` for same-pin, `esp_rom_gpio_connect_in_signal`
for cross-peripheral), every invariant in the Core / Per-driver lists above is
measurable on a stock ESP32-S3 with no jumpers, no external instrumentation,
and the half-bridge driver SD-asserted for safety. Same test source compiles
and runs against `PWM_ESP32_ledc`, `MCPWM_SyncLeg` (HiLi and InEn), and
`PWM_Mock`; per-test cap gates skip what each driver can't honour. The ±25–30 ns
tolerance comfortably swallows the GPIO-matrix propagation offset, which is
common-mode for difference measurements (duty, dead-band) and only affects
absolute-phase comparisons. Sub-30 ns dead-time, gate-slew, shoot-through
current, and EMI remain Round 2 / scope-required.
