Plan: Bluetooth (BLE) Serial Console

Context

The firmware exposes the same string command protocol (handleCommand() in src/main.cpp)
over UART, USB-CDC, telnet, and MQTT. We want a wireless console without Wi-Fi — useful for
field provisioning/diagnostics where there's no AP to join. ESP32-S3 has BLE only (no classic
Bluetooth/SPP), so this is a BLE GATT Nordic UART Service (NUS) acting as a new console
transport that reuses the existing loopConsole() input pipeline and the existing log multiplexer.

Decisions (confirmed with user)

- NimBLE backend (smallest flash footprint).
- Runtime gate via a ble=1 flag in board.conf (default off).
- Passkey pairing + bonding (the console can edit config and trigger OTA).
- Full log mirror to the connected client, like telnet.

⚠️ Central constraint — flash headroom

Current app binary is 1,549,008 B against the 1,871,872 B OTA slot → only ~315 KB free
(partitions.csv, two OTA slots on 4 MB flash). NimBLE adds ~250–400 KB. The flash cost is
incurred at build time whenever BT is compiled in — the runtime ble flag only saves RAM/radio
power, not flash. Therefore:
- Add a compile-time switch WITH_BLE (env var, mirroring the existing RUN_TESTS/MAIN_SRC
  pattern in main/CMakeLists.txt) so the default firmware can omit BLE entirely and only
  BLE-provisioned builds pay the flash.
- After the first BLE build, verify build/fugu-firmware.bin < 1,871,872 B (see Verification).
  If it overflows, see Flash-recovery options below.

Files to change

1. New module: src/console_ble.cpp + src/console_ble.h

NUS GATT peripheral, guarded by #ifdef WITH_BLE (whole file compiles to nothing otherwise).
Public API (header always declares these; bodies are no-ops when WITH_BLE undefined):
void bleConsoleBegin(const std::string &deviceName, uint32_t passkey);
void bleConsoleLoop(unsigned long nowMs);   // called from loopNetwork_task
bool bleConsoleConnected();
Implementation notes:
- Use arduino-esp32 BLE library (BLEDevice/BLEServer/BLECharacteristic/BLESecurity),
  which maps onto the NimBLE host when CONFIG_BT_NIMBLE_ENABLED=y.
- NUS UUIDs: service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E, RX (write, client→device)
  …0002…, TX (notify, device→client) …0003….
- RX path: the characteristic onWrite callback (runs in the NimBLE/Arduino-event task on
  core 0) pushes received bytes into a lock-free byte queue
  (reuse etc/readerwriterqueue.h, as logging.cpp already does). A bleRead(buf,len) drains
  that queue. bleConsoleLoop() calls loopConsole(bleRead, bleWrite, nowMs) — reusing the exact
  line-editing/echo/handleCommand() logic from src/console.cpp:29.
- TX path: bleWrite(buf,len) sets the TX characteristic value and notify()s (chunk to the
  negotiated MTU, default 20 B payload).
- Logging mirror: on connect register a log sink, on disconnect deregister
  (see logging change below).
- Security: BLESecurity with ESP_LE_AUTH_REQ_SC_MITM_BOND + static passkey
  (setStaticPIN/passkey API — confirm exact method name against the vendored lib at
  managed_components/espressif__arduino-esp32/libraries/BLE during implementation). Passkey from
  board.conf. Bonds persist in the nvs partition.

2. src/logging.cpp / src/logging.h — allow multiple log sinks

Today there is a single logCallback (logging.cpp:30) and MQTT already owns it
(addLogCallback at mqtt.cpp). BLE needs to coexist with MQTT, so generalize:
- Replace the single void(*logCallback)(...) with a small fixed array (e.g.
  std::array<cb,4> + count) so MQTT + BLE both receive logs.
- addLogCallback() appends; add removeLogCallback(cb) for BLE disconnect.
- Update the two consumers to iterate the array: vprintf_mux() (logging.cpp:202-208) and
  flush_async_uart_log() (logging.cpp:184-185).
- This is the established pattern — telnet's set_logging_telnet/log_telnet stays as-is; BLE
  uses the callback array. Reuses the existing async-defer machinery (RT-core logs are already
  queued and flushed on core 0), so BLE output is fed safely from core 0 only.

3. src/main.cpp — wire in the transport (guard with #ifdef WITH_BLE)

- setup(): after the Wi-Fi/MQTT block (~`main.cpp:399) and after boardConf is read (main.cpp:317), read
 boardConf.getByte("ble", 0)andboardConf.getLong("ble_passkey", 0); if enabled call bleConsoleBegin(hostname, passkey). Reuse the
 existing hostname used for mDNS / MQTT log topic pv/log/{hostname}` as the advertised name.
- loopNetwork_task(): add bleConsoleLoop(nowMs); next to loopUart(nowMs) (main.cpp:819).
  It is a no-op until bleConsoleBegin runs, and runs on core 0 (assert already enforces this).
- Note: loopConsole's line buffer is static (console.cpp:31), shared across transports;
  concurrent typing on UART and BLE could interleave. This already applies to UART+USB and is
  acceptable (single operator). Document it; do not change.

4. main/CMakeLists.txt — conditional source + compile def

- Add "../src/console_ble.cpp" to SRCS.
- Add an env-driven switch mirroring the existing RUN_TESTS/MAIN_SRC blocks:
  if $ENV{WITH_BLE} set, component_compile_definitions("WITH_BLE=1").

5. sdkconfig.defaults — enable NimBLE, pinned to core 0

sdkconfig is untracked and drifts (per CLAUDE.md), so the source of truth goes in
sdkconfig.defaults. Add (only meaningful for WITH_BLE builds; harmless otherwise but costs
flash, hence prefer enabling these only in a BLE build profile — see below):
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
# CONFIG_BT_BLUEDROID_ENABLED is not set
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
# CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set
# CONFIG_BT_NIMBLE_ROLE_OBSERVER is not set
# keep BT off the RT core (core 1):
CONFIG_BT_CTRL_PINNED_TO_CORE_0=y
CONFIG_BT_NIMBLE_PINNED_TO_CORE_0=y
Core-affinity is critical: the project hard-separates core 1 (RT) from core 0 (everything
else). The BLE controller ISR/task and NimBLE host must be pinned to core 0; verify after build
that no BLE task lands on core 1.

Because these defaults bake the BT stack into every build, prefer keeping them in a separate
defaults file (e.g. sdkconfig.ble) selected only for BLE builds, OR accept the flash cost
globally if it fits. Decide based on the measured size in Verification.

6. Config images: add the flag to relevant config/*/conf/board.conf

Add ble=0 (and a sample # ble_passkey=123456) to the board configs that should expose it
(e.g. config/fmetal, lab mocks). Default off keeps existing behaviour.

Flash-recovery options (only if the BLE build overflows the OTA slot)

- Keep BLE out of the default firmware (compile-time WITH_BLE), ship a separate BLE build for
  boards that need it.
- Trim NimBLE: disable extended advertising / unused GATT features, reduce
  CONFIG_BT_NIMBLE_MAX_CONNECTIONS to 1, drop logging in the BT stack.
- Audit transitively-pulled arduino-esp32 components for dead weight already linked in.

Verification

1. Build (no BLE) still works & unchanged size:
   . ./idf-export.sh && idf.py build → confirm build/fugu-firmware.bin ≈ 1.55 MB.
2. Build with BLE:
   WITH_BLE=1 idf.py build then check size fits:
   ls -l build/fugu-firmware.bin must be < 1,871,872 B; also idf.py size.
3. Core affinity: flash, then on the serial console run rt-stats and confirm no BLE/NimBLE
   task is on core 1; RT loop timing (rtcount) is unaffected.
4. End-to-end console (provision.sh a config with ble=1, flash, idf.py monitor):
- From a phone (e.g. nRF Connect / Serial Bluetooth Terminal using NUS), scan → device
  advertises under the hostname.
- Pairing prompts for the passkey; wrong passkey is rejected, correct one bonds.
- Type rt-stats (or get-config board.conf) over BLE → response returns over the TX
  notify characteristic; background logs stream in like telnet.
- Confirm UART and telnet consoles still work concurrently and MQTT log output is unaffected
  (validates the multi-sink logging change).
  ls -l build/fugu-firmware.bin must be < 1,871,872 B; also idf.py size.
3. Core affinity: flash, then on the serial console run rt-stats and confirm no BLE/NimBLE
   task is on core 1; RT loop timing (rtcount) is unaffected.
4. End-to-end console (provision.sh a config with ble=1, flash, idf.py monitor):
- From a phone (e.g. nRF Connect / Serial Bluetooth Terminal using NUS), scan → device
  advertises under the hostname.
- Pairing prompts for the passkey; wrong passkey is rejected, correct one bonds.
- Type rt-stats (or get-config board.conf) over BLE → response returns over the TX
  notify characteristic; background logs stream in like telnet.
- Confirm UART and telnet consoles still work concurrently and MQTT log output is unaffected
  (validates the multi-sink logging change).
5. OTA sanity: confirm the BLE image still flashes into an OTA slot and ota.sh succeeds
   (headroom check).

Out of scope

- Wi-Fi provisioning over BLE (separate feature).
- Classic Bluetooth SPP (not supported on ESP32-S3).