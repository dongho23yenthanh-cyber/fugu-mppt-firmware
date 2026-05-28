# Real-time performance on ESP32 with Wi-Fi

With networking enabled, the ESP32 executes code that is not real-time capable and thus can block for a couple of
milliseconds.
Luckily, we have 2 cores, so we can use one core for all the non-RT and the other for the RT code.

The DC-DC converter control loop needs to have a fast load transient response to minimize transient surge voltages at
the output. Latency is the time between an input change and a response to this change at the output.

A control loop might look like this:

```
void criticalTask() {
  while(true) {
    adcRead();
    pwmWrite();
    yield();
  }
}
```

Latency should be deterministic. it is the maximum.
On a general purpose CPU, a lot of things can happen besides our critical task

## Real-time loop

Here's a rough pseudocode of how to achieve good real-time performance on the ESP32 while Wi-Fi is enabled:

```
void adcAlertInterrupt() {
  vTaskNotifyGiveFromISR(controlLoopTask);
}

void controlLoop() {
  while(true) {
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1));
    adcRead();
    updateControl();
    pwmWrite();
  }
}

void networkLoop() {
  // wifi stuff and everything else not RT-critical
}


void main() {
    controlLoopTask = createTaskCore1(controLoop, {.prio=20});
    networkLoopTask = createTaskCore0(networkLoop);
}

```

* `controlLoop` is our time critical task. we want the response time, i.e. the time the uC takes to react on an analog
  input change to the output, be less than 1 millisecond
* `core1` is our real-time core, everything that is not related to the controlLoop or can block longer runs on `core0`
* calling `ESP_LOGx(...)` usually writes `UART` and/or USB JTAG, which may block longer
* use a (non-blocking) queue to defer calls from the `controlLoop` to `networkLoopTask` (e.g. logging)
* `controlLoop` runs exclusively on `core1` with elevated priority
* notice that `controlLoop` doesn't call `yield()` or `vTaskDelay()`. `ulTaskNotifyTake` will block while ADC is busy,
  so FreeRTOS housekeeping (`IDLE` task) can run. TODO: specify housekeeping, what does idle task do?
* instead of semaphores we use task notifications which are faster according to FreeRTOS documentation

## ESP32(-S3) internal ADC

With the esp-idf API `esp_adc/adc_continuous.h` we cannot program the ADC conversion time. It appears to be always
working at the shortest possible time. This is why single shot measurements are quite noisy and it is better to use
continuous DMA reading and averaging with the highest possible sampling rate (83kHz for ESP32-S3).

Reading the DMA ring buffer from the "big" control loop might be to slow and we loose samples.
It might be useful to add another critical loop with even higher priority than the control loop that just reads and
averages the ADC samples.

Additionally, in this adc averaging loop we can implement a fast shutdown path to further reduce the response time
to OV or OC transients (load disconnect or short-circuit).

## Set explicit core affinity

```
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y
CONFIG_LWIP_TCPIP_TASK_AFFINITY=0x0
CONFIG_PTHREAD_DEFAULT_CORE_NO_AFFINITY=0x0

CONFIG_ARDUINO_RUNNING_CORE=0
CONFIG_ARDUINO_RUN_CORE0=y
CONFIG_ARDUINO_EVENT_RUNNING_CORE=0
CONFIG_ARDUINO_EVENT_RUN_CORE0=y
CONFIG_ARDUINO_SERIAL_EVENT_TASK_RUNNING_CORE=0
CONFIG_ARDUINO_SERIAL_EVENT_RUN_CORE0=y
CONFIG_ARDUINO_UDP_RUNNING_CORE=0
CONFIG_ARDUINO_UDP_RUN_CORE0=y
```

* assume networking code runs on core0.
* we want to run the latency -sensitive loop on core1.
* arduino's loop() is not RT capable because it does UART stuff between calls (do not use)
* so run arduino and the network on core0, and the loopRT on core1

## Why RT_CORE=1 (core1), not core0

ESP32-S3's two LX7 cores are functionally symmetric for a control loop, so the choice
isn't about raw throughput. The concrete reason to keep RT on core1:

- NVS / littlefs / OTA writes happen from core0 (services, console). Each flash write
  briefly disables the CPU cache; code on the *same* core stalls during that window
  unless it lives in IRAM.
- With RT on core1, those writes don't dip the RT loop — core1 keeps executing the
  `IRAM_ATTR`-marked ADC continuous-DMA ISR + RT code while core0 stalls.
- If RT moved to core0, every `set-config` / OTA chunk / coulomb-counter persist would
  briefly steal cycles from the ADC/MPPT/PWM path.

Flipping RT_CORE to 0 would also require flipping every `CONFIG_*_PINNED_TO_CORE_0` and
`CONFIG_*_AFFINITY_CPU0` to its `_1`/`CPU1` counterpart — significant sdkconfig churn for
no gain. The `xPortGetCoreID() == 0` asserts in `loopNetwork_task` would also need to
become `RT_CORE ^ 1`.

## esp_timer ISR placement

Default in IDF is `CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0` (ISR on PRO_CPU). An earlier sdkconfig
flipped that to CPU1 so any `dispatch_method = ESP_TIMER_ISR` callbacks would run with
RT-core latency. This firmware doesn't use ESP_TIMER_ISR dispatch anywhere — IDF's internal
esp_timer consumers (Wi-Fi keepalives, MQTT, NimBLE GAP, FreeRTOS-timers-via-service-task)
all dispatch to the task on CPU0. Net effect of the old placement: the RT path ate periodic
timer-ISR preemption for callbacks that ran on the other core anyway.

Current setting: `CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0=y` (ISR away from RT_CORE). Re-enable
CPU1 affinity if a future safety callback (e.g. high-rate OV/OC watchdog) needs to dispatch
in ISR context on the RT core.

The check macros in `src/etc/rt_core_check.h` enforce this and the other task placements
at compile time — adding a Kconfig that drifts will fail the build.

## GPIO alert ISR placement (INA226 / ADS)

The INA226 (and ADS1x15) alert pin drives a GPIO interrupt whose handler does
`vTaskNotifyGiveFromISR(loopRT)` to wake the RT sampler. If that GPIO ISR runs on **core0**, the
notify is cross-core: it has to raise a scheduler interrupt on core1, adding latency and jitter to
the wake path that the RT loop blocks on. We want the alert ISR on **RT_CORE** so the notify is
local.

The catch: the GPIO ISR is a single shared service, not per-pin. `arduino-esp32`'s
`attachInterrupt()` *lazily* calls `gpio_install_isr_service()` on the first attach, pinning that
shared service to whatever core called it. The first `attachInterrupt` happens in `setupSensors()`,
which runs in `setup()` on **core0** — so by default every alert ISR lands on core0.

To control it we pre-install the service on RT_CORE *before* any `attachInterrupt` runs (gated by
`PIN_GPIO_ISR_TO_RT_CORE` in `main.cpp`); the later lazy install is then a no-op.

**Landmine — do not wrap `gpio_install_isr_service()` in `esp_ipc_call_blocking(RT_CORE, …)`.**
That function does its *own* internal `esp_ipc_call_blocking()` to the calling core (via
`gpio_isr_register` → `esp_intr_alloc` on the target core). Calling it from inside an IPC callback on
RT_CORE makes that core's single `ipc` worker wait on itself → **permanent deadlock in `setup()`**,
before `loopRT` even exists, so nothing reboots it. This was diagnosed via JTAG (loopTask blocked in
`esp_ipc_call_blocking`, `ipc1` blocked inside `gpio_install_isr_service`) and it silently bricked two
field units after an OTA. The correct way to run it on RT_CORE is a **short-lived task pinned to
RT_CORE** that calls `gpio_install_isr_service()` and notifies setup() when done — the `ipc` worker
stays free, the nested IPC completes, and the ISR lands on RT_CORE.

**IRAM:** the service is installed with `ESP_INTR_FLAG_IRAM`, so every per-pin handler added to it
must be IRAM-safe (`ina226_alert` is `IRAM_ATTR`). Verify any new alert handler is too, or drop the
flag.

This couples to the loop-latency shutdowns seen on fry/flat: when INA226 alert edges are missed/late
the RT sampler starves and the latency watchdog trips `stopAndBackoff`. Lower, deterministic wake
latency (ISR local to RT_CORE) reduces that pressure — the watchdog itself is correct, the starvation
is the bug.

## Note about configTICK_RATE_HZ

defaults to 1000 (1tick = 1ms).
this is the shortest amount of time a task can wait.
not recommended to set to 10000, as it has a lot of overhead.
consider 2000Hz ?
https://www.esp32.com/viewtopic.php?t=1341#p6082

## wdt

https://esp32.com/viewtopic.php?t=14477

##       

https://github.com/MacLeod-D/ESp32-Fast-external-IRQs

https://docs.espressif.com/projects/esp-idf/en/stable/esp32h2/api-guides/performance/speed.html#speed-targeted-optimizations
"In general, it is not recommended to set task priorities higher than the built-in Bluetooth/802.15.4 operations as
starving them of CPU may make the system unstable. For very short timing-critical operations that do not use the
network, use an ISR or a very restricted task (with very short bursts of runtime only) at the highest priority (24).
Choosing priority 19 allows lower-layer Bluetooth/802.15.4 functionality to run without delays, but still preempts the
lwIP TCP/IP stack and other less time-critical internal functionality - this is the best option for time-critical tasks
that do not perform network operations. Any task that does TCP/IP network operations should run at a lower priority than
the lwIP TCP/IP task (18) to avoid priority-inversion issues."

# esp32s2

* core1 is more performant than core0
* FastLED appears to have a significant lag (does it use bit banging?)

# instrumentation profiling of code latency

`gcc -pg`
https://stackoverflow.com/questions/7290131/how-does-gccs-pg-flag-work-in-relation-to-profilers
implement mcount for ESP32 (see esp32-semihosting-profiler)

https://github.com/MacLeod-D/ESp32-Fast-external-IRQs

# rtcount

```
rtcount_print :
key                          num       tot   mean    max maxNum
adc.update              36761737 201234640      5  34734 10528868
mppt.update             12244296 161811306     13    753 8302755
protect                 36732884 298683769      8    537 10520083
adc.update.hasData      36761736 1576494944     42    277 24928691

```

```
rtcount_print :
key                          num       tot   mean    max maxNum
mppt.update             18930352 396081207     20    755 18452797
mppt.startSweep               11      7598    690    722      7
adc.update.handleSensorCalib  56848970 104091131      1    435  53647
protect                 56790990 549161721      9    367      0
adc.update.hasData      56887286 1304901885     22    296 54448783
adc.update              56887286  81186758      1    294 55961535
loopNewData             56844290  83121435      1    293 51416048
adc.update.getSample    56848970 262392390      4    279 43230846
adc.update.AddSampleVirtual  18948096  67719059      3    259 18136411
adc.update.addSample    56848970 167718961      2    232 28838738
adc.update.pre          56887287  71026559      1  
```

```
rtcount_print :
key                                  num       tot   mean    max maxNum
adc.update.getSample           178010278 476241806      2  34601 107427244
mppt.update                     59288893 1005532037     16  34426 35777832
protect                        177865294 1406598724      7  32692 58894801
micros                         178154656 405998661      2  32623 156025214
mppt.startSweep                      253    183189    724    766    137
adc.update.AddSampleVirtual     59306279 203131843      3    538 43849404
adc.update.hasData             178154655 1056389751      5    536 83190215
adc.update                     178154655 255527731      1    530 156025232



rtcount_print :
key                                  num       tot   mean    max maxNum
protect                        720495257 1193071824      1  38146 115103855
adc.update.hasData             721038471 3386893558      4  36684 345675918
mppt.update                    240165136 4108143820     17    847 182623930
mppt.startSweep                        2      1438    719    754      1
start                          721038472 781230800      1    530 576578725
adc.update.handleSensorCalib   720540801  31003272      0    419 547443066
adc.update.AddSampleVirtual    240177087 855618884      3    301 158338037
adc.update.addSample           720540801 794266807      1    292 287884402
protect.pre                    720495257 274942314      0    284 676866487
loopNewData                    720531260  84327997      0    283 662507698
micros                         721038473 727540760      1    283 57643726
adc.update.pre                 721038473  24744438      0    237 144052547
adc.update.startReading        720540801 120960605      0    229 100746055
adc.update.getSample           720540801 1166212132      1    131 585236293
adc.update                     721038473  47885776      0    120 201659165
mppt.update.pre                240165137    344493      0     99 9590907
mppt.startSweep.pre                    2        18      9     11      0


```

```
V=56.45/29.18 I= 1.3/ 2.39A  71.8W -34℃31℃ 1136sps  0㎅/s PWM(H|L|Lm)= 218| 185| 185 st=↑MPPT,1 lag=0.9ms lt=0.9ms N=4788104 rssi=-12
I (6010354) mppt: periodic zero-current calibration
PWM disabled (duty cycle was 245)
I (6010355) mppt: Start sweep
I (6010355) mppt: Start calibration
I (6010355) sensor: U_in_raw reset calibration
I (6010355) adc_fake: Reset channel 1 at 1715387325
I (6010355) adc_fake: Reset channel 2 at 1715387325
I (6010355) sensor: U_out_raw reset calibration
I (6010355) adc_fake: Reset channel 1 at 1715387325
I (6010370) sampler: Sensor U_in_raw calibration: avg=56.4545 std=0.000000
I (6010370) sampler: Sensor Io calibration: avg=0.0000 std=0.000000
I (6010370) sampler: Sensor Io midpoint-calibrated: 0.000000
I (6010371) sampler: Sensor U_out_raw calibration: avg=29.1818 std=0.000000
I (6010371) sampler: Calibration done!
Backflow switch disabled
I (6010579) store: Wrote /littlefs/stats (size 32)
I (6010580) flash: Wrote flash value /littlefs/stats
V=56.45/29.18 I= 0.0/ 0.00A   0.0W -34℃31℃  0sps  0㎅/s PWM(H|L|Lm)=  65| 123| 123 st=SWEEP,1 lag=34.8ms lt=34.8ms N=2608 rssi=-12
Current above threshold 0.20
Backflow switch enabled
Low-side switch enabled
V=56.45/29.18 I= 1.1/ 2.09A  62.8W -34℃31℃ 1135sps  0㎅/s PWM(H|L|Lm)= 209| 178| 178 st=SWEEP,1 lag=34.8ms lt=34.8ms N=14613 rssi=-12
V=56.45/29.18 I= 1.4/ 2.63A  79.0W -34℃31℃ 1137sps  0㎅/s PWM(H|L|Lm)= 354| 302| 302 st=SWEEP,1 lag=34.8ms lt=34.8ms N=26624 rssi=-12
V=56.45/29.18 I= 1.5/ 2.83A  85.1W -34℃31℃ 1136sps  0㎅/s PWM(H|L|Lm)= 498| 424| 424 st=SWEEP,1 lag=34.8ms lt=34.8ms N=38633 rssi=-11
V=56.45/29.18 I= 1.6/ 2.96A  89.0W -34℃31℃ 1135sps  0㎅/s PW
```

# Deferred logging still mallocs on the RT core

Logging from `loopRT` (core1) is deferred: once `loggingEnableDefer()` runs (just before the RT loop starts),
`ESP_LOGx`/`UART_LOG`/`printf_mux` on core1 take the `enqueue_log()` path instead of writing UART/USB synchronously
(`src/logging.cpp`). So the UART blocking is *not* on the RT path. But `enqueue_log()` still does `new char[l+1]` per
entry, and `new` takes the global heap lock. During boot core0 is bringing up Wi-Fi/LWIP/MQTT-TLS with large
allocations that hold that lock for milliseconds, so the core1 `new` can stall on it.

Symptom: a one-shot multi-ms spike in `adc.update.handleSensorCalib` (e.g. max=9ms at an early `maxNum`), mean ~1µs.
The first sensor-calibration completion fires 2-3 `ESP_LOGI`s back-to-back (`src/adc/sampling.h`), each a contended
`new`, all attributed to that one rtcount window. It does not recur once boot allocation traffic settles.

To remove it, get the allocation off the RT path: preallocated buffer pool / fixed-size ring for the async log queue
instead of `new char[l+1]` per entry.

# Flash Cache

* IRAM
* https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/speed.html#measuring-performance
* https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/speed.html#speed-targeted-optimizations
* noflash https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/linker-script-generation.html
    * https://github.com/espressif/esp-idf/blob/v4.2.2/components/freertos/linker.lf

# GCC Instrumentation

https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html

* `-pg` flag
* https://stackoverflow.com/a/7290284/2950527
* inject call to mcount (or _mcount, or __mcount
* https://www.math.utah.edu/docs/info/gprof_toc.html
* https://docs-archive.freebsd.org/44doc/psd/18.gprof/paper.pdf

# Off-loading critical parts

The INA226 can be programmed to trigger an alert on bus over-voltage. this signal can be wired to the shut-down input of
the gate driver to instantly turn off the DC-DC converter. The INA226 has a minimum conversion time of 140µs.

run arduino:

```
CONFIG_ARDUINO_RUNNING_CORE=0
CONFIG_ARDUINO_RUN_CORE0=y
CONFIG_ARDUINO_EVENT_RUNNING_CORE=0
CONFIG_ARDUINO_EVENT_RUN_CORE0=y
CONFIG_ARDUINO_SERIAL_EVENT_TASK_RUNNING_CORE=0
CONFIG_ARDUINO_SERIAL_EVENT_RUN_CORE0=y
CONFIG_ARDUINO_UDP_RUNNING_CORE=0
CONFIG_ARDUINO_UDP_RUN_CORE0=y
```