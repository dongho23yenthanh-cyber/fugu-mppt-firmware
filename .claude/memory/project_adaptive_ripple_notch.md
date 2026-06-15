---
name: project_adaptive_ripple_notch
description: "Inverter on fry's DC bus collapsed MPPT to ~130W; root cause + the adaptive-notch fix and its tests"
metadata: 
  node_type: memory
  type: project
  originSessionId: 8fbc2a0f-3068-45bb-86f7-5e64a34b515c
---

2026-06-06: turning on a ~2 kW inverter on fry's DC bus dropped output from ~500 W (healthy
midday MPPT, Vin pulled to Vmp ~61.6 V) to ~130–150 W with the tracker stuck near Voc (Vin ~73 V
of 75.9 V Voc), hunting 90→210 W. Influx `mppt` (fry) + scope capture
`etc/scope_client/fry/2000w-inverter-midday-20260606T131106Z`.

Root cause: the inverter injects a strong **~120–126 Hz** ripple (2×line; looks like a 60 Hz
inverter) onto Vout — 91 % of Vout's AC energy, also on iout(37%)/vin(22%). The MPPT notch was
**hard-coded to 100 Hz** (`inverterFreq=50` in `sampling.h::createNotchFilter`, Q=20 ≈ 5 Hz BW),
so it gave ~0 dB at 120–126 Hz. The tracker power = `ewm.avg(Vin)·ewm.avg(Iin)` is downstream of
the notch, so the un-notched ripple swamped the P&O perturbation → random-walk → parked near Voc.
This is exactly the [[project_inverter_fed_topology]] scenario.

Fix (committed to code): **adaptive notch**. `src/math/ripple_freq.h` = streaming multi-bin
Goertzel (`RippleFreqDetector`, 80–140 Hz, one-pole DC high-pass, peak/mean SNR confidence,
parabolic sub-bin). Runs entirely on the RT core fed by the Vout sample stream (no cross-core
coeff race); retunes every sensor's biquad notch to the detected tone when SNR≥15, slew-limited,
Nyquist-guarded. Conf keys in `sensor.conf`: `notch_adaptive` (default 1), `notch_freq`
(fixed fallback, 100), `notch_q` (20). Wired in `sensor_setup.cpp` (`configureNotch` +
`setRippleSource(sensors.Vout)`).

Tests: `test/host-stub/ripple-freq-test.cpp` (host, `c++ -std=gnu++17 -O2 -I src -I test ...`) and
on-target `test/test_ripple_freq.cpp` (Unity; includes an esp_dsp end-to-end test that the adaptive
notch leaves ≥4× less ripple than the fixed 100 Hz notch on the real capture). Real-data fixture =
`test/data/fry_vout_inverter_646.h` (8192 Vout samples, host-FFT tone 126.17 Hz). Host test detects
126.09 Hz SNR 161.

Shipped: committed `a99aec9` ("mppt: auto-tune inverter-ripple notch..."); on-target Unity suite
(127 tests, 0 fail) passed on a bench unit; OTA'd to **fry** 2026-06-06 as `fry-brk1-81-ga99aec95`.
fry healthy post-OTA (442 W MPPT, no ADC errors, lag ~3 ms). NOT yet validated under a running
inverter — the notch only retunes when a tone ≥SNR 15 is present; with the inverter off it stays
at the 100 Hz default (no false retune). To verify the fix: run the inverter and watch fry's log
for `ripple notch -> ~126Hz` + Vin pulling to ~61 V + power recovering toward ~500 W (vs the
~130 W collapse). flat NOT updated. On-device fingerprints to tell fry from flat: coil L0 80µH=fry
/ 51µH=flat; fry vout_rl=42137 (calibrated). NAT :232 was fry on 2026-06-06 (mapping not static —
always confirm by L0).

CONFIRMED LIVE on fry under a 2 kW inverter (batmon bat_caravan discharging ~2 kW, SOC 70→54%):
the on-device detector found the ripple at **~89 Hz** (snr ~140) and slewed the notch
100→94→91→90→89.5→89.2 Hz, then settled. **The real ripple is ~89 Hz, NOT the 126 Hz the scope
suggested** — the scope's per-channel sample rate is only estimated, so its absolute-Hz was off.
The on-device detector + notch share the SAME effectiveSampleRate, so the notch lands on the tone
regardless of both inverter-frequency uncertainty and fs-estimate error — a fixed 100 Hz OR a
fixed 126 Hz notch would both miss 89 Hz. fry held ~430 W / Vin ~63 V (Vmp) post-OTA vs the
~130 W / Vin ~74 V collapse earlier the same day. Caveat: fry's recovery in telemetry began
~13:54 UTC, before the 14:20 UTC OTA (old fw, likely a load/freq shift), so the OTA wasn't proven
to flip it live — but the notch tracking the true ~89 Hz tone is the durable fix.

flat ALSO OTA'd 2026-06-07 to fry-brk1-82-g6164333f (flag-on, adaptive notch + measured rate),
via **MQTT** (NAT/mDNS unreachable — flat flaps on weak wifi). Recipe: serve build/fugu-firmware.bin
over HTTP from Mac's 192.168.1.205:9000, then `fugu_console.py --mqtt 192.168.1.200 --mqtt-port 1882
--mqtt-user $MQTT_BROKER_USER --mqtt-pass $MQTT_BROKER_PASS --name flat -c "ota http://192.168.1.205:9000/fugu-firmware.bin"`.
flat's adaptive notch tuned to ~103-107 Hz (snr 17-79) — with measured-rate that's the true ~100 Hz
(50 Hz inverter), confirming the whole diagnosis on the 2nd board. flat L0=56µH (fry=80µH) — use to
disambiguate. flat lag dropped 130ms→3ms after reboot. Both converters now self-tune. `CONFIG_FUGU_INA226_MEASURED_RATE` is now **default y** (commit
195fdc9) so clean builds keep it — no silent revert to nominal rate; no-op on non-INA226 boards.

Periodic-sweep temp/power notches: flat's tracker re-sweeps (duty 0→max + sensor calibration)
every ~71 min (seen 09:07, 10:18, 11:30 local 2026-06-07). Each sweep shows as a brief power dip
AND a temperature notch in ntc+mcu (duty≈0 cools heatsink + the calibration/loop-timing perturbs
readings — ntc+mcu move together ~5°C in 60s = partly sampling artifact, not pure thermal). Cadence
set by tracker.conf. NOT a fault, NOT inverter-related.

Why "89 Hz" not 100: on fry **Vout is read from the INA226** (`vout_adc=ina226`, ch0=Vbus), and
`ADC_INA226::getSamplingRate()` returned the NOMINAL rate `1e6/(2*conv_us)` = 454.5 sps (default
conv 1100µs). fry's INA226 actually converts faster (~512 sps; code comment ina226.h ~L121 flags
it "probably fake"), so a real ~100 Hz (50 Hz inverter) tone at normalized 0.196 cyc/sample gets
LABELED 0.196*454.5≈89 Hz; the scope (assumed 646) labeled the same tone 126 Hz. The notch is
unaffected — it applies the normalized 0.196 to the same stream, so fs cancels and it sits on the
real tone regardless. Fix (commit 6164333): `CONFIG_FUGU_INA226_MEASURED_RATE` (default n) — when
set, `init()`/`resetPeripherals()` measures the real CVRF interval over ~16 conversions at
operational settings (`measurePairUs()`) and getSamplingRate returns `1e6/measuredPairUs_`,
correcting the notch label AND the expected_hz watchdog. NOT hardware-validated yet (bench unit
uses internal ADC, no INA226); validate by OTAing a flag-on build to fry/flat and checking the
"measured X SPS vs nominal" log + "ripple notch -> ~100Hz".

NOTE: scope per-channel sample rate is estimated, so 126 Hz could be a 60 Hz inverter's 120 Hz;
the adaptive notch makes the exact value moot. Related: [[reference_mppt_telemetry_influx]],
[[feedback_filter_pipeline_order]].

VERIFIED end-to-end (closed-loop vconv sim, 2026-06-07): the notch fixes TRACKING, not just
filters ripple. A/B on a bench S3 (WITH_VCONV, vconv_mock + vbat_ac_amp=0.26/noise_vout=0.03,
adc_freq=1000) — same ripple, only the notch changes: notch mistuned 200Hz → 570W @ Vin 72V
(stuck near Voc, the fry failure); notch on-target 100Hz → 809W @ Vmp 62.8V (≈ the 815W no-ripple
baseline). +42% power, operating point pulled Voc→Vmp. Earlier the A/B was BLOCKED because at
adc_freq=3000 the RT loop overran → TWDT reboot-loop under ripple, and adc_freq 1000/2000 hit a
gptimer divider assert. Both fixed by commit 495d317: PeriodicTimer (shared by ADC_Fake mock +
ADC_VConv sim, src/etc/rt.h) used resolution_hz=requestedHz/alarm_count=1 → prescaler overflow
below ~2.5kHz; now fixed 1MHz resolution + alarm_count=1e6/hz, so any adc_freq works. Lower
adc_freq also gives the RT loop slack → no TWDT under ripple. Power-estimate path is sound:
tracker power = ewm.avg(physical I)·ewm.avg(physical U), both notched (initSensors guarantees
physical, throws otherwise) — the "virtual Iin bypasses notch" worry was WRONG.

vconv inverter-noise model (commit 4ad1c85): the capture's Vout ripple is a near-PURE SINE at
2x line, ~0.6% RMS / 2.5% p2p of V_bat, 2nd harmonic only 0.7% — NOT |sin|. Physics: an inverter
draws cos(2*line) reactive power -> sinusoidal bus ripple; |sin| (~20% 2nd harmonic) is a
RECTIFIER-load shape. The vconv plant (src/sim/vconv.*) ripple is now a PLUGGABLE function:
`setBatRippleShape(float(*)(float phase))` — built-ins shapeSine/shapeAbsSin selected by
`vbat_ac_shape` (0/1) in vconv.conf; a new model is one free function, no plant edits. Fry profile
(default OFF in config/lab/vconv_mock): vbat_ac_amp=0.26 (peak V on ~28V), vbat_ac_freq=100,
vbat_ac_shape=0, noise_vout=0.03 (broadband residual). Host sim confirms amp=0.26 -> ~0.6% RMS
Vout. Regression tests in test/test_vconv.cpp (sine h2<5%, |sin| h2~20%, amp=0 silent, custom plug
h2~50%); full on-target suite 131/0 on a bench S3. [[project_vconv_fragility_mechanism]]
