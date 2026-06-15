---
name: project_ina226_alert_cvrf_rearm
description: INA226 conversion-ready alert needs the Mask/Enable read to re-arm; overflow does NOT trigger the pin
metadata: 
  node_type: memory
  type: project
  originSessionId: cebdf0f7-8c7e-44e0-b291-ae6a1aefb078
---

In `src/adc/ina226.h` the per-sample `MASK_EN` read in `hasData()` is **load-bearing and cannot be dropped** for speed. Two distinct reasons, only one of which applies:

- **Disambiguation: not needed.** INA226 alert sources (SOL/SUL/BOL/BUL/POL/CNVR, Mask/Enable bits 15–10) are mutually exclusive; only `enableConvReadyAlert()` (CNVR, bit 10) is active. OVF (overflow, bit 2) and the limit comparators are status flags / disabled functions — they do **not** assert the ALERT pin. So the pin firing always means "conversion ready"; you never read MASK_EN to find out *why*.
- **Re-arming: required.** CVRF (bit 3) clears ONLY on a Mask/Enable read (`readAndClearFlags`) or a Config write — NOT on the next conversion, and NOT by reading the BUS/CURRENT data regs (`getBusVoltage_V`/`getCurrent_A` don't touch it). In conv-ready mode the ALERT pin tracks CVRF, so without the read you get exactly one falling edge then silence. Proven by `testContinuousAlert()` calling `readAndClearFlags()` between consecutive edges, and by production sustaining ~450 SPS only because `hasData()` reads MASK_EN every cycle.

**Why:** I initially suggested dropping the MASK_EN read as "redundant 33% I2C savings" — wrong, user corrected me. The floor is 3 I2C reads/sample (MASK_EN re-arm + BUS + CURRENT); the `overflow` check is free since MASK_EN is read anyway. The only real latency lever is conversion time (`CONV_TIME` 1100→588, stubbed at ina226.h:163), not fewer reads.

**How to apply:** Don't propose removing the MASK_EN/flag read on the INA226 hot path. For latency, go after conversion time; for the shutdown bug see [[project_ina226_timeout_loop_latency_shutdown]]. No auto-re-arming conv-ready mode exists on INA226 (INA228/237 differ = hw swap).
