*this document is an LLM generated placeholder*

# VirtualConverter (vconv) — Design

## Goal

A pure-software model of a DC-DC synchronous buck (later boost) converter, embedded into the firmware so the existing control code (`SynchronousConverter`, `MPPT_Tracker`, PD loops, charger) can be exercised on real ESP32 hardware without a physical power stage. Replaces `ADC_Fake` in mock board configs (`wokwi_mock`, `dry_mock`).

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

- **`PWM_VConv`** — replaces `PwmDriver` (the LEDC backend) at compile time. Implements the same surface as `PWM_Mock` (`pwmMax`, `init_pwm`, `update_pwm(ch, duty)`, `update_pwm(ch, hpoint, duty)`, `stop`). Each `update_pwm` call forwards into `g_vconv.setPwm(ch, hpoint, duty)`. Event-driven — no missed updates.
- **`VirtualConverter`** — the model. Pure C++ (no `<Arduino.h>`, no FreeRTOS, no `esp_*`). Owns the converter state (`V_in`, `V_out`, `I_L_end`), holds the configuration (passives + sources/sinks), exposes `setPwm()` (input) and `step(dt_seconds)` / `getVin() / getVout() / getIout() / getIin()` (output). Stepping is decoupled from PWM updates: `step` advances the model by N PWM cycles using the most recently latched PWM counts.
- **`ADC_VConv`** — implements `AsyncADC<float>`. Wraps the existing `PeriodicTimer` + `TaskNotification` pattern from `ADC_Fake` to schedule samples. On each tick: calls `g_vconv.step(1.0 / adc_freq)`, then `getSample(ch)` returns the channel's current model state. NTC channel returns a constant.

### Compile-time selector

In `src/buck.h`:

```cpp
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
```

CMake enforces `WITH_VCONV` and `WITH_MCPWM` mutually exclusive. `PWM_VConv` mirrors the LEDC-style API only; it does not implement MCPWM-specific calls (`setHsOff/setLsOff/forceShutdown/clearForce/oper/genHS/genLS/getDtTicks`). MCPWM-only code in `buck.h` already lives under `#if WITH_MCPWM`, so when `WITH_VCONV=1` those paths are excluded.

### Runtime selector

`ADC_VConv` is opted-in per channel in `sensor.conf`:
```
vin_adc=vconv    vin_ch=0
iin_adc=vconv    iin_ch=1
iout_adc=vconv   iout_ch=2
vout_adc=vconv   vout_ch=3
ntc_adc=vconv    ntc_ch=4
```

Channel-to-quantity mapping is fixed in `ADC_VConv::getSample()`:

| ch | quantity |
|----|----------|
| 0  | `V_in`   |
| 1  | `I_in_avg` |
| 2  | `I_out_avg` |
| 3  | `V_out`  |
| 4  | NTC (constant `25.0` °C-equivalent voltage) |

## Model

### State

Three scalars carry all state between steps:

- `V_in` — input cap voltage (V)
- `V_out` — output cap voltage (V)
- `I_L_end` — coil current at the end of the previous PWM cycle (A). Signed; reverse current allowed.

### PWM input

`PWM_VConv` writes into a small struct, owned by `g_vconv`, updated under no lock (single producer = `SynchronousConverter::pwmPerturb` on RT core, single consumer = `g_vconv.step` also on RT core via `ADC_VConv::getSample`):

```cpp
struct PwmState {
    uint16_t pwmMax;
    uint16_t pwmCtrl;   // HS on-count  (buck)
    uint16_t pwmRect;   // LS on-count  (buck); = update_pwm(1, duty) - pwmCtrl in HiLi logic
    uint32_t pwmFreq;   // Hz
};
```

LEDC's two API shapes are reconciled in `PWM_VConv::setPwm(ch, hpoint, duty)`:
- `ch=0` (HS / Ctrl): `pwmCtrl = duty - hpoint`
- `ch=1` (LS / Rect, HiLi): `pwmRect = duty - hpoint` (hpoint == pwmCtrl)
- `ch=1` (LS, InEn / `pwmEnLogic`): `pwmRect = duty` (caller passes hpoint=0 in this branch)

This matches the existing `PWM_Mock`'s `update_pwm` semantics in `src/pwm/mock.h:27-33`.

### Per-cycle solution (buck)

Each PWM period of length `T = 1/pwmFreq`:

```
t_HS = T * pwmCtrl / pwmMax     // HS on duration
t_LS = T * pwmRect / pwmMax     // LS on duration
t_off = T - t_HS - t_LS         // both switches off (DCM dead-time)
```

Coil current trajectory, starting from `I_L_end`:

```
Phase 1 (HS on, 0 <= t < t_HS):
    I_L(t) = I_L_end + (V_in - V_out) / L * t
    I_L_after_HS = I_L(t_HS)

Phase 2 (LS on, t_HS <= t < t_HS + t_LS):
    I_L(t) = I_L_after_HS + (-V_out) / L * (t - t_HS)
    zero-crossing time within phase 2:  t_zc = t_HS + I_L_after_HS * L / V_out
    if t_zc < t_HS + t_LS and I_L_after_HS > 0:
        # DCM diode-emulation: coil hits zero before LS turns off
        I_L_after_LS = 0       # firmware terminates LS before crossing
    elif I_L_after_HS <= 0:
        # already in reverse before LS even started — pathological, latch to 0
        I_L_after_LS = 0
    else:
        # CCM, or DCM where firmware deliberately held LS past zero crossing
        I_L_after_LS = I_L_after_HS - V_out / L * t_LS   # signed, may go negative

Phase 3 (both switches off, t_HS + t_LS <= t < T):
    If I_L_after_LS > 0:  LS body diode conducts, current decays at -V_out/L toward 0,
                          clamped at 0 (then idle for the rest of the period).
    If I_L_after_LS == 0: idle for the rest of the period.
    If I_L_after_LS < 0:  (forced-PWM reverse-current regime) HS body diode conducts,
                          current decays at +V_in/L toward 0, clamped at 0.

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
float b = I_L_end + (V_in - V_out) / L * t_HS;       // I_L_after_HS
float c = solve_phase2(b);                            // I_L_after_LS, with zero clamp
float area_HS = 0.5f * (a + b) * t_HS;
float area_LS = compute_phase2_area(b, c, t_LS, V_out);   // accounts for zero clamp
float area_off = c * (T - t_HS - t_LS);
I_in_avg  = area_HS / T;
I_out_avg = (area_HS + area_LS + area_off) / T;
I_L_end = c;
```

### Cap dynamics

Forward-Euler at PWM-cycle rate (per `stepOneCycle`):

```
V_in  += (I_pv(V_in)  - I_in_avg)  * T / C_in
V_out += (I_out_avg   - I_bat)     * T / C_out
I_bat = (V_out - V_bat) / R_bat     // small R_bat → stiff battery
```

Numerical guards:
- Clamp `V_in ∈ [0, Voc + 5%]` to keep `I_pv` exponential from blowing up.
- Clamp `V_out ∈ [0, 2*V_bat]` defensively.
- If `V_out < 0.1 V` or `V_in < 0.1 V`, skip the per-cycle math and just decay caps toward source/sink — the model isn't physically meaningful in that regime and the firmware already special-cases it via `MinRatioVoltage`.

### Sources / sinks

**PV (input):** single-diode-ish exponential, two knobs:

```
I_pv(V) = Isc * (1 - exp((V - Voc) / k))
```

Clamped to `[0, Isc]`. `k` controls MPP sharpness — `k ≈ 2 V` gives MPP near `0.8 * Voc` for typical Si.

**Battery (output):** stiff voltage source `V_bat` with small series `R_bat` (~0.05 Ω) so the cap dynamics see a derivative instead of a clamp δ-function. `I_bat = max(0, (V_out - V_bat) / R_bat)` — backflow protection clamped at the model level, matching what `backflow.h` does on real HW.

### N-cycle stepping

`ADC_VConv::getSample()` ticks at `adc_freq` (default same as `ADC_Fake`'s `adc_fake_freq`). Per ADC sample, `g_vconv.step(1.0f / adc_freq)` runs `N = round(pwmFreq / adc_freq)` cycles (typically 10–15). N is recomputed each step from the latched `pwmFreq` in case it changes (it won't, but the cost is one division).

## Configuration

New file `vconv.conf`, lives in board configs alongside `coil.conf`:

```
# PV source
isc=8.0           # short-circuit current, A
voc=40.0          # open-circuit voltage, V
pv_k=2.0          # exponential sharpness, V (smaller = sharper MPP)

# Battery sink
v_bat=28.0        # stiff battery voltage, V
r_bat=0.05        # small series resistance for stiffness, Ω

# Passives  (L is read from coil.conf::L0)
c_in=470e-6       # input cap, F
c_out=470e-6      # output cap, F

# Sampling tick (also reused as the cycle-step granularity)
adc_freq=3000     # Hz, matches ADC_Fake default
```

`vin_rh / vin_rl` etc. from `sensor.conf` still apply downstream — the ADC backend returns raw V at the ADC pin (post-divider equivalent), the `Sensor` `LinearTransform` scales it. To keep this simple: `ADC_VConv` returns physical V_in / V_out / I_in / I_out, and the divider/gain math in `sensor.conf` is set to identity (`*_factor=1.0`, no divider) in mock board configs. (Existing `wokwi_mock` and `dry_mock` already do something similar.)

## Console command

```
vconv                           # dump state: V_in, V_out, I_L_end, I_in_avg, I_out_avg, mode (CCM/DCM)
vconv pv <isc> <voc> [k]        # update PV params at runtime
vconv bat <v>                   # update battery voltage at runtime
vconv set <key> <value>         # generic setter (c_in, c_out, r_bat, ...)
```

Registered in `src/cli.cpp` alongside the existing commands. Dispatch reads / writes `g_vconv` directly. No persistence — `set-config vconv.conf <key> <value>` is the path for that.

## Tests

Two layers:

1. **Math unit test** — `test/host-stub/vconv-test.cpp`. Builds in host-stub mode (no Arduino, no IDF). Exercises:
   - Steady-state CCM: known V_in, V_out, D → expected I_L_avg
   - DCM zero-crossing detection: as Iout falls, model enters DCM at the same boundary buck.h does
   - Sweep: PV IV-curve has a peak at the expected V
   - Cap dynamics: step change in PV current → V_in settles with the right time constant
2. **On-target integration test** — flash `wokwi_mock` (or a new `vconv_mock` board config) with vconv enabled. Boot, let it run: MPPT tracker should find an MPP near `0.8*Voc`, PD loops should regulate, charger should reach CV. Pass = no protection trips, MPP within ±10% of expected, Iout > 0 at MPP.

The host-stub already has `converter-test.cpp` and infrastructure — `vconv-test.cpp` slots in next to it.

## Difficulties / risks

1. **DCM/CCM boundary chatter.** Mirror `buck.h::DcmEnterRippleRatio = 2.0` and `DcmExitRippleRatio = 1.8` to keep the model and firmware in lockstep. Tests pin a fixed PWM, sweep Iout, and assert no boundary oscillation.
2. **Numerical stiffness near zero.** Cap dynamics + tiny voltages can blow up. Hard guards on V_in/V_out clamps (above).
3. **N-cycle convergence across ADC sample.** When duty steps by ±50 in one perturb, the model runs 10–15 cycles before the next ADC read — the firmware sees the *settled* state, not an intermediate transient. Real HW behaves similarly because the sensor filters smooth over many cycles. Acceptable.
4. **wokwi_mock test fallout.** Anything that depended on `ADC_Fake`'s sinusoidal output will break. The fix: change `*_adc=fake` → `*_adc=vconv` in those configs, OR introduce a new `vconv_mock` board config and leave the existing fake configs alone. Recommend the latter for v1 — keeps the old configs as a fallback.
5. **Test access to `g_vconv` state.** Unit tests inject scenarios via the console command (UART/MQTT). For deeper inspection (assert exact I_L_end values), the host-stub test uses the model directly without ADC_VConv.

## Out of scope (parking lot)

- Boost topology — same structure, sides swapped, parameterize on `isBoost` later.
- MOSFET losses (R_DS_on, switching loss) — adds a small Vh-Vl drop and skews efficiency.
- Cap ESR — see brainstorming notes; v1 omits, ripple realism improves with it later.
- Coil saturation / non-linear L — current model uses constant L0 like firmware does.
- Multiple input sources / output sinks — single PV, single battery.
- Host-side firmware run — host-stub currently builds `SynchronousConverter` only; full firmware run would need a much larger host-stub (FreeRTOS, esp_timer, mcpwm). Not needed for vconv to be useful.

## Implementation order

1. `src/sim/vconv.{h,cpp}` — pure model. Test in host-stub against analytic expectations.
2. `src/pwm/vconv.h` — `PWM_VConv` shim forwarding to `g_vconv`.
3. `src/adc/vconv.h` — `ADC_VConv` backend.
4. `buck.h` selector + CMake gate.
5. `setupSensors` recognises `*_adc=vconv`.
6. `vconv.conf` parsing + parameter wiring.
7. Console command `vconv`.
8. New `config/lab/vconv_mock/` board config.
9. On-target smoke test: boot wokwi_mock or vconv_mock, verify MPPT finds MPP.
