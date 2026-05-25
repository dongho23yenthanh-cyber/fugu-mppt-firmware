*this document is an LLM generated placeholder*

# PWM gate-driver closed-loop verifier — design

**Goal:** finish `etc/mcpwm_gate_verify.py` into a one-shot Python tester that drives the device
over the existing console transport and captures HS/LS gate signals on a PicoScope 2000, asserting
four properties of the MCPWM driver: frequency, HS-duty linearity, LS-pulse position/width,
hardware dead-time + no shoot-through, and zero-CPU fault brake.

**In scope:** gate-driver only (mock-ADC bench device, dry probes on the gates, no converter
power). **Out of scope:** diode-emulation behaviour under load, MPPT/protection, real Vin/Vout.

## Bench wiring

Fixed for the whole run — the tester does not pause for re-probing.

```
   PicoScope 2000  Ch A ── HS gate pin (board.conf::pwm_hi)
                   Ch B ── LS gate pin (board.conf::pwm_li)
                   GND  ── device GND
   wire           drv ── fault-driver pin (any free GPIO, default 14)
                  ── pwm_fault_pin (board.conf::pwm_fault_pin)
```

Channels: 5 V range, DC-coupled. Trigger: Ch A rising at 1.65 V mid-rail.
The fault-driver pin is asserted via the firmware's `gpio <n> 0|1` console verb;
the MCPWM brake still reacts in hardware (the CPU only supplies the level — the brake
module forces the gates without further CPU involvement).

## Approach

One Python file, `etc/mcpwm_gate_verify.py`, replaces the existing 145-line stub. Runs four
phases sequentially, prints a per-assertion PASS/FAIL table, exits with `0` if all pass.

Reuses `etc/pico_capture.py::PS2000` (`capture(tb, n, two_ch=True, arm_timeout)`, proven in
`/tmp/ls_sweep2.py`), and `etc/fugu/transport.py` + `etc/fugu/console.py` for serial/socket.

Three approaches considered:

- **A. Finish `etc/mcpwm_gate_verify.py` in place** *(chosen)* — keeps the stub's docstring,
  argparse, `edges()` helper; replaces the broken `scope.capture_block_ab` with the real
  `PS2000.capture` API; plugs in four phase functions.
- **B. New `etc/pwm_test.py`** — rename for clarity, same shape. No real win; the stub's
  edge helpers are reusable.
- **C. Package `etc/pwm/{freq,duty,dt,fault}.py` + CLI wrapper** — modular, easier to grow.
  YAGNI for a 4-phase tester that fits in < 300 lines.

## Flow

```
  args (transport, deadtime_ns_override, fault-driver-pin, --skip-fault)
       │
       ▼
  open Console (Serial|Socket) + open PS2000
       │
       ▼
  setup_board(): get-config board.conf pwm_freq, pwm_max, pwm_deadtime_ns,
                                       pwm_hi, pwm_li, pwm_fault_pin
       │
       ▼
  Phase 1: frequency + HS-duty linearity
     ├─ for hs in resolve_grid(HS_GRID, pwmMax): `dc <hs>`
     ├─ hs=0 → assert HS static-LOW (no rises);  hs=pwmMax → assert HS static-HIGH
     ├─ otherwise capture(scope, freq); analyse → freq_meas, duty_hs
     ├─ assert |freq_meas - freq_expect| / freq_expect ≤ 1e-3
     └─ assert |duty_hs - hs/pwmMax| ≤ 1/pwmMax
       │
       ▼
  Phase 2: LS pulse position + width  (HS × LS matrix)
     ├─ for hs in resolve_grid(HS_GRID, pwmMax):
     │     for ls in resolve_grid(LS_FRACTIONS, pwmMax - hs - 1):
     │        send `dc <hs> <ls>`; read `pwm-dump` → (hs_off, ls_on, ls_off); capture
     ├─ assert ls_rise_s ≈ ls_on · tick_s            (±1 tick)
     ├─ assert ls_high_s ≈ (ls_off - ls_on) · tick_s (±1 tick)
     ├─ hs=0       → assert no edges on either channel
     ├─ hs=pwmMax  → assert HS static-HIGH, LS static-LOW (no-window case)
     ├─ ls=0       → assert no LS rises
     └─ ls fills window → assert LS does NOT stick HIGH (cliff regression)
       │
       ▼
  Phase 3: dead-time + no shoot-through
     ├─ `dc <pwmMax/2> <pwmMax/2 + N>` so both gates fully active
     ├─ capture one period both channels
     ├─ assert HS↓→LS↑ gap = pwm_deadtime_ns ±1 tick
     └─ assert no sample has hs>th AND ls>th
       │
       ▼
  Phase 4: fault GPIO brakes both gates  (skipped with --skip-fault)
     ├─ `dc 1024 1280` so both gates active
     ├─ over Console: `gpio <fault-driver-pin> 1` → drives pwm_fault_pin active
     ├─ capture; assert HS and LS go LOW within < 5 µs of the fault rising edge
     ├─ `gpio <fault-driver-pin> 0`; `dc 1024` again
     └─ assert fresh capture shows ≥2 HS rises (switching resumed)
       │
       ▼
  print PASS/FAIL summary; exit = number of failed rows
```

## Data shapes and PASS-gate math

One **capture** = `(dt_sec, hs[N], ls[N])` from
`PS2000.capture(tb, 3000, two_ch=True, arm_timeout=2.0)`, sized to ≥4 switching periods at
the configured `pwm_freq` via `timebase_for(window=4/freq, samples=3000)`.

One **analysis** = `edges(v, th=1.65)` → `(rises[], falls[])` index lists. From those:

| derived | formula | used by |
|---|---|---|
| `period_s` | `(hs_rises[1] − hs_rises[0]) · dt` | Phase 1 |
| `freq_meas` | `1 / period_s` | Phase 1 |
| `tick_s` | `1 / (freq_expect · pwmMax)` | all phases |
| `hs_high_s` | `(hs_falls[0] − hs_rises[0]) · dt` | Phase 1 |
| `duty_hs` | `hs_high_s / period_s` | Phase 1 |
| `ls_rise_s` | `(ls_rises[k] − hs_rises[0]) · dt`,  k = first LS rise after HS fall | Phase 2 |
| `ls_high_s` | `(ls_falls[k+1] − ls_rises[k]) · dt` | Phase 2 |
| `dt_hl_s` | `(ls_rises[k] − hs_falls[0]) · dt` | Phase 3 |
| `shoot_through` | `any(hs[i] > th and ls[i] > th)` | Phase 3 |
| `t_brake_s` | `t(first hs↓ after fault rise) − t(fault rise)` | Phase 4 |

**Event-tick semantics** (edge-aligned, count-up timer, period = `pwmMax`):

```
  tick 0          HS rises  (TEZ)
  tick hs_off     HS falls
  tick ls_on      LS rises  ( = hs_off + dtTicks )
  tick ls_off     LS falls
  tick pwmMax     timer wraps → next period (HS rises again)
```

`pwm-dump` (see Firmware surface) publishes these **event ticks directly** (not the
internal `pwmCtrl`/`pwmRect` widths) — the tester multiplies by `tick_s` to get expected
edge times without needing to know that `pwmRect` is a width in the driver. Tester math
stays correct when buck-math changes.

PASS expressions:

```
P1.freq        : abs(freq_meas - freq_expect) / freq_expect <= 1e-3
P1.duty[hs]    : abs(duty_hs - hs_off / pwmMax) <= 1.0 / pwmMax            # special: hs=0 / hs=pwmMax → skip duty, assert static-low / static-high
P2.lspos[hs,ls]: abs(ls_rise_s - ls_on · tick_s)            <= tick_s
                 abs(ls_high_s - (ls_off - ls_on) · tick_s) <= tick_s
                 hs=0       → no HS rises, no LS rises
                 hs=pwmMax  → HS static-HIGH, LS static-LOW
                 ls=pwmMax  → LS not static-HIGH (cliff regression)
P3.dt          : abs(dt_hl_s - (ls_on - hs_off) · tick_s)   <= tick_s
                 abs((ls_on - hs_off) · tick_s - pwm_deadtime_ns · 1e-9) <= tick_s
                 shoot_through == False
P4.brake       : t_brake_s < 5e-6
                 after deassert: fresh capture has ≥2 HS rises (switching resumed)
```

**Sweep grids — normalised against `pwmMax`.** Each entry is either an `int` (absolute
count, clamped into `[0, pwmMax]`) or a `float` in `[0.0, 1.0]` (fraction of `pwmMax`,
rounded to nearest integer). This lets the same grid hit the same logical points if
`pwmMax` changes (e.g. switching from 2048 to 1024 or 4096):

Grids are built at runtime once `pwmMax` is known from `get-config`. Each builder returns
a sorted list of unique ints in `[0, pwmMax]`. Boundary points `{0, 1, pwmMax-2, pwmMax-1,
pwmMax}` are always included to keep the cliff regressions covered as `pwmMax` changes.

```python
def hs_grid(pwm_max):
    """HS sweep points: boundary + log-spaced body."""
    fractions = [1/128, 1/64, 1/16, 1/4, 1/2, 3/4]
    body = [int(round(f * pwm_max)) for f in fractions]
    return sorted(set([0, 1] + body + [pwm_max - 2, pwm_max - 1, pwm_max]))
    # pwm_max=2048 → [0, 1, 16, 32, 128, 512, 1024, 1536, 2046, 2047, 2048]
    # pwm_max=4096 → [0, 1, 32, 64, 256, 1024, 2048, 3072, 4094, 4095, 4096]

def ls_grid(window):
    """LS sweep points for one HS, where window = pwm_max - hs - 1 is the LS clamp upper bound.
       Skips when window < 2 (no meaningful LS range)."""
    if window < 2:
        return []
    fractions = [0.05, 0.25, 0.5, 0.75, 0.99]
    body = [int(round(f * window)) for f in fractions]
    return sorted(set([0, 1] + body + [window - 1, window]))
    # hs=1024, pwm_max=2048 → window=1023 → [0, 1, 51, 256, 512, 767, 1013, 1022, 1023]
```

**Phase 1** iterates `hs_grid(pwmMax)`. Entries `hs=0` and `hs=pwmMax` are checked as
static-low / static-high outcomes (no edges expected); the linearity asserts run on all
other entries.

**Phase 2** is an `HS × LS` matrix, outer loop on HS:

```python
for hs in hs_grid(pwm_max):
    window = pwm_max - hs - 1
    for ls in ls_grid(window):
        send_dc(hs, ls); dump = read_pwm_dump(); cap = capture(scope)
        check_ls_pos_width(cap, dump, results)
    # special outcomes:
    if hs == 0:        assert_no_edges(...)
    if hs >= pwm_max - 1:  assert_hs_static_high_ls_static_low(...)
```

## Firmware-side surface

Everything except one new verb is already present:

| verb | status | used by | semantics |
|---|---|---|---|
| `dc <hs> [ls]` | exists (`src/cli.cpp`) | P1, P2, P3 | manual PWM; with `ls` switches to forced-rect (`manualRect`) |
| `gpio <pin> <0\|1>` | exists (`src/cli.cpp`) | P4 | `digitalWrite` for the fault-driver pin |
| `mppt` | exists | reset between phases | leaves manual mode |
| `get-config board.conf <key>` | exists | startup readback | reads `pwm_freq`, `pwm_max`, `pwm_deadtime_ns`, `pwm_hi`, `pwm_li`, `pwm_fault_pin` — tester does not need CLI args for these |
| **`pwm-dump`** | **new** | P2, P3, P4 | one-line readback of live PWM state |

`pwm-dump` output, parsed by the tester with a key=value regex:

```
freq=39062 pwmMax=2048 hs_off=1024 ls_on=1026 ls_off=1280 fault=0 brake=0
```

Fields are **event ticks** (count where the named edge fires), not driver-internal widths.
The tester multiplies each tick by `tick_s = 1 / (freq · pwmMax)` to get expected edge
times. Derivation in the firmware:

- `hs_off = pwmCtrl`
- `ls_on  = pwmCtrl + dtTicks`   (or `0` when LS is disabled / `pwmRect == 0`)
- `ls_off = pwmCtrl + pwmRect`   (or `0` when LS is disabled)

Implementation: ~15-line handler in `src/cli.cpp` reading from `Converter` getters and a
new `MCPWM_SyncLeg::getDtTicks()` getter. Per CLAUDE.md ("expose with a getter") — no new
member variables.

Recovery between phases: `dc 0` then `mppt` to leave manual mode cleanly. After Phase 4 the
tester also sends `gpio <fault-driver-pin> 0` followed by `dc 1024`, then waits for a fresh
capture showing switching activity as the "fault clears" assertion.

## File layout

One file, `etc/mcpwm_gate_verify.py`, ~250 lines.

```python
# argparse:
#   transport: --serial PORT | --ip HOST --port PORT
#   --fault-driver-pin N (default 14)
#   --hs-grid / --ls-grid (comma lists, defaults from above)
#   --skip-fault   (skip Phase 4 if the fault wire is not in place)
#   --force-host   (override the hostname allow-list — see Safety)
#   -v             (per-capture detail rows)

LOGIC_TH = 1.65

def edges(v, th=LOGIC_TH): ...                  # kept from the existing stub
def open_console(args): ...                     # SerialTransport | SocketTransport
def open_scope():                               # PS2000 with two ch, 5V DC
    s = pc.PS2000()
    s.channel(pc.CH_A, pc.RANGES['5v'], pc.DC)
    s.channel(pc.CH_B, pc.RANGES['5v'], pc.DC)
    s.trigger(pc.CH_A, 0, rising=True, delay_pct=0, auto_ms=500)
    return s

def capture(scope, freq, samples=3000):
    tb, dt, n = scope.timebase_for(4 / freq, samples)
    a, b, ov = scope.capture(tb, n, True, arm_timeout=2.0)
    return dt, pc.to_volts(a, pc.RANGES['5v']), pc.to_volts(b, pc.RANGES['5v'])

def parse_pwm_dump(line) -> dict: ...           # key=value regex
def read_state(con) -> dict:                    # send `pwm-dump`, parse reply
    return parse_pwm_dump(con.command('pwm-dump').reply)
def setup_board(con) -> dict:                   # `get-config` for the six keys above

def phase1_freq_duty(con, scope, board) -> list[Result]: ...
def phase2_ls_pos_width(con, scope, board) -> list[Result]: ...
def phase3_deadtime(con, scope, board) -> list[Result]: ...
def phase4_fault_brake(con, scope, board, drv_pin) -> list[Result]: ...

Result = dict(name=str, pass_=bool, measured=float|None, expected=float|None,
              tol=float|None, detail=str)
```

## Output format

One row per assertion. Phase headers and a summary line:

```
PHASE 1  freq + HS duty (pwmMax=2048)
  static[hs=0]         PASS   HS static-low
  duty[hs=1]           PASS   0.05%       exp 0.05%      tol 1t
  duty[hs=16]          PASS   0.78%       exp 0.78%      tol 1t
  duty[hs=128]         PASS   6.25%       exp 6.25%      tol 1t
  duty[hs=1024]        PASS   50.00%      exp 50.00%     tol 1t
  freq_meas            PASS   39060.5 Hz  exp 39062.5 Hz tol 0.1%
  static[hs=2048]      PASS   HS static-high
PHASE 2  LS pos+width (HS × LS)
  ls_pos[hs=1,ls=51]      PASS   0.66µs   exp 0.66µs   tol 0.013µs (1t)
  ls_width[hs=1,ls=51]    PASS   0.62µs   exp 0.62µs   tol 0.013µs (1t)
  ls_pos[hs=1024,ls=512]  PASS  13.10µs   exp 13.10µs  tol 0.013µs
  ls_width[hs=1024,ls=W]  PASS  12.83µs   exp 12.83µs  tol 0.013µs   <-- cliff regression (ls fills window)
  static[hs=2048]         PASS   no LS window: HS static-high, LS static-low
  ...
PHASE 3  deadtime + shoot-through
  dt_hl                PASS   80ns        exp 80ns       tol 12.8ns (1t)
  shoot_through        PASS   none
PHASE 4  fault brake
  t_brake              PASS   1.42µs      exp <5µs
  recover              PASS   12 HS rises in 4-period window

SUMMARY  PHASE1 PASS  PHASE2 PASS  PHASE3 PASS  PHASE4 PASS   exit=0
```

## Error handling

- Exceptions inside a phase function produce one `Result(pass_=False, detail=<one-line traceback>)`
  and the phase keeps going where it can (rows are independent). Phases themselves run sequentially
  — a failed phase does not abort later phases.
- `KeyboardInterrupt`: send `dc 0` then close the scope; cleanup lives in `try/finally`.
- `PS2000.capture` arm-timeout (gates not switching at all) → single FAIL row, abort that phase.
- Exit code = total number of failing rows (0 = all pass).

## Safety

The fixed-host CLAUDE.md guard, mechanised:

- On startup the tester reads the device hostname from the console welcome banner / `ip` reply.
- If the hostname matches `fry`, `flat`, or any name not matching the allow-list pattern
  `fugu-esp32s3-*`, the tester **refuses to start**.
- Override with `--force-host` (the operator types it on the command line every run — no
  config file, no env var). The override is logged into the output table so any captured run
  trail records that the safety was bypassed.

This guards the "fry & flat are real power converters connected to solar panels and a battery"
case in CLAUDE.md from the tester driving manual PWM on a live half-bridge.

## Future extensions (not built now)

- Interleaved-leg phase offset assertion when `MCPWM_Converter<N>` with N>1 is enabled.
- Statistical capture across `M` periods per point (currently uses the first two HS rises in a
  window of 4) for jitter / glitch budgets.
- A `--csv` flag to emit raw measurements for offline analysis.

These are easy adds because each phase function returns a list of `Result` independently — extend
by appending new functions, not by reshaping existing ones.
