---
name: project_measure_coil_on_device
description: on-device measure-coil command (L0/rect_offset sweeps) ported from measure_coil.py; validated on flat within ~2%
metadata: 
  node_type: memory
  type: project
  originSessionId: 3a0d7705-abb0-4b12-9275-121ae787cfa9
---

`measure-coil l0|ls [steps|hs] [dwell_ms] [apply]` ports `etc/measure_coil.py` onto the device. The
sweep logic lives in its own `src/measure_coil.{h,cpp}` (spawned core-0 task; externs main.cpp
globals like cli.cpp, so listed in MAIN_SRC in main/CMakeLists.txt, not built for test mains);
`cli.cpp::cmdMeasureCoil` is just an arg-parsing thunk calling `measureCoilStart()`. buck.h gained
`getPwmFrequency()`. Same DCM transfer relation
`L=(Vin-Vout)·Vin·D²/(2·Vout·fsw·Iout)`, reads `sensors.*->ewm.avg.get()` instead of parsing the
status line. Has busy-guard, Vin>Vout+1 check, overcurrent/CCM aborts, always-restore epilogue,
median+IQR, `quadfit3` parabola for the LS peak, and `apply` writes to coil.conf. E2E parity test
`etc/e2e-test/test_measure_coil.py` runs host script + device back-to-back (shared converter/telnet
slot) and asserts L0 within --tol (default 20%); no-sun runs SKIP. Committed on scope-client as
a13022e (feature-only; rest of that branch is the user's concurrent WIP).

**Validated on real hardware (flat, 2026-05-22):** on-device `measure-coil l0` gave **50.34 µH**
(IQR 16%) vs host `measure_coil.py` **49.69 µH** (IQR 5%) vs flat's known KS130 ~51 µH (doc median
50.9 µH) — agree within ~2%, flat L-vs-D profile (no trend), stable Vin. Settling logic kept (it
produced the clean profile).

**Earlier mock confusion (resolved):** first tested on fugu139C, a **bench device with a mock ADC** —
its synthetic output showed a spurious upward L-vs-D trend, "lagging" Iout, and "Vin swings" that I
mis-read as real settling/panel dynamics, and added a pre-settle + window-based Iout convergence to
fight. Those were mock artifacts. On real HW a plain fixed dwell + median settles fine (the host tool
proves it), so that machinery was **removed** — `settleAndRead` is now fade-wait + dwell + 5-read
median, default dwell 3000 ms. For a tighter result use more steps (smaller jumps); the default
10-step sweep gives only ~5 usable points before the 2 A i_max cutoff. Ignore the 139C numbers.

**Flashing caveat:** a full `idf.py flash` rewrites littlefs with the `wokwi_mock` config baked into
CMakeLists, clobbering a provisioned board — use OTA (app slot only). `etc/ota.py` auto-discovers and
OTAs *every* device it finds; target one host with a direct `ota http://<myip>:9000/build/...bin`
(works behind NAT; flat/fry reach the host outbound). See [[project_fry_flat_nat_mapping_swapped]]
for ports (:232=flat, :233=fry as of 2026-05-22 — confirm via welcome banner first).
