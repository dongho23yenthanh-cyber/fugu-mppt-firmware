---
name: feedback-filter-pipeline-order
description: "In this firmware's ADC chain the notch must run BEFORE the median; do not suggest the conventional \"median first\" reordering."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cdedff64-8ca5-498c-9def-132235e1891c
---

The ADC filter pipeline in `src/adc/sampling.h::Sensor::add_sample` runs in this order: calibrated raw → NotchFilter (biquad, ~100 Hz) → RunningMedian5 → EWM (2-pass EWMA) → AdaptiveNoiseFilter. I previously suggested swapping median ahead of the notch — the user pushed back and was correct.

**Why:** ADC sample rate is ~400 Hz and the dominant interferer is 100 Hz (rectified inverter, 2× line). fs/f0 = 4 means a 5-tap median spans 1.25 periods of the interferer. A median of a sparsely-sampled sinusoid is not a sinusoid — it scatters tone energy into harmonics (200/300 Hz). The downstream linear narrow-band notch can no longer cancel the tone once that has happened. The standard "median first" textbook advice assumes fs/f0 ≫ 10, which doesn't hold here. With notch first, the linear stage cleanly removes the tone, then the median despikes residual outliers and suppresses any biquad ringing on transients.

Related project constraint: notch is NOT removable on Vout for the inverter-fed topology — at kW power, ~1 mF bulk caps present ZC ≈ 1.6 Ω at 100 Hz, so ripple on the converter terminal is volts. The battery (mΩ) is the actual sink, and the converter sits between, so the controller would chase 100 Hz oscillation without digital notching. See [[project-inverter-fed-topology]] if/when that memory exists.

**How to apply:** When reviewing or modifying the filter chain in `Sensor::add_sample`, keep the notch ahead of the median. If recommending reordering, first check fs and the dominant interferer frequency — the "median first" rule only applies at high oversampling ratios.
