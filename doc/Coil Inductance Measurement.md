*this document is an LLM generated placeholder*

# Measuring the Coil Inductance (`coil.conf::L0`)

The synchronous converter has no inductor-current probe. It still needs the coil inductance `L0`
to emulate a diode (decide CCM vs DCM and time the low-side turn-off — see
[Diode Emulation.md](Diode%20Emulation.md)). This note covers the theory of obtaining `L0`, how a
miscalibrated current sensor distorts the measurement, what a wrong `L0` does to diode emulation,
and how `etc/measure_coil.py` measures it from the three sensors the board already has
(`Vin`, `Vout`, `Iout`).

Symbols: `Vin`, `Vout` the converter terminal voltages; `M = Vout/Vin` the buck ratio; `D` the
high-side on-fraction of a switching period; `fsw` the switching frequency; `L` the inductance;
`Iout` the average inductor (= output) current; `ΔI` the peak-to-peak ripple current.

## 1. Background relations

In **continuous conduction (CCM)** the inductor sees `Vin−Vout` while the high side is on and
`−Vout` while it is off, giving a triangular ripple

```
ΔI = (Vin − Vout)·D / (fsw·L) ,   with D = M = Vout/Vin in CCM
   = Vout·(1 − Vout/Vin) / (fsw·L)
```

This is exactly `SynchronousConverter::rippleCurrent()` (`src/buck.h`), where `fL = fsw·L·0.95`
(the 0.95 `InductivityDcBias` undershoots `L` slightly — see §4).

The **CCM/DCM boundary** is where the valley of the ripple touches zero, i.e. the average current
equals half the ripple:

```
Iout,crit = ΔI/2 = Vout·(1 − Vout/Vin) / (2·fsw·L)
```

In **discontinuous conduction (DCM)** into a stiff voltage sink (a battery clamps `Vout`), the
current is a triangular pulse that returns to zero each cycle. Charging for `D·T` reaches
`Ipk = (Vin−Vout)·D·T/L`, discharging into `Vout` for `t2 = (Vin−Vout)·D·T/Vout`, then idle.
Averaging the pulse over the period gives the DCM transfer relation:

```
Iout = (Vin − Vout)·Vin·D² / (2·Vout·fsw·L)          (DCM, Vout clamped)
  ⟹  L = (Vin − Vout)·Vin·D² / (2·Vout·fsw·Iout)
```

At the boundary `D = M` the two collapse to the same expression — the DCM relation is the
inverse of `rippleCurrent()`.

```
CCM (heavy load) - inductor current never reaches zero
  i_L |    /\      /\      /\
      |   /  \    /  \    /  \        ripple  dI = (Vin-Vout)*D/(fsw*L)
 Iout |--/----\--/----\--/----\--     average current = Iout
      | /      \/      \/      \
    0 +/-------------------------> t  valley > 0  (continuous)
       |<-D*T->|

 boundary: the valley just touches 0   =>   Iout = dI/2

DCM (light load, Vout clamped by battery) - current returns to 0, then idles
  i_L |   /|       /|       /|        rise slope  (Vin-Vout)/L  during D*T
      |  / |      / |      / |        fall slope  -Vout/L       during t2
 Iout |-/--+----/--+----/--+----      average = Iout
    0 +/---+____/--+____/--+____> t        = (Vin-Vout)*Vin*D^2 / (2*Vout*fsw*L)
       D*T  t2  gap
```

Only DCM (right) is solvable for `L` from the three DC averages: the current pulse is fully
determined by `L` and the terminal voltages, so a measured average `Iout` pins down `L`.

## 2. Methods to obtain `L0`

### With a current probe / bench instrument

- **Impedance / LCR meter** — the gold standard. Off-circuit, gain-independent, and the only
  practical way to trace the full **DC-bias curve** (inductance vs DC current) with a bias-current
  fixture. Use this if the coil can be disconnected.
- **CCM ripple, scoped current** — run in CCM, capture the inductor-current triangle, read `ΔI`
  peak-to-peak, then `L = (Vin−Vout)·D / (fsw·ΔI)`. Works at any DC bias point, so it can also map
  the bias curve in-circuit. Needs HF current capture and trusts the probe's AC gain.
- **di/dt over a known interval** — apply a known voltage across the coil for a measured time and
  read the current slope; `L = V·Δt/ΔI`.

### Without a current probe (only `Vin`, `Vout`, `Iout`)

Four candidates, evaluated against this hardware (battery output, `fs = 511 sps` control rate):

| method | idea | verdict here |
|---|---|---|
| DCM voltage ratio | `Vout/Vin` depends on `L` in DCM | **fails** — the battery clamps `Vout`, so the ratio is pinned by the pack, not by `L` |
| Output-voltage ripple | `ΔVout ≈ ΔI/(8·fsw·Cout)` | **fails** — switching ripple is invisible at 511 sps, swamped by 100 Hz mains ripple on an inverter-fed bus, and needs a trusted `Cout` |
| Load-step transient | current slew bounded by `V/L` | **impractical** — hard to command cleanly with a battery clamp + MPPT, unobservable at 511 sps |
| **DCM transfer relation** | invert `Iout = f(D, V, L)` | **works** — uses only the three DC averages; battery clamp is what *makes* it work |

The DCM transfer method is the one implemented (§5). Note the CCM/DCM **boundary** by itself is not
usable from DC data into a battery: with `Vout` clamped there is no kink in `Vout` vs load, and the
firmware's own `inDCM()` flag is computed *from* `L0`, so trusting it to find the boundary is
circular.

## 3. How a broken current sensor affects the measurement

Let the current sensor have a linear gain error `g`, reporting `Iout_meas = g·Iout_true` (`g < 1`
under-reads). Because `L` enters the DCM relation only through `Iout`:

```
L_meas = (Vin−Vout)·Vin·D² / (2·Vout·fsw·Iout_meas) = L_true / g
```

A sensor reading **low** (`g < 1`) inflates the measured inductance; reading **high** shrinks it.
The error is a pure scale factor, so it is invisible in the run-to-run *repeatability* — a precise
sensor with a wrong shunt/calibration looks trustworthy yet biases `L_meas`.

A sensor **offset** (non-zero reading at zero current) does not scale; it dominates at low current,
where it makes `L_meas` blow up (since `L ∝ 1/Iout`). This is why the script discards near-zero
points (§5).

These are not hypothetical: a counterfeit INA226 with a non-standard shunt LSB produces exactly a
clean gain error — see [dev-notes/ina226.md](dev-notes/ina226.md). The chart below is a real sweep
of two otherwise-identical boards sharing the same coil; one has a genuine INA226, the other a
suspected fake reading ~1.6x low. The fake board's curve is shifted up by that gain (`L_true/g`),
and both boards' lowest-current points are inflated by the sensor offset:

```
 L /µH                                    F = fry point (fake INA226)
 140|F                                    f = flat point (genuine INA226)
    |
 120|    F
 100|       F
    | ------F--------F-------F-----F-----  fry  median ~85 uH  (= true L / g)
  85|         F          F
  80|        F                 F
    |
  60| ....................................  56 uH nameplate (true L)
  51| ---f--f------f--f------f--f------f--  flat median ~51 uH
  45|   f       f        f          f
  40| f
    +--+----+----+----+----+----+----+---> Iout /A
      0.1  0.3  0.5  0.7  0.9  1.1  1.3
       ^ low-current points inflated by the sensor offset (L ~ 1/Iout)
```

The genuine board recovers the nameplate inductance (the `~51` vs `56` gap is tolerance plus a
little DC-bias droop, §5); the fake board's `~85` is purely the current-gain error.

**Self-consistency note.** If `L0` is *derived from* the same biased sensor and then used by the
firmware, the gain cancels for diode emulation specifically — see §4.

## 4. How a wrong `L0` affects diode emulation

`L0` feeds only `rippleCurrent()` → `computeDCM()` (`src/buck.h`). The low-side turn-off ratio
`rectCtrlRatio(M) = 1/M − 1` depends on the voltage ratio alone, not on `L0`. So `L0`'s entire
influence is the single decision

```
in DCM when   ΔI_fw(L0) > 2·Iout      (with hysteresis)
```

and `ΔI_fw ∝ 1/L0`. The two error directions are **asymmetric**:

- **`L0` too high** → `ΔI_fw` underestimated → the converter believes current is continuous when it
  is actually discontinuous → the low side is left on past the zero crossing → **reverse current**.
  Energy flows back, the switch node boosts, and the low-side switch (and anything on the input)
  can be destroyed. This is the dangerous direction.
- **`L0` too low** → `ΔI_fw` overestimated → DCM is declared too eagerly → the low side turns off
  early even in CCM; current commutates to the body diode for the remainder. **Safe**, but extra
  body-diode conduction loss (lower efficiency near the boundary).

That asymmetry is why the firmware deliberately undershoots with `InductivityDcBias = 0.95`: erring
toward "too low" trades a little efficiency for safety margin. When in doubt, round `L0` down.

**Why the sensor-gain error cancels here.** If `L0 = L_true/g` (measured with a sensor of gain `g`)
and the firmware compares against the *same* sensor's `Iout_meas = g·Iout_true`:

```
ΔI_fw(L0) = g · ΔI_phys / 0.95        (∝ 1/L0)
2·Iout_meas = g · 2·Iout_true
```

`g` appears on both sides of the inequality and divides out — the DCM decision is correct despite
both `L0` and `Iout` being wrong. Consequences:

- On a board with a biased sensor, the *self-measured* `L0` (e.g. the inflated value) gives correct
  diode emulation; substituting the physically-true `L0` would actually mistime it.
- The cancellation is local to diode emulation. Everything that uses `Iout` in an absolute sense —
  reported power, energy metering, charge-termination current, the `iout_max` cutout — stays wrong
  by `g`.
- Only the linear gain cancels; a sensor offset/nonlinearity does not.
- `L0` and the sensor are therefore coupled: fixing the sensor calibration requires updating `L0`
  to the true value in the same step, or the previously-canceling pair becomes a real error.

## 5. The measurement script (`etc/measure_coil.py`)

A host tool that drives the firmware console (serial / TCP-telnet / BLE) via the `fugu` package. It
implements the DCM transfer method because, per §2, that is the only one that survives a battery
load with the sensors present — and it reuses constants the firmware already holds, inverting its
own `rippleCurrent()`.

Procedure:

1. Read `fsw` from `board.conf::pwm_freq`; obtain `pwmCtrlMax` from the `dc` out-of-range reply and
   derive the PWM period `pwmMax = pwmCtrlMax / (1 − MinDutyCycleLS)` (buck); read the current
   `coil.conf::L0` for reference.
2. Read an idle status line for `M = Vout/Vin`; require `Vin > Vout` (buck headroom / sun).
3. Sweep the high-side duty count `H` upward across the DCM band (`--lo`..`--hi` × `M·pwmMax`),
   holding each step `--dwell` seconds and median-averaging the streamed status line
   (`Vin`, `Vout`, `Iout`, the actual applied `H`, and the firmware CCM/DCM flag). Staying below
   `M` keeps the converter in DCM and the current bounded; the sweep stops on the firmware
   reporting CCM or `Iout` exceeding `--i-max`.
4. Per point compute `D = H/pwmMax` and `L = (Vin−Vout)·Vin·D² / (2·Vout·fsw·Iout)`.
5. Discard points below an `Iout` floor (sensor offset territory, §3) and report the median of the
   rest, plus an IQR spread. Restore MPPT (or `dc 0`) on exit.

Robustness checks built in: `pwmMax` is cross-checked three ways (`pwmCtrlMax`, `pwmRectMin`, and
the LEDC `clock/fsw` resolution must agree); a flat `L` vs `D` across the band confirms the duty
scale (a duty/dead-time error would show as a trend); all-CCM points warn that `forced_pwm` may be
on (which suppresses DCM).

Limitations:

- **Low-bias only.** Measurements are taken well below the boundary current, so the result is the
  near-unbiased small-signal inductance. The method cannot trace the DC-bias curve — pushing the
  current up crosses into CCM, where the transfer ratio no longer depends on `L`. For the bias
  curve use a scoped CCM-ripple measurement or an LCR meter with bias injection (§2).
- The result scales with the **`Iout` calibration** (§3) and with **`pwmMax`**; dead-time and DCR
  bias it a few percent low.
- The reported value is the *physical* inductance — put it in `coil.conf` as `L0` directly; the
  firmware applies its own 0.95 bias.

Example:

```bash
python etc/measure_coil.py --ip 192.168.4.2 --i-max 1.0
python etc/measure_coil.py -p /dev/cu.usbmodem1101 --steps 12 --dwell 6
```
