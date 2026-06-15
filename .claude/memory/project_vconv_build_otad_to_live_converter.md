---
name: project-vconv-build-otad-to-live-converter
description: "A WITH_VCONV build OTA'd to a live converter = 0W but looks alive; check sdkconfig before OTAing fry/flat"
metadata: 
  node_type: memory
  type: project
  originSessionId: cab40c5b-c952-479f-affa-fb1476e21933
---

2026-05-30: fry+flat both stuck after an 18:15 OTA — endless SWEEP→`Vr-sensor-fail`→backoff, 0W, while the pack discharged ~42A. Root cause: the pushed `build/fugu-firmware.bin` was a bench build with `CONFIG_FUGU_WITH_VCONV=y` (+ `MEASURE_COIL=y`).

**Why:** `buck.h:13` selects the PwmDriver at compile time — `WITH_VCONV` → `PwmDriver = PWM_VConv` (sim plant, `pwm/vconv.h`) instead of the real `PWM_ESP32_ledc`. The real HS/LS gates are never toggled. But the ADC backends in sensor.conf stay real (esp32adc1/ina226), so Vin reads real Voc (~74V) and Vout the real battery — the device *looks* healthy (sampler running, sweeps), yet transfers zero power at any duty. The `Vr-sensor-fail` guard (mppt.h:545) is then CORRECT, not overzealous: disabling `reverse_current_paranoia` just let it ramp duty to ~98% with still 0 current and Vin pinned at Voc — proving a real no-conduction path.

**How to apply:** Before OTAing fry/flat, verify the build is production: `grep -iE 'WITH_VCONV|WITH_MCPWM|MEASURE_COIL' sdkconfig` — VCONV and MEASURE_COIL must be off (clean Kconfig defaults: BLE/NETW on, rest off). A vconv build is for bench/sim only. Symptom signature of a vconv build on real HW: 0W, Vin glued at Voc (no sag) across all duties, repeated `Vr-sensor-fail`. Confirm `reverse_current_paranoia` is read only at boot (set-config needs a restart to take effect). See [[project_vconv_on_esp32_classic]], [[project_ina226_timeout_loop_latency_shutdown]].
