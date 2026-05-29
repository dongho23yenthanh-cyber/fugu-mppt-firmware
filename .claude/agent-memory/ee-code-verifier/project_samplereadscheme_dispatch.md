---
name: samplereadscheme-dispatch
description: How AsyncADC SampleReadScheme (cycle/any/all) maps to ADC_Sampler dispatch, and the EE latency traps per scheme
metadata:
  type: project
---

`AsyncADC<float>` (src/adc/adc.h:5-9) declares 3 read schemes; `ADC_Sampler::_updateAdc` (src/adc/sampling.h:411-468) dispatches on `adc->scheme()`:

- **cycle** (ADS1115 ads.h:40, Fake mock.h:21, Dummy): round-robin one channel per `update()`. Sampler owns `cycleSensors` + `cycleSensorsPos`; getSample() returns current ch, then advances pos and `startReading(next)`. Handshake = startReading (request) ... hasData (isReady poll) ... getSample. Channel-switch settling on the muxed ADS happens between updates.
- **any** (ESP32 continuous DMA adc_esp32_cont.h:26): DMA fills a buffer; `read(callback)` fires per-sample with `chToIdx[ch]`. startReading is a no-op. fs per channel = sampleRateHz/numChannels (adc_esp32_cont.h:34).
- **all** (INA226 ina226.h:65): on conversion-ready alert, sampler loops *every* registered channel calling startReading(i)+getSample(i). INA226 `hasData()` (ina226.h:74-81) does the actual I2C `readAll()` (bus+shunt+current) then getSample() just returns the cached field.

EE traps found in review (2026-05-29):
- **"vout last" guarantee only holds for scheme=all and scheme=any.** For scheme=cycle, vout is sampled once per N updates in round-robin, NOT last-every-tick. OVP latency = (N-1) sample periods worst case. The "vout last" invariant from CLAUDE.md is a per-tick property only the all/any schemes satisfy.
- INA226 `hasData()` (ina226.h:364-399) DOES read MASK_EN (`readRegister(INA226_MASK_EN_REG, 2)`, line 379) every accepted alert; per datasheet that read clears CVRF and re-arms the alert pin. Consistent with [[ina226 alert read re-arms CVRF]]: the MASK_EN read is load-bearing, must not be dropped. The commented-out rewrite-MASK_EN block (lines 392-396) is correctly unnecessary.
- `_readNext` (sampling.h:267-271) picks the *first non-null* channel via find_if, ignoring cycleSensorsPos -> only correct for all/any priming, benign for cycle since begin() re-primes cycle separately (sampling.h:285-289).
