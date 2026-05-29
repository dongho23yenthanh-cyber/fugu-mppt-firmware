---
name: esp32cont-read-throws-and-no-sample-watchdog
description: ADC_ESP32_Cont::read() can throw into the RT loop on adc_continuous_read error; no no-sample watchdog so Wokwi (never samples) silently starves
metadata:
  type: project
---

Two RT-robustness gaps in `ADC_ESP32_Cont` (`src/adc/adc_esp32_cont.cpp/.h`), review-only 2026-05-29:

1. `read()` runs in the RT loop. On an `adc_continuous_read` ret other than OK/TIMEOUT it does
   `assert(ret==ESP_ERR_INVALID_STATE); ESP_ERROR_CHECK_THROW(ret)` (.cpp:139-140) — **throwing
   from the RT path is forbidden** (must stopAndBackoff). The sampler already checks
   `adc->isGood()` (sampling.h:449) and converts false→AdcError; prefer flipping an isGood flag
   over throwing.

2. No no-sample watchdog. `hasData()` = `notification.wait(1ms)` (.h:96). Per project memory the
   continuous ADC "inits but never samples" on Wokwi/classic-ESP32 — if conv-done never fires,
   `read()` is never entered and this backend silently yields zero samples forever (Vin/NTC dead,
   calibration never completes). Add a since-start no-sample timeout that flips isGood()=false.

Also (lower sev): the conv-done ISR path itself is CORRECT — `s_conv_done_cb`/`convDoneCallback`/
`notifyFromIsr` are IRAM_ATTR and only do vTaskNotifyGiveFromISR. The cali/scope/log work is in
`read()` which is the RT *task*, not the ISR, so it's legal there.

Other bugs found same review: `ch & 0x7` channel mask (.cpp:48,65) aliases ch8/ch9; `assert(ch <
ADC_CHANNEL_9)` (.h:100) off-by-one rejects valid ch9; deinit()/stop() don't null `handle`.

Related: [[esp32cont-samplingrate-dual-formula]]
