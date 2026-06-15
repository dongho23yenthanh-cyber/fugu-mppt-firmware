---
name: project_esp32_classic_buck_test_uart_hijack
description: full Unity suite reboot-loops on esp32-classic at test_buck (LEDC on GPIO1=U0TXD hijacks console UART)
metadata: 
  node_type: memory
  type: project
  originSessionId: e7592dde-4174-4742-8248-a4a255e8c897
---

On the **ESP32-classic** (not S3), running the full on-target Unity suite reboot-loops at the
`test_buck` block: `test_buck.cpp::initConvEx` configures LEDC on `pwm_hi=1`/`pwm_li=2`, and on
classic ESP32 **GPIO1 = U0TXD**. The buck test repurposes the console TX pin, so every log line
after it (later tests + the `N Tests M Failures` summary) is lost as serial garbage, and the suite
eventually resets — re-running forever.

On the **S3** this is harmless: GPIO1/2 aren't the console (LEDC even logs "GPIO 1 is not usable"),
so test order is immaterial there.

Workaround used (2026-05-30): moved the `tracker.h` RUN_TEST block to run **before** the buck block
in `test/main.cpp` so its output prints while UART is alive. Real fix would be to give `test_buck`
non-UART LEDC pins on esp32 (or guard the buck tests off on classic).

Build/flash recipe for the classic test image (root `sdkconfig` is pinned to esp32s3, so use a
dedicated one): `IDF_TARGET=esp32 RUN_TESTS=1 idf.py -B build-tests-esp32 -D SDKCONFIG=sdkconfig.esp32test build`,
flash with `ESPPORT=/dev/cu.usbserial-0001`. See [[project_esp32_iram_overlay]].

All 6 new tracker P&O tests PASS on the classic. Tracker tests added per [[project_diode_emulation_tests_todo]]-style gap.
