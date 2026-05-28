# Serial Console

Send text commands over UART (or telnet, USB serial-JTAG, or MQTT) to interact with the charger
while it is running. The same string protocol is used on every transport, which also makes it
suitable for automated tests. Input and output are multiplexed across UART, USB serial-JTAG and
telnet.

Default UART baud rate is 115200. Terminate each command with `\n` or `\r` (new line).
A successfully handled command is confirmed with:

```
OK: <cmd>
```

An unknown, malformed, or out-of-context command is confirmed with `ERR: <cmd>`
and a logged reason: a parser error for an unknown command, or the specific
rejection message for invalid arguments / wrong context.

# System Commands

| Command | Description |
| --- | --- |
| `wifi on`, `wifi off [minutes]` | Enable / disable Wi-Fi (and with it all network services). Disabling Wi-Fi usually increases the control-loop rate. Bare `wifi off` disables for good and clears the stored SSID in NVS; `wifi off <minutes>` disables temporarily and re-enables after the timeout, keeping the stored SSID. |
| `wifi add <ssid>:<password>` | Store a new Wi-Fi network. |
| `ip` | Show the local IP address. |
| `hostname <hostname>` | Set the device hostname (persisted in NVS, applied on next boot). |
| `ota <url>` | Download and flash a new app image from an HTTP(S) URL. Halts the converter and ADC during the update. |
| `restart` (aliases `reset`, `reboot`) | Reset the MCU. |

# Control & Diagnostics

| Command | Description |
| --- | --- |
| `fan <float>` | Set fan speed, 0–100. |
| `led <RRGGBB>`, `led <RGB>` | Set the LED color in hex or short hex (e.g. `led 33ff33` or `led 3f3`). |
| `sensor` | Dump per-sensor state (last/raw value, EWM average and std, adaptive notch filter stats). `sensor avg` prints one compact line of EWM averages (`sens: vin=… iout=… …`) for fast polling. |
| `mem` | Display heap and PSRAM size (total and free). |
| `peek <addr> [len]` | Read memory at `<addr>` (hex `0x…`, decimal, or octal). With `len ∈ {1,2,4,8}` (default 4) prints one typed hex value (`peek 0x… = 0x…`); other `len` ≤ 256 prints a hex+ASCII dump. Refuses addresses outside internal RAM / DROM / external RAM / IRAM/IROM (the latter needs 4-byte alignment for word-bus reads). The host CLI (`etc/fugu_console.py`) accepts `peek <symbol>[.field…][+offset]` and ships `sym <pattern>` + `peek-struct <symbol>[.field…]` — all resolved client-side against the build ELF (DWARF for member offsets / field decoding), so the device only ever sees a numeric address. |
| `peek-struct <obj>[.field…] [depth]` *(host-only)* | DWARF-typed dump of an object or sub-object: enumerates each member (offset, type, name) and decodes its value (int / float / bool / pointer / enum / char[]). Reads the byte image via chunked `peek` calls. Embedded aggregates expand inline up to `[depth]` levels (default 2, range 0..16); past the budget they print as `<TypeName, N B>` and can be drilled into with a longer dotted path. Static `constexpr` class members are skipped (no storage). |
| `uptime` | Print seconds since boot (monotonic; resets only on reboot) and the running app description (name, version, build date/time, IDF version). |
| `rt-stats` | Print FreeRTOS per-task runtime statistics (sampled over 1 s). |
| `reset-lag` | Reset the max-lag statistic and print [rtcount](Real-time%20Counter.md) timings. |
| `scan-i2c` | Run an I²C bus scan. |
| `adc-restart` | Re-initialize the ADC backends. |
| `adc-reset` | Reset the ADC peripherals. |

# Config Commands

Hardware and runtime parameters live in `.conf` files on the device's littlefs partition under
`/littlefs/conf/`. These commands edit them in place without re-flashing.

| Command | Description |
| --- | --- |
| `set-config <file> <key> <value>` | Set a key in a config file and persist it to flash. |
| `del-config <file> <key>` | Remove a key; the whole line, including any inline comment, is deleted. |
| `get-config <file> [<key>]` | Print a single key, or dump every key if `<key>` is omitted. |
| `conf-check` | Re-read `charger.conf`/`limits.conf` and warn about keys no loader reads (typos / obsolete, e.g. `cv_min` where the firmware reads `cv_float`). Same check runs at boot for the parameter confs. |

Examples:

```
set-config coil.conf L0 50
set-config limits.conf iout_max 35
set-config converter.conf vout_max 28.5
set-config mqtt.conf broker_uri mqtt://192.168.1.134:1882
set-config charger.conf cell_voltage_eoc 3.53
set-config sensor.conf vout_filt_len 10

del-config sensor.conf vout_filt_len

get-config mqtt.conf broker_uri
get-config converter.conf

conf-check
```

# Charger Commands

| Command | Description |
| --- | --- |
| `status` | Print a charger/battery snapshot: termination state, effective limits (`Vbat_max`/`Vout_max`, `Ibat_lim`/`Iout_max`), the termination line (`v_term`/`cv_min`/`cv_eoc`/`Cbat`/`recharge_dod`) and the BMS feed (`vcell_high` with staleness, `ibat`, `ahSinceFull`, `vout_avg`). |
| `vset <float>` | Set the battery max voltage (`Vbat_max`), range 0–999. |
| `iset <float>` | Set the battery current limit (`Ibat_lim`), range 0–999. |

These override the running charger parameters only; use `set-config charger.conf …` to persist.

# Manual PWM Commands

| Command | Description |
| --- | --- |
| `dc <int>` | Set the converter duty cycle directly and switch the charger to manual PWM mode (no tracking, protection still active). A non-zero duty enables sync rectification and the backflow switch unless `reverse_current_paranoia` is set. |
| `+<int>`, `-<int>` | Relative duty-cycle perturbation step. Available both in manual and tracking mode (to test tracker recovery). **Be careful with large positive jumps** — they can cause extreme current transients that destroy the switches. |
| `mppt` | Switch back to MPP tracking mode (only valid while in manual PWM mode). |
| `sweep` | Start a global MPP scan / search. |
| `speed <float>` | Set tracking speed scale, range 0–10 (default 1.0). |
| `measure-coil l0\|ls [steps\|hs] [dwell_ms] [apply]` | Measure the coil on-device by driving a DCM sweep (takes over manual PWM, restores MPPT when done). `l0` sweeps duty and reports the inductance (median over the DCM band); `ls` holds HS and sweeps the low-side count to find the `rect_offset` timing. `apply` writes the result to `coil.conf`. Needs `Vin > Vout` (sun/headroom). Port of `etc/measure_coil.py`; see [Coil Inductance Measurement](Coil%20Inductance%20Measurement.md). |

The following commands require manual PWM mode:

| Command | Description |
| --- | --- |
| `sync [on\|1\|off\|0\|forced]` | Disable/enable the low-side switch (diode emulation / synchronous rectification). `forced` puts the converter into forced-PWM mode and disables the various reverse-current checks. |
| `bf <0\|1>`, `panel <0\|1>` | Disable/enable the backflow (panel) switch. When enabled it allows current to flow from output to input (battery to solar). Requires a configured backflow switch. |
| `short-ls` | Short the low-side switch. Only valid in boost topology with `Vin` near zero (e.g. for a controlled output discharge). |

# Service Commands

The optional non-RT subsystems (`mqtt`, telemetry, `ftp`, `telnet`, `lcd`, `scope`) are managed as
services. Each has its own state, log level, and `enabled` flag persisted in its conf file.

| Command | Description |
| --- | --- |
| `svc` / `svc list` | List all services with state, log level and enabled flag. |
| `svc on <name>` | Enable (persist) and start a service. |
| `svc off <name>` | Disable (persist) and stop a service. |
| `svc restart <name>` / `svc rs <name>` | Restart a service (stop then start); re-reads its conf. |
| `svc log <name> <error\|warn\|info>` | Set and persist a service's log level. |

# Telnet

Use any telnet client to connect on port 23. No password is required. Only one connection at a time.

Connect from Home Assistant:

* install the "Terminal & SSH" add-on
* in the add-on Configuration, add `busybox-extras` to Packages
