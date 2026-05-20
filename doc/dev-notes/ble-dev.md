*this document is an LLM generated placeholder*

# Plan: Bluetooth (BLE) Serial Console

## Implementation status (2026-05-20)
**Implemented and verified on hardware** (esp32s3, mock board config). `WITH_BLE=1` build compiles,
links, and fits the OTA slot (1,803,360 B / ~4% free). On device: boots with BLE in, auto-starts when
`ble.conf enabled=1`, `svc start/stop ble` advertise/teardown cleanly (justworks security), RT
loop unaffected (steady ~1.1 ms max lag; one-time ~16 ms `BLEDevice::init` transient on the core-0
network loop at start). **Still untested:** actual BLE client connection (pairing + NUS round-trip
from a phone / WebBLE) — needs a central. Build with `WITH_BLE=1 idf.py build`.
- `src/console_ble.{h,cpp}` — NimBLE NUS server (RX queue → `loopConsole`, TX notify, configurable
  security, log mirror), no-op stubs when `WITH_BLE` undefined.
- `src/logging.{h,cpp}` — single `logCallback` → array + `removeLogCallback` (MQTT switched to it).
- `src/main.cpp` — `BleConsoleService` (no network req, disabled by default), registered in `setup()`.
- `main/CMakeLists.txt` (+`WITH_BLE` def, source) and top `CMakeLists.txt` (+`sdkconfig.ble` profile);
  new `sdkconfig.ble` (NimBLE, core-0 pinned, peripheral only).
- `config/{fmetal,lab/wokwi_mock}/conf/ble.conf`; WebBLE transport added to
  `etc/config-tool/conf-editor.html`.

## Context

The firmware exposes the same string command protocol (`handleCommand()` in `src/main.cpp`)
over UART, USB-CDC, telnet, and MQTT. We want a **wireless console without Wi-Fi** — useful for
field provisioning/diagnostics where there's no AP to join. ESP32-S3 has **BLE only** (no classic
Bluetooth/SPP), so this is a BLE GATT **Nordic UART Service (NUS)** acting as a new console
transport that reuses the existing `loopConsole()` input pipeline and the existing log multiplexer.

### Decisions (confirmed with user)
- **NimBLE** backend (smallest flash footprint).
- **Wrapped as a `Service`** (`src/service.h`) — *blocked on the in-progress service architecture;
  continue once it lands.* The NUS console is a `BleConsoleService`: `start()` initializes the
  NimBLE stack + advertises, `stop()` tears it down, `onTick()` (core 0) drains RX → `loopConsole`,
  `restart()` re-reads conf. Registered in `ServiceManager g_services` in `setup()`. This **replaces**
  the earlier direct `setup()` + `loopNetwork_task` wiring described below — see "Service wrapping".
- **Runtime gate** via the service's own `ble.conf` (`enabled`/`log_level`), consistent with the
  other services (mqtt/tele/ftp/telnet/lcd/scope). The `board.conf` `ble=1` idea is superseded.
- **Configurable security, Just Works default** — `board.conf` `ble_security` ∈ `none|justworks|passkey`;
  default `justworks` (encrypted link, no passkey) for reliable Web Bluetooth; `passkey` available
  for native clients that need MITM. (Originally passkey+bonding; relaxed for WebBLE compatibility.)
- **Full log mirror** to the connected client, like telnet.
- **Web Bluetooth is a first-class client** — the existing `etc/config-tool/conf-editor.html` gets a
  NUS transport alongside its current Web Serial path.

### Web Bluetooth (WebBLE) compatibility — why the choices above
- NUS over BLE GATT is exactly what `navigator.bluetooth` is built for; the existing web editor
  already drives the `get-config`/`set-config` console protocol over **Web Serial**, so WebBLE is a
  parallel transport reusing the same command strings.
- **Chromium-only** (Chrome/Edge/Opera on Android/Win/macOS/Linux/ChromeOS); not Safari/Firefox, and
  iOS needs a WebBLE wrapper app. Document this limitation.
- **Secure-context required**: `navigator.bluetooth` only exists over **https or localhost** —
  `file://` gives `undefined`. The web editor must be served (see WebBLE client section).
- **Security mode matters**: MITM passkey pairing through WebBLE is flaky across OSes; **Just Works**
  (or open) is the reliable path — hence the configurable default above.

### ⚠️ Central constraint — flash headroom
Current app binary is **1,549,008 B** against the **1,871,872 B** OTA slot → only **~315 KB free**
(`partitions.csv`, two OTA slots on 4 MB flash). NimBLE adds ~250–400 KB. **The flash cost is
incurred at build time whenever BT is compiled in — the runtime `ble` flag only saves RAM/radio
power, not flash.** Therefore:
- Add a **compile-time switch** `WITH_BLE` (env var, mirroring the existing `RUN_TESTS`/`MAIN_SRC`
  pattern in `main/CMakeLists.txt`) so the default firmware can omit BLE entirely and only
  BLE-provisioned builds pay the flash.
- After the first BLE build, **verify `build/fugu-firmware.bin` < 1,871,872 B** (see Verification).
  If it overflows, see *Flash-recovery options* below.

## Files to change

### 1. New module: `src/console_ble.cpp` + `src/console_ble.h`
NUS GATT peripheral, guarded by `#ifdef WITH_BLE` (whole file compiles to nothing otherwise).
Public API (header always declares these; bodies are no-ops when `WITH_BLE` undefined):
```
void bleConsoleBegin(const std::string &deviceName, uint32_t passkey);
void bleConsoleLoop(unsigned long nowMs);   // called from loopNetwork_task
bool bleConsoleConnected();
```
Implementation notes:
- Use arduino-esp32 BLE library (`BLEDevice`/`BLEServer`/`BLECharacteristic`/`BLESecurity`),
  which maps onto the NimBLE host when `CONFIG_BT_NIMBLE_ENABLED=y`.
- **NUS UUIDs**: service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`, RX (write, client→device)
  `…0002…`, TX (notify, device→client) `…0003…`.
- **RX path**: the characteristic `onWrite` callback (runs in the NimBLE/Arduino-event task on
  core 0) pushes received bytes into a lock-free byte queue
  (reuse `etc/readerwriterqueue.h`, as `logging.cpp` already does). A `bleRead(buf,len)` drains
  that queue. `bleConsoleLoop()` calls `loopConsole(bleRead, bleWrite, nowMs)` — reusing the exact
  line-editing/echo/`handleCommand()` logic from `src/console.cpp:29`.
- **TX path**: `bleWrite(buf,len)` sets the TX characteristic value and `notify()`s (chunk to the
  negotiated MTU, default 20 B payload).
- **Logging mirror**: on connect register a log sink, on disconnect deregister
  (see logging change below).
- **Security (configurable)**: read `board.conf` `ble_security`:
  - `none` → no `BLESecurity`, characteristics open (most reliable WebBLE, least safe).
  - `justworks` (default) → `BLESecurity` with `ESP_LE_AUTH_REQ_SC_BOND` (encrypted, no passkey).
  - `passkey` → `ESP_LE_AUTH_REQ_SC_MITM_BOND` + static passkey from `board.conf` `ble_passkey`
    (`setStaticPIN`/passkey API — confirm exact method against the vendored lib at
    `managed_components/espressif__arduino-esp32/libraries/BLE` during implementation).
  Bonds persist in the `nvs` partition. Default `justworks` keeps Web Bluetooth working smoothly.

### 2. `src/logging.cpp` / `src/logging.h` — allow multiple log sinks
Today there is a **single** `logCallback` (`logging.cpp:30`) and MQTT already owns it
(`addLogCallback` at `mqtt.cpp`). BLE needs to coexist with MQTT, so generalize:
- Replace the single `void(*logCallback)(...)` with a small fixed array (e.g.
  `std::array<cb,4>` + count) so MQTT + BLE both receive logs.
- `addLogCallback()` appends; add `removeLogCallback(cb)` for BLE disconnect.
- Update the two consumers to iterate the array: `vprintf_mux()` (`logging.cpp:202-208`) and
  `flush_async_uart_log()` (`logging.cpp:184-185`).
- This is the established pattern — telnet's `set_logging_telnet`/`log_telnet` stays as-is; BLE
  uses the callback array. Reuses the existing async-defer machinery (RT-core logs are already
  queued and flushed on core 0), so BLE output is fed safely from core 0 only.

### 3. Service wrapping — `BleConsoleService` (guard with `#ifdef WITH_BLE`)
*Depends on the in-progress service architecture (`src/service.h`, CLAUDE.md "Service architecture").
Confirm the final `Service` interface before writing this.* The concrete wrapper lives in
`src/main.cpp` alongside the other service wrappers (it needs the hostname/console glue):
- `name()` → `"ble"` (used as the ESP_LOG tag and `ble.conf` basename).
- `start()` → read `ble.conf` (`ble_security` default `justworks`, `ble_passkey`), call
  `bleConsoleBegin(hostname, …)`; advertise under the existing hostname (same one used for mDNS /
  MQTT topic `pv/log/{hostname}`). Set `ServiceState` Running/Failed accordingly.
- `stop()` → tear down advertising + NimBLE; deregister the log sink.
- `onTick()` (core 0) → `bleConsoleLoop(nowMs)` (drains RX queue → `loopConsole`). No-op when stopped.
- `restart()` → re-read `ble.conf`, restart if security/name changed.
- Registered + started in `setup()` via `ServiceManager g_services`. Unlike the network services,
  BLE has **no Wi-Fi precondition** — it can run with Wi-Fi down (that's its main use case).
- Note: `loopConsole`'s line buffer is `static` (`console.cpp:31`), shared across transports;
  concurrent typing on UART **and** BLE could interleave. This already applies to UART+USB and is
  acceptable (single operator). Document it; do not change.

### 3b. `ble.conf` — service config
New conf file read by the service: `enabled` (0/1, default 0), `log_level` (error/warn/info),
`ble_security` (`none|justworks|passkey`, default `justworks`), `ble_passkey` (digits, for passkey
mode only). Replaces the earlier `board.conf` `ble*` keys.

### 4. `main/CMakeLists.txt` — conditional source + compile def
- Add `"../src/console_ble.cpp"` to `SRCS`.
- Add an env-driven switch mirroring the existing `RUN_TESTS`/`MAIN_SRC` blocks:
  if `$ENV{WITH_BLE}` set, `component_compile_definitions("WITH_BLE=1")`.

### 5. `sdkconfig.defaults` — enable NimBLE, pinned to core 0
`sdkconfig` is untracked and drifts (per CLAUDE.md), so the source of truth goes in
`sdkconfig.defaults`. Add (only meaningful for `WITH_BLE` builds; harmless otherwise but costs
flash, hence prefer enabling these only in a BLE build profile — see below):
```
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
```
**Core-affinity is critical**: the project hard-separates core 1 (RT) from core 0 (everything
else). The BLE controller ISR/task and NimBLE host **must** be pinned to core 0; verify after build
that no BLE task lands on core 1.

Because these defaults bake the BT stack into *every* build, prefer keeping them in a separate
defaults file (e.g. `sdkconfig.ble`) selected only for BLE builds, OR accept the flash cost
globally if it fits. Decide based on the measured size in Verification.

### 6. Config images: add `ble.conf` to relevant `config/*/conf/`
Add a `ble.conf` with `enabled=0`, `log_level=info`, `ble_security=justworks` (and a commented
`# ble_passkey=123456`) to the configs that should expose it (e.g. `config/fmetal`, lab mocks).
Disabled by default. Runtime control via the existing console: `svc start ble` /
`svc stop ble` / `svc log ble info`.

### 7. WebBLE client: `etc/config-tool/conf-editor.html` (first-class target)
The editor already has a Web Serial transport (`connectSerial`/`serialReadLoop`/`serialWrite`/
`importFromSerial`/`disconnectSerial`, ~`conf-editor.html:703-790`) that issues `get-config <file>`
and parses responses. Add a **sibling WebBLE transport** that reuses the same command-string flow:
- A "Connect Bluetooth" button next to "Connect serial".
- `navigator.bluetooth.requestDevice({ filters:[{ services:[NUS_SERVICE_UUID] }] })`, then get the
  RX (write) and TX (notify) characteristics; `startNotifications()` feeds the same line-parser the
  serial read loop uses; writes go to the RX characteristic chunked to ~20 B.
- Feature-detect `('bluetooth' in navigator)`; on failure show the same style of message as Web
  Serial ("use Chrome/Edge over https or localhost"). Note iOS/Safari/Firefox unsupported.
- **Hosting**: `navigator.bluetooth` needs a secure context. Document serving the page over
  https/localhost (e.g. a one-line `python -m http.server` note, or GitHub Pages) — it will not work
  from `file://`.
- Keep the `*this document is an LLM generated placeholder*` first-line marker already present.

## Flash-recovery options (only if the BLE build overflows the OTA slot)
- Keep BLE out of the default firmware (compile-time `WITH_BLE`), ship a separate BLE build for
  boards that need it.
- Trim NimBLE: disable extended advertising / unused GATT features, reduce
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` to 1, drop logging in the BT stack.
- Audit transitively-pulled arduino-esp32 components for dead weight already linked in.

## Verification

1. **Build (no BLE) still works & unchanged size**:
   `. ./idf-export.sh && idf.py build` → confirm `build/fugu-firmware.bin` ≈ 1.55 MB.
2. **Build with BLE**:
   `WITH_BLE=1 idf.py build` then check size fits:
   `ls -l build/fugu-firmware.bin` must be **< 1,871,872 B**; also `idf.py size`.
3. **Core affinity**: flash, then on the serial console run `rt-stats` and confirm no BLE/NimBLE
   task is on core 1; RT loop timing (`rtcount`) is unaffected.
4. **End-to-end console — native client** (`provision.sh` a config with `ble=1`, flash,
   `idf.py monitor`):
   - From a phone (e.g. nRF Connect / Serial Bluetooth Terminal using NUS), scan → device
     advertises under the hostname; with `ble_security=justworks` it connects without a passkey
     prompt; with `passkey` it prompts and rejects wrong passkeys.
   - Type `rt-stats` (or `get-config board.conf`) over BLE → response returns over the TX
     notify characteristic; background logs stream in like telnet.
   - Confirm UART and telnet consoles still work concurrently and MQTT log output is unaffected
     (validates the multi-sink logging change).
5. **End-to-end console — WebBLE**: serve `etc/config-tool/conf-editor.html` over https/localhost,
   open in Chrome/Edge, click "Connect Bluetooth", select the device, then "read live config" →
   confirm `get-config` responses populate the editor over BLE (same as the Web Serial path).
   Verify the unsupported-browser message appears in Safari/Firefox.
6. **OTA sanity**: confirm the BLE image still flashes into an OTA slot and `ota.sh` succeeds
   (headroom check).

## Out of scope
- Wi-Fi provisioning over BLE (separate feature).
- Classic Bluetooth SPP (not supported on ESP32-S3).
- iOS/Safari/Firefox WebBLE support (browser limitation, not fixable in firmware).
