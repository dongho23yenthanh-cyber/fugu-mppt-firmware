---
name: project-vconv-on-esp32-classic
description: How to build/flash/run the vconv virtual-converter smoke test on esp32-classic; vconv_mock is S3-only
metadata: 
  node_type: memory
  type: project
  originSessionId: a73d2322-985c-45f0-83c1-e7f91dff1391
---

Running the vconv (virtual converter, `CONFIG_FUGU_WITH_VCONV`) e2e smoke test on the **esp32-classic** bench unit (`/dev/cu.usbserial-0001`):

- **Firmware:** classic needs its own sdkconfig. `sdkconfig.esp32app` is the working classic config (NETW+NETTOOLS+BLE on, VCONV off). `sdkconfig.esp32vconv` = copy of it with `CONFIG_FUGU_WITH_VCONV=y`. Build: `IDF_TARGET=esp32 idf.py -B build-esp32-vconv -D SDKCONFIG=sdkconfig.esp32vconv build`. Fits classic OTA slot (~8% free). VCONV is mutually exclusive with MCPWM (off).
- **littlefs:** `config/lab/vconv_mock` is **S3-only** — its board.conf has `mcu=esp32s3` and GPIOs 47/40/42 that crash `pinMode` on classic. Created `config/lab/vconv_mock_esp32` = vconv_mock confs + classic board.conf from `[[project_wokwi_esp32_setup]]`'s `wokwi_mock_esp32` (valid classic pins 33/32/27/25, i2c 21/22). Under VCONV the PWM/ADC are software shims (`PWM_VConv` ignores the pin), so only `pwm_sd`/`panel_sd`/`i2c_sda` must be valid-or-disabled.
- **Flash:** `idf.py -B build-esp32-vconv app-flash` (firmware only — the build bundles `config/fugu1/fugu1_esp32` littlefs, NOT vconv), then `./provision.py config/lab/vconv_mock_esp32` for the littlefs.
- **Drive/observe:** firmware auto-sweeps at boot. `--stdin` console disconnects after replies; to watch convergence hold the connection open: `{ echo status; sleep 45; } | .venv/bin/python etc/fugu_console.py -p <port> --stdin`. Status line auto-prints intermittently.

**Result (2026-05-30, PASS):** plant Isc=13 Voc=76 k=0.85, Bat=27V/0.05Ω, L=50µH. MPPT converges global-sweep → fast P&O, settles **Vin≈64.6V (=0.85·Voc, ~1% off), ~815W, Iout≈27.8A, Vout≈28.4V**, holds stably, full 3000 sps, no trips/ADC-errors/panics. Matches the s3 known-good ~815W ([[project_vconv_fragility_mechanism]]).
