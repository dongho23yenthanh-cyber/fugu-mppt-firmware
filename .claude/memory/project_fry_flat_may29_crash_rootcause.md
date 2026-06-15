---
name: fry-flat-may29-crash-rootcause
description: "fry/flat 2026-05-29 crashes were 3 firmware bugs (not hardware); \"dead ADC\" was a deadlock, hot-removal was a red herring"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2942d6e5-8a72-4ad2-9c4d-d09d05dfd848
---

The 2026-05-29 fry/flat instability (crash loops, "no ADC samples", wifi-task panics) was **three
firmware bugs**, all fixed in HEAD `g2e4ca6c`:
- `6f02574` — wifi-task vprintf (logging in the wifi task context overflowed its stack)
- `24e24cf` — sys_evt stack size
- `14734cc` — **ADC deadlock** (this is what produced fry's "`adc_esp32: no ADC samples`" / 0 sps,
  dying ~10 s after every boot, across builds)

Trigger was the **NAT-router (192.168.1.173) reflash → WiFi reconnect storm** hammering the buggy
connect/log paths, not anything physical.

**Correction to my earlier conclusions (I was wrong):**
- I diagnosed fry's dead ADC as **hardware damage** (internal ADC1 zapped when the user hot-removed
  the MCU daughterboard from the powered stage ~15:00). **WRONG** — it was the ADC-deadlock bug; the
  internal ADC is healthy on the fix. The hot-removal was a **coincidence**. **Confirmed sound by a
  direct hardware self-test** (vin/ntc DMA conv-done firing @458 sps no watchdog trip; INA226 I2C +
  1025 alert interrupts/2s) — no board swap. Lesson: a driver *deadlock* gives the identical "DMA
  stops / no samples (not garbage)" signature as damaged silicon, so a production stall does NOT
  prove a bad peripheral. The valid discriminator is an **isolated self-test** away from the
  watchdog-gate loop — if it samples cleanly, the hardware is fine.
- My ADC self-heal (`ADC_ESP32_Cont::resetPeripherals` + RT-loop trigger) and the
  `CONFIG_HEAP_POISONING_COMPREHENSIVE` diag build were superseded by the real fixes; not pushed.

**What held up:** the fry coredump decode correctly fingered the **wifi task + corrupted NVS-handle
vtable = wifi-task stack overflow** (fixed by 6f02574/24e24cf) — I just over-hedged it. Extraction
via MQTT (around the BLE 8 KB truncation, [[project-coredump-get-ble-8kb-truncation]]) and the
exact-commit worktree rebuild + esp-coredump SHA-gate bypass both worked. INA226 (vout/iout, I2C on
the power stage) was never affected — only the internal `esp32adc1` (vin/ntc) path.

Both converters ended on `g2e4ca6c`: fry serial-flashed (config restored), flat OTA'd.
