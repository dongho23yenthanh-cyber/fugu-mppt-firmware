---
name: project-wokwi-no-continuous-adc
description: "Wokwi doesn't simulate ESP32 continuous-mode (DMA) ADC; firmware's ADC_ESP32_Cont backend gets no samples"
metadata: 
  node_type: memory
  type: project
  originSessionId: 8f3751a3-c262-460a-bad4-487c1a773f7b
---

Switching `sensor.conf` to `adc=esp32adc1` in Wokwi *configures* fine — ADC pattern logs init, attenuation gets applied — but **no samples ever arrive** ("Never got a sample! Please check ADC"). Wokwi only simulates one-shot `analogRead()`-style ADC; the continuous DMA path (`adc_continuous_*`) used by `ADC_ESP32_Cont` (src/adc/adc_esp32_cont.h) is unsupported.

**Why:** Wokwi's ESP32 model implements the ADC RTC peripheral but not the DMA/continuous controller + notification ISR.

**How to apply:** for Wokwi keep `adc=fake` (or `adc_fake` per channel). For real-ADC simulation you'd need a new `ADC_ESP32_Oneshot` backend that timer-polls `adc_oneshot_read()`. Don't repeat the pot+esp32adc1 path expecting it to work.

Discovered: 2026-05-25 while wiring potentiometers to GPIO 36/34/39 in wokwi_mock_esp32. See [[project-wokwi-esp32-setup]].
