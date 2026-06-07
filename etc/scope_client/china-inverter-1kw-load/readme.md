# cheap-China-inverter 1 kW load

A nasty cheap inverter on the DC bus that draws **dense, narrow current pulses** instead of a clean
sine. Raw ADC counts, ~4000 samples/channel (`vin/vout/iout/ntc/ucTemp.csv`); `img.webp` is the
scope view. `filt.py` (sibling dir) prototyped a median de-spiker against `iout.csv`.

## what it is

The disturbance is concentrated on the **current** (iout), mild on the voltages:

| chan | mean | σ | p2p | character |
|------|------|----|-----|-----------|
| iout | 1570 | **264 (17%)** | 1211 | dense spike train |
| vout | 2491 | 30 (1.2%) | 98 | small ripple |
| vin  | 1616 | 21 | 90 | slow envelope |

Spectrally, iout has a sharp line at **~0.22 cyc/sample** (pulses every ~4.5 samples) with a
sub-harmonic at ~0.11 (asymmetric double-pulse) and harmonics that **alias past Nyquist** back into
the control band. Crest factor ~2.5 — totally unlike the fry inverter's near-pure sine.

## model (vconv)

Added ripple **shape id 2 = `shapeSpiky`** to the vconv plant (`src/sim/vconv.h`): a narrow
per-cycle pulse `((1+cos)/2)^8`, zero-mean, crest 2.5, h2/h1 ≈ 0.70 (the peakiest built-in shape).
Reproduce with `vconv.conf`: `vbat_ac_shape=2`, `vbat_ac_amp~0.3`, `vbat_ac_freq~660`
(~0.22 cyc/sample at `adc_freq=3000`). Tests: `test/host-stub/vconv-test.cpp` (testU, crest factor)
and `test/test_vconv.cpp` (`test_vconv_spiky_ripple_is_harmonic_rich`, h2 > 0.5).

## filter finding + fix

Replaying the device pipeline (`notch → median5 → EWM`) on this capture exposed a bias the median
hides on clean loads: a dense periodic spike train is **signal, not glitches** — the unconditional
median-of-5 discards the real pulse charge and reads the current **~4.5% low** (EWM-series bias
−70 of 1570). It also doesn't beat a plain mean on variance.

Fix (conf-gated, default off): **glitch-safe / decision-based median** (`sensor.conf::despike`,
0 = off, ~8 enables). A sample passes through unchanged unless it is an extreme outlier
(`|v−median| > despike · running mean-deviation`), so recurring load pulses reach the mean
(unbiased) while genuine impulse glitches are still clipped. On this capture it takes the current
bias −70 → ~0 while a lone injected anomaly is still caught; on the fry sine case it is a no-op
(8/8 sparse glitches still rejected, sine undistorted). A rate-limited console WARN reports when it
clips. See `src/adc/sampling.h::Sensor::add_sample` and `test/host-stub/despike-test.cpp`.
