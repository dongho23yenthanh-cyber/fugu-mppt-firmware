---
name: project_ina226_timeout_loop_latency_shutdown
description: "fry/flat \"Loop latency high, shutdown\": INA226 alert timeouts starve the RT sampler under high power (2026-05-22); but on fixed fw in CV/idle the acute trigger is console-command handling stalling the RT loop (7/7, 2026-05-28)"
metadata: 
  node_type: memory
  type: project
  originSessionId: 5628603e-0ac6-4f11-8f7f-5ee052d4e1ab
---

On 2026-05-22 fry & flat tripped the loop-latency protection ("Loop latency high (<200 Hz), shutdown!" in `loopLF`, src/main.cpp:506) **21 times** while converting 230-380W.

Root cause: the **INA226 (I2C, alert-GPIO driven) chronically fails to deliver conversion-ready alerts.** `ADC_INA226::hasData()` (src/adc/ina226.h:364) waits `taskNotification.wait(3)` = 3ms per miss and logs `ina22x: <n> timeout!` only every 20000th. fry logged 20000→120000 in ~31 min ≈ **54 timeouts/s**, each a 3ms wasted wait injected into the RT loop.

KEY: on **fmetal both `vout` (ina226 ch0/ChVBus) AND `iout` (ch1/ChI) are on the INA226** (vin/ntc on internal esp32adc1). So a missed alert freezes `Vout->numSamples` directly — and that IS the watchdog's metric. INA `scheme()==all`: when `hasData()` is false, neither vout nor iout gets a sample. Sustained alert loss → `sps<expected_hz` (sensor.conf expected_hz=200, normal ~451-511) over the >2.7s window → `stopAndBackoff`.

Alert path is edge-fragile: continuous mode + non-latched conv-ready, FALLING-edge ISR (GPIO 41), flag cleared by reading MASK_EN in hasData; if the read-clear is late vs the next ~1.1ms conversion the falling edge is missed. `i2c_freq=800000` (board.conf) is aggressive (>std 400kHz); fry's INA226 is suspected counterfeit ("much higher noise on shunt", ina226.h:107) — both make the I2C/alert path marginal under high-power switching EMI, clustering misses.

**The watchdog is working correctly — it's a symptom reporter, not the bug.** Tell-tales in the status line: `lag` constant (~2705µs fry) = no multi-second *compute* stall, RT loop is *blocked waiting* for ADC data; `0㎅/s` = scope NOT streaming.

Ruled out: the `scope-client` branch (changes are core-0 only; scope wasn't streaming), and the `sensor`/`sensor avg` console polling next to the trips (coincidental — read-only, one UART_LOG; it's just what's run while watching).

**Why:** chronic INA226 alert flakiness is long-standing (cumulative timeout counts 560k-5.8M predate the log window). The acute shutdowns happen when starvation lasts >2.7s.
**How to apply:** fixing the watchdog is wrong. Fix the INA226 alert path (check ALERT GPIO/config, conv-time vs 3ms wait, I2C health under high-power EMI) and/or make the sampler skip a timed-out sensor without burning 3ms or starving `Vout`. Related: [[project_fry_ina226_undereads_157.md]], device log: ssh havan.local `tail pv/fugu_console.log`.

---
**UPDATE 2026-05-28 (post deadlock + cache-error fix, see [[project_gpio_isr_install_nested_ipc_deadlock]]):** with fry recovered and idling in CV (~0W, battery full), I quantified shutdowns from the MQTT log over a clean hour: ~7/hour, median sps 511 (healthy ~90%), ~10% of 1s windows <200, persistent max lag ~3ms. **7/7 shutdowns fired ~1s after a console command** (`hostname`,`ip`,`uptime`,`getc`) — incl. pure in-memory ones that read no flash. So the acute trigger here is **console-command handling on core0 stalling the RT loop on core1** (same heap-lock/log-alloc contention as the "Deferred logging still mallocs on the RT core" note in Real-Time Latency.md), not flash and not (only) INA226 alert misses. This **refines** the 2026-05-22 "ruled out console polling as coincidental" line — at high power INA226 starvation dominates; at idle the loop sits near the edge (lag ~3ms) and any console command pushes a window <200. A discovery/health poller (`ip`/`hostname`/`uptime`) thus shuts the converter down every poll. Fixes documented in Real-Time Latency.md ("Console commands trip the loop-latency watchdog"): log-alloc off RT path; watchdog require N consecutive low windows; stop polling the console. **Not yet implemented** (live converter, pending user decision).
