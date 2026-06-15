---
name: project_fry_ina226_undereads_157
description: "RESOLVED/WITHDRAWN: the fry-vs-flat 1.57x coil-L gap is two DIFFERENT coils, not a fry current-sensor error; both INA226s are fine"
metadata:
  node_type: memory
  type: project
  originSessionId: 0a0ec245-91f7-480d-ac84-ef1b597813ea
---

Coil-inductance sweeps (etc/measure_coil.py, DCM method) gave **flat 50.9 µH** vs **fry 79.8 µH**
(medians, ~550 pts each), ratio 1.57. I originally (wrongly) assumed both boards ran the same coil
and concluded fry's INA226 was fake and under-read Iout 1.57×. **That conclusion is withdrawn.**

**Resolution (2026-05-22): they are different coils.**
- flat: 2× stacked KS130-060A, Al=122 nH/N², ~20–21 t → computed 48.8–53.8 µH. Measured 50.9 µH ✓
  — so flat's sensor AND the DCM method are validated against a known nameplate.
- fry: 2× KDM KS184-125A, Al=562 nH/N², documented 10 t → 56 µH. Measured ~80 µH; the documented
  56 (from fisi.py) is the suspect figure (implies ~12 effective turns). fry's true L ≈ 80 µH.

**Independent cross-check confirmed no sensor gain error.** Regress bat_caravan (ant24) pack shunt
current vs each charger's reported Iout on decoupled daytime windows: g_fry ≈ 1.0 ±0.1 (if anything
g_flat ≈ 1.06–1.07, flat under-reports ~6–7%). Low-R² days (inverter discharging) go collinear —
ignore. Assumes the BMS shunt is the truthful reference.

**How to apply:** set each coil.conf L0 to that board's own measured L — flat ≈ 51e-6 (its 56e-6 is
~10% high, the riskier direction), fry ≈ 80e-6 (its 40e-6 is over-conservative). No INA226
recalibration needed. The ±10–16% L wiggle on both is duty-pinned SR-timing reverse current, not
core saturation. See [[project_charger_shared_bms_and_flat_vout_cal]] (flat's Vout reads ~0.35 V low
— a separate axis). Documented in doc/Coil Inductance Measurement.md §7.
