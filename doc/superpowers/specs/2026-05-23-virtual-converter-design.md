*this document is an LLM generated placeholder*

# VirtualConverter (vconv) — Design

## Goal

A pure-software model of a DC-DC synchronous buck (later boost) converter, embedded into the firmware so the existing
control code (`SynchronousConverter`, `MPPT_Tracker`, PD loops, charger) can be exercised on real ESP32 hardware without
a physical power stage. Replaces `ADC_Fake` in mock board configs (`wokwi_mock`, `dry_mock`).

Out of scope for v1: boost topology, MOSFET losses, coil saturation, cap ESR, host-side run.

## Architecture

Three units with one responsibility each:

```
┌─────────────────────┐    update_pwm()    ┌──────────────────────┐    getSample()    ┌──────────────────┐
│   PWM_VConv         │ ─────────────────> │   VirtualConverter   │ <───────────────  │   ADC_VConv      │
│ (PwmDriver impl)    │                    │   (g_vconv singleton)│                   │ (AsyncADC impl)  │
│ src/pwm/vconv.h     │                    │   src/sim/vconv.{h,cpp}                  │ src/adc/vconv.h  │
└─────────────────────┘                    └──────────────────────┘                   └──────────────────┘
   selected at build-time                   pure C++ (no Arduino/IDF)                   selected at runtime
   via WITH_VCONV in buck.h                                                             via sensor.conf
```

- **`PWM_VConv`** — replaces `PwmDriver` (the LEDC backend) at compile time. Implements the same surface as `PWM_Mock`
  (`pwmMax`, `init_pwm`, `update_pwm(ch, duty)`, `update_pwm(ch, hpoint, duty)`, `stop`). The shim translates LEDC's
  two call shapes into an absolute on-count and calls `g_vconv.setPwmCount(ch, on)` — see §PWM-input for the per-shape
  rules; `stop(ch, idleLevel)` zeroes the latched counts for that channel so the coil idles.
- **`VirtualConverter`** — the model. Pure C++ (no `<Arduino.h>`, no FreeRTOS, no `esp_*`). Owns the converter state
  (`V_in`, `V_out`, `I_L_end`), holds the configuration (passives + sources/sinks), exposes
  `setPwmCount`/`setPwmMax`/`setPwmFreq` (input) and `stepSeconds(dt, freqFallback)` / `getVin / getVout / getIoutAvg /
  getIinAvg / getIL` (outputs). Stepping is decoupled from PWM updates: each call advances the model by
  `N = round(dt · pwmFreq)` PWM cycles using the most recently latched PWM counts.
- **`ADC_VConv`** — implements `AsyncADC<float>`. Wraps the existing `PeriodicTimer` + `TaskNotification` pattern from
  `ADC_Fake` to schedule samples. On each tick: calls `g_vconv.step(1.0 / adc_freq)`, then `getSample(ch)` returns the
  channel's current model state. NTC channel returns a constant.

### Compile-time selector

`WITH_VCONV` slots **inside** the existing `#ifndef MOCK` block in `src/buck.h`, alongside the MCPWM/LEDC branches —
vconv targets real on-device builds (it *is* the mock plant), not the `host-stub` MOCK build, which keeps using
`PWM_Mock`:

```cpp
#ifndef MOCK
  #include <Arduino.h>
  #if WITH_VCONV
    #include "pwm/vconv.h"
    using PwmDriver = PWM_VConv;
  #elif WITH_MCPWM
    #include "pwm/mcpwm.h"
    using PwmDriver = MCPWM_SyncLeg;
  #else
    #include "pwm/ledc.h"
    using PwmDriver = PWM_ESP32_ledc;
  #endif
#else
  #include "pwm/mock.h"
  using PwmDriver = PWM_Mock;
#endif
```

CMake enforces `WITH_VCONV` and `WITH_MCPWM` mutually exclusive (and both imply `!MOCK`). `PWM_VConv` mirrors the
LEDC-style API only; it does not implement MCPWM-specific calls
(`setHsOff/setLsOff/forceShutdown/clearForce/oper/genHS/genLS/getDtTicks`). MCPWM-only code in `buck.h` already lives
under `#if WITH_MCPWM`, so when `WITH_VCONV=1` those paths are excluded.

### Runtime selector

`ADC_VConv` is opted-in per channel in `sensor.conf`:
```
adc=vconv
vin_ch=0
vout_ch=1
iout_ch=2
ntc_ch=4
```

Channel-to-quantity mapping is fixed in `ADC_VConv::getSample()`:

| ch | quantity |
|----|----------|
| 0  | `V_in`   |
| 1  | `V_out`  |
| 2  | `I_out_avg` |
| 4  | NTC (constant `25.0` °C-equivalent voltage) |

## Model

### State

Three scalars carry all state between steps:

- `V_in` — input cap voltage (V)
- `V_out` — output cap voltage (V)
- `I_L_end` — coil current at the end of the previous PWM cycle (A). Signed; reverse current allowed.

### PWM input

`PWM_VConv` writes into a small struct, owned by `g_vconv`, updated under no lock (single producer =
`SynchronousConverter::pwmPerturb` on RT core, single consumer = `g_vconv.step` also on RT core via
`ADC_VConv::getSample`):

```cpp
struct PwmState {
    uint16_t pwmMax;
    uint16_t pwmCtrl;   // HS on-count  (buck)
    uint16_t pwmRect;   // LS on-count  (buck)
    uint32_t pwmFreq;   // Hz
};
```

LEDC's `duty` parameter is the high-level **duration** (on-count), not the high→low transition
point — `LPOINT = HPOINT + DUTY` in the TRM. `hpoint` only shifts where in the period the
pulse sits; for the plant (which places LS strictly after HS) it carries no information.

`PWM_VConv` translates the two LEDC call shapes into absolute on-counts before calling
`g_vconv.setPwmCount(ch, on)`:
- `update_pwm(ch, hpoint, duty)` two-arg (HiLi): on-count = `duty`. `hpoint` is ignored.
  - `ch=0` (HS): firmware passes `hpoint=0, duty=pwmCtrl` → `pwmCtrl = duty`.
  - `ch=1` (LS): firmware passes `hpoint=pwmCtrl, duty=pwmRect` → `pwmRect = duty`.
- `update_pwm(ch, duty)` single-arg:
  - `ch=0` (HS): `pwmCtrl = duty`.
  - `ch=1` EnLogic (EN signal): firmware passes `duty = pwmCtrl + pwmRect`. The shim
    subtracts the last-stored HS count: `pwmRect = duty − pwm_.pwmCtrl` (clamped ≥ 0).
  - `ch=1` HiLi reset (`update_pwm(1, 0)`): subtraction yields 0, which is the desired reset.

The EnLogic ordering quirk in `buck.h::pwmPerturb` (EN updated before IN on `direction<0`) is
faithfully mirrored: when ch=1 is written first with the *new* EN duty but the model still
holds the *old* `pwmCtrl`, the subtraction yields `pwmRect_new + (pwmCtrl_new − pwmCtrl_old)`,
matching the firmware's documented "effective LS less than commanded" behavior.

Note: `PWM_Mock::v = duty − hpoint` in `src/pwm/mock.h:30` looks similar but is unused dead
code (no reader anywhere) — not a reference for the correct mapping.

### Per-cycle solution (buck)

Each PWM period of length `T = 1/pwmFreq`:

```
t_HS = T * pwmCtrl / pwmMax     // HS on duration
t_LS = T * pwmRect / pwmMax     // LS on duration
t_off = T - t_HS - t_LS         // both switches off (DCM dead-time)
```

The plant assumes the firmware honors `pwmCtrl + pwmRect <= pwmMax` (clamped in
`buck.h::update()`) so that `t_off >= 0`. `vconv.cpp` asserts that this condition
is true, prints an error and stops operation if violated.

Coil current trajectory, starting from `I_L_end`:

```
Phase 1 (HS on, 0 <= t < t_HS):
    I_L(t) = I_L_end + (V_in - V_out) / L * t
    I_L_after_HS = I_L(t_HS)

Phase 2 (LS on, t_HS <= t < t_HS + t_LS):
    I_L(t) = I_L_after_HS + (-V_out) / L * (t - t_HS)
    I_L_after_LS = I_L_after_HS - V_out / L * t_LS    # signed; may go negative
```

Phase 2 is integrated straight through with no mode decision. Diode emulation is a
*firmware* behavior — `buck.h` chooses `pwmRect` to schedule LS-off at the predicted
coil zero crossing, so a correctly tuned firmware lands phase 2 at `I_L_after_LS ≈ 0`
by construction. Forced-PWM (sync rect held past the crossing) is just the same
integration with a longer commanded `pwmRect`; the endpoint goes negative and phase 3
handles HS-body-diode pumping. The phase-2 area is the signed trapezoid on the linear
segment from `I_L_after_HS` to `I_L_after_LS`.

```
Phase 3 (both switches off, t_HS + t_LS <= t < T):
    If I_L_after_LS > 0:  LS body diode conducts, current decays at -V_out/L toward 0,
                          clamped at 0 (then idle for the rest of the period).
    If I_L_after_LS == 0: idle for the rest of the period.
    If I_L_after_LS < 0:  (forced-PWM reverse-current regime) HS body diode clamps SW
                          to V_in, so V_L = V_in - V_out and current decays at
                          +(V_in - V_out)/L toward 0, clamped at 0.

I_L_end := value of I_L at end of phase 3 (typically 0 in DCM, ==I_L_after_LS in CCM
           where phase 3 has zero length because t_HS + t_LS == T).
```

Averages over the period (the quantities the ADC reads):

```
I_in_avg  = (area under I_L during phase 1) / T            # PV pulls current only during HS
I_out_avg = (area under I_L during phases 1+2+3) / T       # battery sees the cap-filtered mean
```

Both integrals are closed-form (triangles + trapezoids). Implementation:

```cpp
// pseudocode, in VirtualConverter::stepOneCycle():
float a = I_L_end;
float b = a + (V_in - V_out) / L * t_HS;             // I_L_after_HS
float c = b - V_out / L * t_LS;                       // I_L_after_LS (signed)
float area_HS = 0.5f * (a + b) * t_HS;
float area_LS = 0.5f * (b + c) * t_LS;                // signed trapezoid

// Phase 3: body-diode decay over t_off = T - t_HS - t_LS.
// c > 0 → -V_out/L slope (LS body diode); c < 0 → +(V_in - V_out)/L slope (HS body diode).
// Slope == 0 (V_out == 0 short-circuit, or V_in == V_out): no back-EMF, current
// passes through unchanged. Otherwise clamp at zero once reached.
float t_off = T - t_HS - t_LS;
float slope = (c > 0) ? -V_out / L
            : (c < 0) ? (V_in - V_out) / L
                      : 0.0f;
float cEnd, area_off;
if (t_off <= 0 || c == 0 || (c > 0 && slope >= 0) || (c < 0 && slope <= 0)) {
    // No active decay path: coil idles at c.
    cEnd = c;
    area_off = c * t_off;
} else {
    float t_to_zero = -c / slope;                     // > 0 by construction
    if (t_to_zero < t_off) {
        cEnd = 0;
        area_off = 0.5f * c * t_to_zero;              // triangle, then idle at 0
    } else {
        cEnd = c + slope * t_off;
        area_off = 0.5f * (c + cEnd) * t_off;
    }
}

I_in_avg  = area_HS / T;
I_out_avg = (area_HS + area_LS + area_off) / T;
I_L_end   = cEnd;
```

### Capacitor dynamics

V_in cap is forward-Euler at PWM-cycle rate; V_out cap is backward-Euler (see below):

```
V_in   += (I_pv(V_in) - I_in_avg) * T / C_in
I_bat   = (V_out - V_bat) / R_bat     // small R_bat → stiff battery; bidirectional
```

Numerical guards:
- Clamp `V_in ∈ [0, Voc + 5%]` to keep `I_pv` exponential from blowing up.
- Clamp `V_out ∈ [0, max(2·V_bat, 2·Voc)]` defensively. The `2·Voc` fallback matters in the open-circuit preset
  (`v_bat=0`) — otherwise `2·V_bat = 0` would pin V_out at zero and erase every interesting transient.
- If `V_in < 1e-4 V`, skip the per-cycle math (no PV source → phase-1 is degenerate) and relax caps only. V_out is
  **not** guarded: a real short pulls V_out near zero while the converter still delivers large I_L, and the phase math
  handles V_out → 0 cleanly (phase-2/3 slopes go to zero, coil idles).
- `setBat` clamps `r_bat ≥ 1 µΩ` to keep the BE coefficient `a = T/(R·C)` finite. Console `vconv bat short` uses
  `r_bat = 1 mΩ`, three orders of magnitude above the floor.

The V_in cap uses forward-Euler (its time constant is set by `C_in` and the PV dynamic
resistance, both bounded — no stability concern). The V_out cap uses **backward-Euler**:

```
V_out_new = (V_out + T·I_out_avg/C_out + a·V_bat_eff) / (1 + a),   a = T / (R_bat·C_out)
```

This is unconditionally A-stable: `r_bat` can go arbitrarily small without crossing a
numerical cliff (forward-Euler would need `T/(R_bat·C_out) < 2`, which fails well above
realistic short impedances). In the small-T limit BE matches FE exactly, so the
normal-passives regime is unchanged.

### Sources / sinks

**PV (input):** single-diode-ish exponential parameterized by `Isc`, `Voc`, and `k = V_mpp / Voc`
(dimensionless; ~0.75–0.85 for typical Si):

```
I_pv(V) = Isc * (1 - exp(α * (V - Voc) / Voc)) / (1 - exp(-α))
```

The factor `1 / (1 - exp(-α))` normalizes so `I_pv(0) = Isc` and `I_pv(Voc) = 0` exactly.
`α` is derived from `k` once at config-load by solving the MPP condition `dP/dV = 0`,
which reduces to:

```
α * (1 - k) = ln(1 + k * α)
```

Solve with a few Newton iterations starting from `α₀ = 1/(1-k)`. Default `k = 0.8 → α ≈ 11.5`.
Smaller `k` → smaller `α` → softer knee. `I_pv` is clamped to `[0, Isc]` at runtime.

**Battery (output):** stiff voltage source `V_bat` with small series `R_bat` (~0.05 Ω) so the cap 
dynamics see a derivative instead of a clamp δ-function. `I_bat = (V_out − V_bat_eff) / R_bat`, 
always bidirectional — the battery sources current when V_out drops below V_bat, which is essential
to model the sync-rect reverse-current regime (LS held past the coil zero crossing pumps energy from 
output back into V_in via the HS body diode). Modelling the `backflow.h` GPIO switch is out of scope
for v1: the firmware closes it as part of normal operation, and the regimes that depend on it being 
open (full-disable hold-off) are tested separately.

**Open-circuit output.** No new state — degenerate case of the same equation: set `v_bat=0` and
`r_bat=1e9` (console preset `vconv bat open`). `I_bat = V_out / 1e9 ≈ 0` so the output cap just
integrates whatever the converter delivers, and V_out climbs until the firmware OVP trips (or
the model's defensive ceiling at `max(2·V_bat, 2·Voc)`, which is why the ceiling falls back to
`2·Voc` when `V_bat ≈ 0` — otherwise it would pin V_out at zero and erase every interesting
open-circuit transient). Mains ripple is skipped when `V_bat == 0`: a bipolar swing around zero
is unphysical and meaningless when there's no sink.

**Mains ripple on V_bat.** Inverter-fed topologies (see [[project_inverter_fed_topology]]) put 2·f_grid 
ripple on the DC bus — at kW it's *volts* peak, bulk caps don't filter it, and the battery is the only sink.
The plant injects this as a sinusoidal modulation:

```
V_bat_eff(t) = V_bat + vbat_ac_amp * sin(phase)
phase += 2π * vbat_ac_freq * T   # mod 2π each cycle to avoid float drift
```

Default `vbat_ac_amp = 0` (off, back-compat with the existing deterministic tests). Setting it non-zero shows up as
ripple on `V_out` through the `R_bat·C_out` lowpass and is what the firmware's `vout` IIR notch (tuned at `notch_f`, see
`src/adc/sampling.h:149`) is supposed to cancel. If the notch is mis-tuned or never engaged, this disturbance will leak
into MPPT P&O and visibly perturb tracking — exactly the failure mode the notch exists to suppress.

Ripple is suppressed when `V_bat == 0` (open-circuit / short-circuit presets): a bipolar swing around zero is
unphysical and meaningless when there's no sink.

### N-cycle stepping

`ADC_VConv::getSample()` ticks at `adc_freq` (default same as `ADC_Fake`'s `adc_fake_freq`). Per ADC sample,
`g_vconv.step(1.0f / adc_freq)` runs `N = round(pwmFreq / adc_freq)` cycles (typically 10–15). N is recomputed each step
from the latched `pwmFreq` in case it changes (it won't, but the cost is one division).

**Coalesced-tick catch-up.** The periodic timer notifies the RT task every ADC tick, but the notification is a
clear-on-exit binary count: if the control loop overruns one tick, several notifications pile into one wakeup.
`hasData()` reads that count (`TaskNotification::waitCount`) and advances the plant by `count × dt`, not a single `dt`,
so sim-time tracks the elapsed wall-clock-ticks even under overrun — a real plant keeps moving while the controller is
busy. Stepping `count × dt` is exactly equivalent to `count` separate `dt` steps (proved by
`test_vconv_catchup_equals_uncoalesced`). The catch-up is capped at `kMaxCatchupTicks` (8) so a gross stall can't turn
into a multi-ms compute spike. The earlier single-`dt`-per-wakeup behavior silently dropped sim-time, which is what made
the MPPT loop flip convergence basins under sub-µs timing perturbations — vconv is for **profiling**, not control-
behavior validation (validate control changes on real hardware).

### ADC noise

`ADC_VConv::getSample(ch)` adds zero-mean Gaussian noise per channel *after* fetching the deterministic plant value,
modelling LSB jitter and analog-front-end noise. Per-channel σ comes from `vconv.conf` (`noise_vin`, `noise_iin`,
`noise_iout`, `noise_vout`, `noise_ntc`) in physical units; default 0 keeps existing host-stub tests reproducible.
Generated via Box-Muller on a fast xorshift32 PRNG — no `<random>` pulled in, ~1 µs/sample on the S3. Noise lives in the
ADC layer, **not** in the plant: the per-cycle solver stays deterministic so unit tests can assert exact `I_L_end` /
area values, and the noise is what stresses the moving-median + EWM chain in `src/adc/sampling.h`. A typical realistic
bench setup: `noise_vin ≈ 0.02 V`, `noise_iin ≈ 0.05 A` (matches the LSB σ on the INA226 and the internal-ADC noise
floor measured via the `scope` command).

## Configuration

New file `vconv.conf`, lives in board configs alongside `coil.conf`:

```
# PV source
isc=8.0           # short-circuit current, A
voc=40.0          # open-circuit voltage, V
pv_k=0.8          # V_mpp / Voc ratio (dimensionless, 0.75–0.85 typical for Si)

# Battery sink
v_bat=28.0        # stiff battery DC voltage, V
r_bat=0.05        # small series resistance for stiffness, Ω
vbat_ac_amp=0.0   # peak AC ripple amplitude on V_bat, V (0 = off; ~1 V at kW into an inverter)
vbat_ac_freq=100  # ripple frequency, Hz (2·f_grid: 100 for 50 Hz mains, 120 for 60 Hz)

# Passives  (L is read from coil.conf::L0 — nameplate, not the 0.95·L0 the
#           firmware uses internally via InductivityDcBias; the plant models the
#           real coil and lets the firmware's derate be tested as a mismatch)
c_in=470e-6       # input cap, F
c_out=470e-6      # output cap, F

# Sampling tick (also reused as the cycle-step granularity)
adc_freq=3000     # Hz, matches ADC_Fake default

# ADC noise (zero-mean Gaussian σ in physical units; default 0 keeps tests deterministic)
noise_vin=0.0     # V
noise_iin=0.0     # A
noise_iout=0.0    # A
noise_vout=0.0    # V
noise_ntc=0.0     # V
```

`vin_rh / vin_rl` etc. from `sensor.conf` still apply downstream — the ADC backend returns raw V at the ADC pin
(post-divider equivalent), the `Sensor` `LinearTransform` scales it. To keep this simple: `ADC_VConv` returns physical
V_in / V_out / I_in / I_out, and the divider/gain math in `sensor.conf` is set to identity (`*_factor=1.0`, no divider)
in mock board configs. (Existing `wokwi_mock` and `dry_mock` already do something similar.)

## Console command

```
vconv                           # dump state: Vin, Vout, IL, Iin, Iout, CCM/DCM, pwm[ctrl, rect, max, freq], ERR?
vconv pv <isc> <voc> [k]        # update PV params at runtime
vconv bat <v>                   # update battery voltage at runtime
vconv bat open                  # open-circuit output preset (v_bat=0, r_bat=1e9)
vconv bat short                 # hard-short output preset (v_bat=0, r_bat=1e-3)
vconv set <key> <value>         # in-memory setter; keys: c_in, c_out, l, r_bat, vbat_ac_amp, vbat_ac_freq
```

Registered in `src/cli.cpp` alongside the existing commands. Dispatch reads / writes `g_vconv` directly. No persistence
— `set-config vconv.conf <key> <value>` is the path for that.

## Tests

Two layers:

1. **Math unit test** — `test/host-stub/vconv-test.cpp`. Builds in host-stub mode (no Arduino, no IDF). This test block
   is meant to test the vconv model only, not the firmware.

   **Physical invariants** (cheapest checks that the per-cycle solver is dimensionally + signwise correct):
   - **Volt-second balance on L (CCM steady state).** After settle: `Vout/Vin ≈ tHS/(tHS+tLS)` to 0.5%.
   - **Volt-second balance on L (DCM steady state).** With diode emulation landing phase 2 at `c ≈ 0` and phase 3
     idle: `Vin·tHS ≈ Vout·(tHS + tLS)` to 0.5%. Forced-PWM DCM with phase-3 decay: `Vin·tHS ≈ Vout·(tHS + tLS +
     tDecay)` where `tDecay = b·L/Vout` is the phase-3 ramp-to-zero time. Catches sign bugs in the phase-3 area
     integration that the CCM check misses (DCM is where diode emulation actually runs most of the time).
   - **Coil ripple amplitude.** Settled cycle: `max(IL) − min(IL) = (Vin − Vout)·tHS/L` to 1%. Capture `a/b/c` via
     friend or by querying `iLEnd` over one cycle.
   - **Per-cycle charge balance on Cout.** `mean(iOutAvg) ≈ Ibat` to ~1% after `T·N ≫ Rbat·Cout`.
   - **Energy conservation (lossless plant).** `Vin·iInAvg ≈ Vout·iOutAvg` to 1% in CCM steady state. Plant has no
     Rds(on) / diode drop, so a failure exposes an area-integration bug.

   **Operating-point sweeps:**
   - Steady-state CCM: known V_in, V_out, D → expected I_L_avg.
   - DCM zero-crossing: at fixed `pwmCtrl`/`pwmRect`, sweep the load (vary `V_bat`) and assert the model's `inDcm()`
     flips true exactly when the closed-form ripple half-amplitude `ΔI_L/2 = (V_in − V_out)·D·T/(2L)` exceeds the
     steady-state `I_out_avg`. **`D` here is `tHS/T` (HS-only duty), not `pwmCtrl + pwmRect`** — using EnLogic duty
     introduces an off-by-(1+dLS) factor at high load. Purely a model-vs-analytic check; firmware-boundary alignment is
     the on-target risk item (§Difficulties #1).
   - Sweep: PV IV-curve has a peak at the expected V. Also pin boundaries: `pvCurrent(0) == Isc`,
     `pvCurrent(Voc) == 0`, `pvCurrent(V > Voc) == 0`, `pvCurrent(V < 0) == Isc` (all clamped). Run the `setPv` Newton
     solver across `k ∈ {0.5, 0.75, 0.85, 0.95}` and assert convergence + `dP/dV|_{V=k·Voc} ≈ 0` to 0.5% (the MPP
     condition the solver enforces).
   - Cap dynamics: step change in PV current → V_in settles with the right time constant.

   **Corner cases:**
   - **D=0 idle** (`pwmCtrl=0`): after 1000 cycles assert `iInAvg=0`, `iOutAvg=0`, `iLEnd=0`, Vin → Voc, Vout → Vbat
     exactly. Pins the `tHS=0` branch against state carry from prior runs.
   - **D=1 saturation** (`pwmCtrl=pmax, pwmRect=0`): `tOff=0`, `dcm_==false`, IL grows monotonically until Vout → Vin.
     Catches sign bugs in the phase-3 guard.
   - **Vout ≈ Vin** (D≈1, light load): phase-2 slope `−Vout/L` is large vs phase-1 slope `≈ 0` — assert no NaN, IL stays
     bounded.
   - **Iload ≈ 0** (set `v_bat = Vout_target`): assert steady `iOutAvg ≈ (Vout − Vbat)/Rbat` to 1%.
   - **Battery at termination** (sweep Vbat up to `cv_eoc`): raising Vbat above MPP·D collapses `iOutAvg → 0` without
     instability.
   - **Open-circuit output** (`vconv bat open`, i.e. `v_bat=0, r_bat=1e9`): with a modest duty driving the plant, assert
     V_out climbs monotonically from its start and stops at `≤ 2·Voc` (the `max(2·V_bat, 2·Voc)` defensive ceiling).
     `iOutAvg` should be essentially zero in steady state, and `getIL()` finite. Catches a regression where the V_out
     clamp re-pins to `2·V_bat=0` or where the mains-ripple guard fires with `V_bat=0`.
   - **Short-circuit output** (`vconv bat short`, i.e. `v_bat=0, r_bat=1e-3`): commanded duty with `pwmRect` small,
     run a few hundred cycles. Assert (a) `isfinite(V_out)` and `isfinite(IL)` throughout, (b) `V_out` stays small
     (`< 1 V`), (c) `iOutAvg > 0` (current actually flows into the short — guards against the V_out-below-`kVMin` early
     return zeroing iOutAvg), (d) phase-3 doesn't divide-by-zero when V_out lands at exactly 0 (the `slope < 0` guard).
     Note: with no L-saturation model, I_L grows over many cycles; the test horizon is bounded, not asserting steady
     state. The real firmware would trip OCP — that's an on-target concern, not this test's.
   - **Reverse-current into a short** (`vconv bat short` + forced-PWM `pwmRect` past the zero crossing): phase 2 ends
     with `c < 0`, phase 3 should engage the HS body diode with `slope = (V_in − V_out)/L ≈ V_in/L > 0` and pump charge
     back into V_in. Assert (a) `isfinite` throughout, (b) V_in trends upward over the test window (HS body-diode pump
     is the only source feeding C_in beyond `I_pv`), (c) `iInAvg` matches the analytic phase-1 triangle sign (the
     reverse-pump current shows up on the *input* side), (d) no NaN from the phase-3 `slope > 0` guard when V_in ≈ V_out
     ≈ 0 transiently. Confirms the HS body-diode path stays well-defined when both nodes are near zero.
   - **`pwmCtrl + pwmRect > pwmMax` latch.** Force `pwmCtrl=600, pwmRect=600, pwmMax=1000`, run one cycle. Assert
     `errored() == true`, `iInAvg == 0`, `iOutAvg == 0` (the early-return path at `vconv.cpp:21-30`). Pins the error
     gate that defends the rest of the math against a misbehaving caller.
   - **Degenerate phase durations.** (a) `pwmCtrl=0, pwmRect>0`: phase 1 is degenerate (`a=b` at `vconv.cpp:69`), phase
     2 runs `−Vout/L·tLS` against residual IL — assert IL drains monotonically toward zero, no NaN. (b) `pwmCtrl>0,
     pwmRect=0`: phase 2 is degenerate (`b=c` at `vconv.cpp:74`), phase 3 handles the LS body-diode decay — assert
     `iOutAvg > 0` and `iLEnd == 0` in DCM steady state. Pins both `tHS==0` and `tLS==0` short-circuit branches.

   **Shim wiring (`PWM_VConv` ↔ `VirtualConverter`):**
   The shim must translate LEDC's two call shapes into `(pwmCtrl, pwmRect)` correctly, including the EnLogic
   "EN-before-IN on `direction<0`" ordering that gives "effective LS less than commanded". One scripted sequence covers
   both modes and both directions.

   Starting state: `pwmMax=1000, pwmCtrl=500, pwmRect=20`.

   | Step | Call                            | Mode    | Expected after            | Note                                       |
   |------|---------------------------------|---------|---------------------------|--------------------------------------------|
   | 1a   | `update_pwm(0, 0, 510)`         | HiLi    | `pwmCtrl=510, pwmRect=20` | HS HiLi: duty straight-through             |
   | 1b   | `update_pwm(1, 510, 25)`        | HiLi    | `pwmCtrl=510, pwmRect=25` | LS HiLi: duty straight-through             |
   | 2a   | `update_pwm(0, 490)`            | EnLogic | `pwmCtrl=490, pwmRect=25` | HS single-arg                              |
   | 2b   | `update_pwm(1, 515)`            | EnLogic | `pwmCtrl=490, pwmRect=25` | LS = EN − stored HS = 515 − 490 = 25 ✓     |
   | 3a   | `update_pwm(1, 515)`            | EnLogic | `pwmCtrl=490, pwmRect=15` | **direction<0 quirk**: EN written before IN; LS = 515 − 500 = 15 (deficit = 10 = ΔHS) |
   | 3b   | `update_pwm(0, 490)`            | EnLogic | `pwmCtrl=490, pwmRect=15` | HS catches up; effective LS stays at 15    |
   | 4    | `update_pwm(1, 0)`              | EnLogic | `pwmCtrl=490, pwmRect=0`  | HiLi reset: 0 − 490 clamped to 0           |

   For step 3a the starting `pwmCtrl` is reset to 500 first. Step 3 codifies the documented "EN-before-IN on
   direction<0" behavior — if the test fails, the shim's subtract-stored-HS logic at `src/pwm/vconv.h` is broken or the
   firmware's ordering assumption in `buck.h::pwmPerturb` has shifted.

   **Numerical / stability:**
   - **Output-node stability at small R_bat.** With backward-Euler on V_out, sweep `r_bat ∈ {1e-3, 1e-2, 0.05, 1, 1e9}`
     at default `c_out` and `pwm_freq`; assert step response is bounded and monotone in every regime (no oscillation,
     no blow-up). The short-circuit (1 mΩ) and open-circuit (1 GΩ) endpoints both pass — locks BE in against an
     accidental forward-Euler regression.
   - **`vbatAcPhase_` wrap.** Assert phase stays in `[−2π, 2π]` after 1e6 cycles at `vbat_ac_freq=1 Hz` (slow phase
     accumulation is where float precision bites).
   - **No hidden L derate.** Compute `ΔIL_pp` from `vconv.conf` `L` (== `coil.conf::L0`) and compare to the model — the
     plant must use nameplate L exactly, with the firmware's 5% `InductivityDcBias` showing up only on the controller
     side as a real mismatch.
   - **Determinism.** Seed plant state, run M cycles, capture full state (`vIn_, vOut_, iLEnd_, iInAvg_, iOutAvg_,
     vbatAcPhase_`); reset, run again with identical inputs, assert bit-identical output. Pins the spec promise that
     the per-cycle solver is deterministic and noise lives only in `ADC_VConv`.
   - **N-cycle stepping equivalence.** Assert `stepSeconds(N · T, …)` and `N × stepOneCycle(T)` produce identical state
     to float precision. Pins the integration path that `ADC_VConv` actually exercises against the path the rest of
     these unit tests use.
   - **Mains-ripple transfer to V_out.** Set `vbat_ac_amp=1, vbat_ac_freq=100`, run long enough for the transient to
     die (`≫ R_bat·C_out`), capture V_out over an integer number of ripple periods, FFT (or sine-fit). Assert the
     100 Hz component has amplitude `|1 / (1 + j·2π·100·R_bat·C_out)|` to 5% and 0 Hz component ≈ V_bat. Pins (a) the
     ripple-injection wiring at `vconv.cpp:35`, (b) the BE filter's frequency response, (c) a known reference amplitude
     for layer 2's notch-rejection test to compare against. The new `getVbatAcAmp()`/`getVbatAcFreq()` getters let the
     test verify config before measurement.

   **Reverse-current regime:** force LS-on past the zero crossing (set `pwmRect` such that the analytic phase-2 ends
   with `I_L_after_LS < 0`). Run N cycles, assert:
   - V_in rises, V_out drops, I_bat goes negative (battery sources) — confirms HS-body-diode pumping path.
   - **Bidirectional charge balance:** `iInAvg·Vin + iOutAvg·Vout ≈ 0` over one settled cycle (lossless plant, energy
     flows source → sink reversed). Pins the path quantitatively, not just by sign.
   - **`iInAvg` sign + magnitude** match the analytic phase-1 triangle area `(a + b)·tHS/(2T)` with `a < 0, b = 0` to
     1%. Documents whether negative `iInAvg` is the physically-correct "HS body-diode reverse pump" reading, or a
     plant artefact that needs clamping.

   **Punch list (concrete assertions):**

   | # | Test                            | Quantity                                                | Expected                       | Tol          |
   |---|---------------------------------|---------------------------------------------------------|--------------------------------|--------------|
   | A | CCM volt-sec balance            | `Vout/Vin` after settle                                 | `tHS/(tHS+tLS)`                | 0.5%         |
   | B | CCM ripple amplitude            | `max(IL) − min(IL)` settled                             | `(Vin − Vout)·tHS/L`           | 1%           |
   | C | Energy conservation             | `Vin·iInAvg / (Vout·iOutAvg)`                           | 1.0                            | 1%           |
   | D | D=0 idle                        | `iInAvg, iOutAvg, iLEnd` @ 1000 cyc                     | 0, 0, 0                        | exact        |
   | E | D=1 saturation                  | `tOff=0`, `dcm_==false`, IL monotone                    | —                              | —            |
   | F | Iload≈0 (Vbat = Vout_target)    | steady `iOutAvg`                                        | `(Vout − Vbat)/Rbat`           | 1%           |
   | G | BE stability vs r_bat           | step response bounded & monotone for r_bat ∈ {1e-3 … 1e9} | —                            | —            |
   | H | Reverse-pump charge balance     | `iInAvg·Vin + iOutAvg·Vout`                             | 0                              | 1%           |
   | I | `iInAvg` in reverse regime      | sign + magnitude vs analytic triangle                   | match                          | 1%           |
   | J | DCM boundary                    | sweep Vbat at fixed PWM, find Iout where `dcm_` flips   | matches `ΔIL/2 = Iout`         | 1 sweep step |
   | K | Phase wrap                      | `\|vbatAcPhase_\| ≤ 2π` after 1e6 cyc @ 1 Hz            | —                              | —            |
   | L | PV boundary                     | `pvCurrent(0), pvCurrent(Voc), pvCurrent(V>Voc), pvCurrent(V<0)` | `Isc, 0, 0, Isc`       | exact        |
   | M | PV Newton convergence           | `dP/dV @ V=k·Voc` for `k ∈ {0.5,0.75,0.85,0.95}`        | 0                              | 0.5%         |
   | N | Shim wiring sequence            | 7-step `update_pwm` table → final `(pwmCtrl, pwmRect)`  | per table                      | exact        |
   | O | Error latch                     | `pwmCtrl+pwmRect > pwmMax` → `errored()`, `iIn=iOut=0`  | true, 0, 0                     | exact        |
   | P | Determinism                     | M-cycle state hash after replay                         | bit-identical                  | exact        |
   | Q | N-cycle equivalence             | `stepSeconds(NT)` vs `N × stepOneCycle(T)` state delta  | 0                              | exact        |
   | R | DCM volt-sec balance            | `Vin·tHS` vs `Vout·(tHS+tLS+tDecay)` settled            | equal                          | 0.5%         |
   | S | Degenerate phase durations      | `tHS=0,tLS>0` IL drains; `tHS>0,tLS=0` `iLEnd=0` in DCM | per bullet                     | exact / 1%   |
   | T | Mains-ripple → V_out            | FFT 100 Hz amplitude at `vbat_ac_amp=1`                 | `\|1/(1+j·2π·f·RC)\|`          | 5%           |

2. **On-target integration test** — flash `wokwi_mock` (or a new `vconv_mock` board config) with vconv enabled. Boot,
   let it run: MPPT tracker should find an MPP near `pv_k * Voc`, PD loops should regulate, charger should reach CV.
   Pass = no protection trips, MPP within ±10% of expected, Iout > 0 at MPP. A second pass with `vbat_ac_amp=0.5` +
   `noise_vin=0.02` must still converge — that's the "realistic bench" smoke test.
    - **Noise tolerance:** set `noise_vin=0.05` (V σ). Run steady-state with constant duty; assert the
      median+EWM-filtered V_in stays within ±2σ/√N_window of the deterministic plant value across a long window. Confirms
      the filter chain isn't degraded by added jitter.
    - **Notch rejection:** set `vbat_ac_amp=1.0`, `vbat_ac_freq=100`. Run the firmware's vout sensor chain over many
      cycles; assert post-notch ripple is attenuated ≥20 dB vs raw. With the notch disabled (or mis-tuned to e.g. 120
      Hz), assert the ripple leaks through. Confirms the notch is wired into the V_out path and actually tuned to
      `notch_f`.

The host-stub already has `converter-test.cpp` and infrastructure — `vconv-test.cpp` slots in next to it.

## Difficulties / risks

1. **Firmware mode-decision vs model physics.** The model has no DCM/CCM "mode" — it just integrates physics; `inDcm()`
   is a derived observation ("did the coil hit zero this cycle"). The firmware's `buck.h` *does* pick a mode (via
   `DcmEnterRippleRatio = 2.0` / `DcmExitRippleRatio = 1.8`) to gate sync-rect dither and decide its predicted LS-off
   timing. Risk is that the firmware's predicted DCM boundary doesn't line up with where the model actually shows the
   coil hitting zero — e.g. the firmware enters DCM but its `pwmRect` is still long enough that the model runs phase 2
   past the crossing into reverse, or vice versa. Tests pin a fixed PWM, sweep Iout, and assert (a) the firmware's mode
   flag transitions at the same Iout the model first shows phase-2 ending at zero, and (b) no chatter across the
   hysteresis band.
2. **Numerical stiffness near zero.** Cap dynamics + tiny voltages can blow up. Hard guards on V_in/V_out clamps
   (above).
3. **N-cycle convergence across ADC sample.** When duty steps by ±50 in one perturb, the model runs 10–15 cycles before
   the next ADC read — the firmware sees the *settled* state, not an intermediate transient. Real HW behaves similarly
   because the sensor filters smooth over many cycles. Acceptable.
4. **wokwi_mock test fallout.** ~~Anything that depended on `ADC_Fake`'s sinusoidal output will break.~~ Resolved by
   introducing a new `config/lab/vconv_mock/` board config (the chosen path); existing `wokwi_mock` and `dry_mock`
   keep using `ADC_Fake` unchanged.
5. **Test access to `g_vconv` state.** Unit tests inject scenarios via the console command (UART/MQTT). For deeper
   inspection (assert exact I_L_end values), the host-stub test uses the model directly without ADC_VConv.

## Out of scope (parking lot)

See `etc/virtual-converter/TODO.md` for the prioritised list. Headlines:

- **PV strings** — multiple parallel strings with different `isc/voc/k` so the combined IV curve has multiple local
  maxima. Stresses the MPPT global-sweep / re-sweep logic (partial-shading scenarios).
- Boost topology — same structure, sides swapped, parameterize on `isBoost` later.
- MOSFET losses (R_DS_on, switching loss) — adds a small Vh-Vl drop and skews efficiency.
- Cap ESR — see brainstorming notes; v1 omits, ripple realism improves with it later.
- Coil saturation / non-linear L — plant uses constant nameplate `L0` from `coil.conf`. The firmware applies its own 5%
  derate (`InductivityDcBias = 0.95`) internally to model worst-case saturation; the plant deliberately does **not**
  apply it, so the firmware's derate is tested as a real plant-vs-controller mismatch (especially around the DCM/CCM
  boundary). Modeling actual saturation curves is a v2 item.
- Multiple input sources / output sinks.
- Host-side firmware run — host-stub currently builds `SynchronousConverter` only; full firmware run would need a much
  larger host-stub (FreeRTOS, esp_timer, mcpwm). Not needed for vconv to be useful.

## Status

v1 landed (in-tree): pure model + PWM shim + ADC backend + `buck.h`/`main/CMakeLists.txt` gate
+ `setupSensors` recognising `*_adc=vconv` + `vconv.conf` parsing + `vconv` console command +
`config/lab/vconv_mock/` board config. Remaining: complete the host-stub test punch list
(currently three of the listed tests are implemented; the rest are still TODO) and run the
on-target smoke test.
