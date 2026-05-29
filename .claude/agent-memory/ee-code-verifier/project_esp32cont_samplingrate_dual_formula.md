---
name: esp32cont-samplingrate-dual-formula
description: ADC_ESP32_Cont::getSamplingRate recomputes pattern shape independently from start(); they agree only in NTC-present+duplicate-fits case, else notch mistuned
metadata:
  type: project
---

`ADC_ESP32_Cont` (`src/adc/adc_esp32_cont.h` / `.cpp`) computes the DMA channel-pattern
length in TWO independent places that can disagree:

- `start()` (.cpp:59) emits the HF-duplicate pattern (each non-NTC ch sampled twice/pattern)
  only when `hasNtc && (patLen-1)*2 <= SOC_ADC_PATT_LEN_MAX`.
- `getSamplingRate()` (.h:63-82) UNCONDITIONALLY assumes the duplicate exists:
  `chNum = 2*chNum; if (hasNtc) chNum -= 1;` and returns 2x rate for non-NTC channels.

**Why it matters:** `getSamplingRate(ch)` feeds the notch filter tuning
(`sampling.h:329` createNotchFilter → `notch->begin(100Hz / fs)`). The notch targets the
100 Hz (2x50 Hz rectified) inverter ripple on inverter-fed boards (fmetal/fry/flat). If the
two formulas disagree (no NTC on esp32adc1, or duplicate doesn't fit the pattern table), the
reported per-channel fs is ~2x wrong and the notch is mistuned / won't cancel the ripple.

**Fix direction:** store the actual patLen + per-channel appearance counts as members in
`start()`, derive `getSamplingRate` from them — single source of truth. Not yet fixed (review only, 2026-05-29).

Related: [[esp32cont-read-throws-and-no-sample-watchdog]]
