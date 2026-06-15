---
name: project-inverter-fed-topology
description: The charger is sometimes wired to inverter AC-DC terminals (not battery terminals) — 100 Hz ripple on Vout is real and large at kW.
metadata: 
  node_type: memory
  type: project
  originSessionId: cdedff64-8ca5-498c-9def-132235e1891c
---

This MPPT firmware can be deployed in a topology where the charger output (Vout) is connected to an inverter's DC bus rather than directly to a battery. In that case the 100 Hz ripple (rectified line 2×f, EU 50 Hz) is large — at kW load, even ~1 mF of bulk capacitance presents ZC ≈ 1.6 Ω at 100 Hz, so several volts of ripple are normal. The battery (mΩ impedance) is the only real low-pass; if the converter doesn't see through that ripple, its Vout control loop will chase it.

**Why:** This is why the firmware has a digital notch filter on Vout at 2×f_inverter (`PhysicalSensor::createNotchFilter` in `src/adc/sampling.h`). It's not optional in this topology — without it the CV controller would oscillate at 100 Hz. See [[feedback-filter-pipeline-order]] for the related ordering rule.

**How to apply:** Don't suggest removing the notch on Vout/Vin. If considering changes that affect the filter pipeline's behavior at 100 Hz (notch Q, cascade depth, adaptive tracking for off-grid frequency drift), remember the use case isn't just "DC battery with milliohm Z" — it can be "inverter DC bus with seconds of bulk-cap energy but no low-Z sink." Off-grid inverters can drift 1–2 % from nominal, so a Q=20 notch (±2.5 Hz @ -3 dB) may need to widen or track.
