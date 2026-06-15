---
name: project_picoscope_ps2000_capture
description: "headless PicoScope 2000 capture on Apple Silicon — legacy ps2000 driver via Rosetta+ctypes, etc/pico_capture.py"
metadata: 
  node_type: memory
  type: project
  originSessionId: b7d04981-8ce2-40cb-9fb4-e6a1ce62f033
---

The bench PicoScope is a legacy **ps2000-series** (2204/2205-class): `ps2000_open_unit` (in
`libps2000.dylib`) returns handle 1; the newer `ps2000a` API rejects it. `etc/pico_capture.py`
drives it headlessly.

Non-obvious gotchas:
- The driver dylibs ship inside `/Applications/PicoScope 7 T&M.app/Contents/Resources/` and are
  **x86_64-only**. This is an arm64 Mac, so the script re-execs itself under
  `arch -x86_64 /usr/bin/python3` (a universal binary) with that Resources dir on
  `DYLD_LIBRARY_PATH` (libps2000 lazily needs `libpicoipp`/`libiomp5` from there). Rosetta is present.
- `ps2000_get_timebase` returns FALSE (0) and leaves interval=0 when `no_of_samples` exceeds the
  buffer. Buffer cap here is **3968 samples**; request ≤3000.
- Timebase 0 needs ETS (fails); tb=1 is 50 MS/s (20 ns), interval doubles per index.
  `time_interval` is in **ns regardless** of the separate `time_units` output.
- `ps2000` enums: ranges 20mV=1..20V=10; coupling DC=1/AC=0; trigger source NONE=5 (free-run);
  MAX_ADC=32767. No numpy needed — raw `ctypes` int16 buffers.

Use case: scoped CCM-ripple cross-check of `coil.conf::L0` (the §2/open-question in
[[project_fry_ina226_undereads_157]] — confirm fry's ~80 µH). Clamp is a **Hantek CC-65**, DC–20 kHz,
100 mV/A (20 A range) or 10 mV/A (65 A range).
