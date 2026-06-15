---
name: project_stm32wb09_port
description: "STM32WB09KE port of the firmware — branch, scope, and the InEn PWM mapping decision"
metadata: 
  node_type: memory
  type: project
  originSessionId: 9d03547d-b34c-4040-a658-eb33c01a640a
---

Porting the Fugu MPPT firmware ESP32-S3 → **STM32WB09KE** (Cortex-M0+ 64 MHz, 512KB/64KB, native BLE 5.4, one advanced 16-bit timer, 12-bit SAR ~1 Msps). Chip chosen for cheapest BLE-native STM32 that has an advanced timer for the half-bridge (WB05/WB06 are cheaper but WB06 also has the advanced timer; WB09 picked for the most flash/RAM headroom).

**Work lives on branch `stm32wb09-port`** under `port/stm32wb09/` (additive; ESP32 `main` build untouched). Increment 1 = scaffolding + compat layer (Arduino/esp_log/esp_timer/freertos shims) + PWM & ADC driver skeletons + BLE-console stub + CMake/toolchain. **Does not compile yet** — needs `arm-none-eabi-gcc` + STM32CubeWB0 (neither installed on this machine).

**Scope:** RT control core (sampling→protection→PD→MPPT→buck PWM) + internal ADC + BLE NUS console + UART. OUT: WiFi/MQTT/telnet/FTP/InfluxDB (no radio for it), I²C sensors (dropped → internal ADC only).

**Timer reality (corrected):** WB09 has **NO TIM1**. Its timers are TIM2 (general-purpose, CH1-CH4), TIM16/TIM17 (1 channel + complementary + dead-time + break). `IS_TIM_ADVANCED_INSTANCE(WB09)==0`; the true multi-channel advanced TIM1 is on **WB06**, not WB09. So the earlier "WB09 has one advanced 16-bit timer" was wrong — its complementary/break capability lives in TIM16/17 (single-channel).

**Key PWM decision:** use **`InEn`** mode on **TIM2** — CH1=HS, CH2=LS, two INDEPENDENT channels both high from counter=0, off at own compare (`LL_TIM_OC_SetCompareCH1/CH2`). Preserves diode emulation (LS off early); the InEn gate driver inserts dead-time. Set `pwm_driver_logic=InEn`, `pwm_deadtime_ns=0`. Trade-off: TIM2 has no break input → hardware OC-trip needs TIM16/17 (single-ch, CCM only) or stays in software + driver DIS pin. `pwm_stm32_timer.h` is written against real LL and **compiles clean** against CubeWB0.

**Toolchain gotcha:** homebrew `arm-none-eabi-gcc` formula ships **no libstdc++** (C-only) — firmware needs the STL. Use the full Arm GNU Toolchain. Working g++ on this machine: the Renesas-e2studio-bundled GCC 13.2 at `/Applications/Renesas e2 studio*/toolchains/arm-gnu-toolchain-13.2.Rel1-*/bin/arm-none-eabi-g++` (has libstdc++). The `gcc-arm-embedded` cask downloaded but its .pkg needs GUI/privilege to finish installing.

**SDK:** `third_party/STM32CubeWB0` (gitignored), HAL+CMSIS-device+FreeRTOS submodules fetched. Linker `STM32WB09KEVX_FLASH.ld` + GCC startup copied into port/. Compile defs: `-DUSE_FULL_LL_DRIVER -DSTM32WB09`.

**Host min-sim BUILDS + RUNS** (`port/stm32wb09/app/main_min.cpp` + `src/sim/vconv.cpp`, plain `c++ -std=c++17 -I src`): inline P&O MPPT on the VirtualConverter plant, tracks MPP to Vin≈31.84V/231W (target Vmpp=32V). Uses pwmMax=1641 (WB09 TIM2 @39kHz/64MHz). Gotcha learned: driving the plant with full-complementary `rect=pmax-ctrl` back-feeds (reverse current); must size LS for diode emulation `rect≈ctrl*(Vin-Vout)/Vout`. NOT yet the firmware's MpptSampler/tracker (those need conf/sensor/logging deps).

**Core integration boundary:** firmware uses ESP-IDF-style `<freertos/FreeRTOS.h>` (prefixed) — added `compat/freertos/*.h` redirect shims to CubeWB0's unprefixed headers, plus esp_attr/esp_heap_caps/esp32-hal/esp_littlefs(stub) shims. ESP include surface for the core is small. Still needed to compile the 24k-LOC core: FreeRTOSConfig.h, littlefs→internal-flash conf, then link.

**buck.h hook:** added `#elif WITH_STM32_TIM → PWM_STM32_Timer`, and widened 3 guards to `WITH_MCPWM || WITH_STM32_TIM` (getDtTicks, setManualRect setLsOff, init bestTiming/dt/start); faultBrake stays MCPWM-only (nested #if). Inert when WITH_STM32_TIM undefined.

PWM resolution caveat: 64 MHz timer clock → ~10.7-bit duty at 39 kHz fsw (vs ESP32 MCPWM's 12-bit at 160 MHz). True 12-bit at SMPS freq needs HRTIM (G4/H7).
