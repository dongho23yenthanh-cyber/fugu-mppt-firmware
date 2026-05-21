# Configuration files

Hardware behavior and runtime options are **not compiled in**. They live as flat `key=value`
`.conf` files on the device's `littlefs` partition under `/littlefs/conf/`, parsed by `ConfFile`
(`src/conf.h`). Source-of-truth images per board live under `config/` (e.g. `config/fmetal`,
`config/lab/wokwi_mock`).

Editing options:

- `set-config <file>.conf <key> <value>` from the serial / telnet / MQTT / BLE console (in place, no
  re-flash)
- `get-config <file>.conf <key>` reads one back. `get-config <file>.conf` reads the whole file
- `del-config <file>.conf <key>` to remove a value
- FTP when Wi-Fi is up (1 connection, no passive mode), or `etc/config-tool/conf-tool.py`.
- The single-page editor `etc/config-tool/conf-editor.html` which can connect via serial, BLE or read from uploads.
  Export as zip file.
- `./provision.sh <board>` writes a whole `config/<board>` image to the littlefs partition.

Conventions used in the tables below:

- **default** — the value the firmware uses when the key is absent. `—` means the key is
  **required / board-specific** (no built-in default; usually a divider ratio, pin, or limit that
  must come from the board image).
- A missing **current** sensor (`iin`/`iout`) is replaced by a `VirtualSensor` derived from the
  other side and `power_conversion_eff`; channel `255` means absent.


---

## board.conf — pins, buses, ADC/driver wiring

| key                          | unit | type   | default | description                                    |
|------------------------------|------|--------|---------|------------------------------------------------|
| `mcu`                        |      | string | —       | MCU type, e.g. `esp32s3` or `esp32`            |
| `skip_assert`                |      | bool   | 0       | Skip GPIO pull-resistor sanity checks at init  |
| `i2c_sda`                    | GPIO | int    | —       | I2C SDA pin                                    |
| `i2c_scl`                    | GPIO | int    | —       | I2C SCL pin                                    |
| `i2c_freq`                   | Hz   | int    | 100000  | I2C bus clock frequency                        |
| `i2c_port`                   |      | int    | 0       | ESP32 I2C controller index (0 or 1)            |
| `ina22x_alert`               | GPIO | int    | —       | INA226 ALERT pin                               |
| `ina22x_addr`                |      | int    | —       | INA226 I2C address                             |
| `ina22x_resistor`            | Ω    | float  | —       | INA226 current-sense shunt resistance          |
| `ina22x_range`               | A    | float  | —       | INA226 max current range for PGA config        |
| `pwm_freq`                   | Hz   | int    | —       | Converter PWM switching frequency              |
| `pwm_driver_logic`           |      | enum   | —       | Gate driver logic: `HiLi` or `InEn`            |
| `pwm_hi`                     | GPIO | int    | —       | High-side gate driver pin (HiLi mode)          |
| `pwm_li`                     | GPIO | int    | —       | Low-side gate driver pin (HiLi mode)           |
| `pwm_sd`                     | GPIO | int    | —       | Gate driver shutdown/DIS pin (HiLi mode)       |
| `pwm_in`                     | GPIO | int    | —       | Gate driver IN pin (InEn mode)                 |
| `pwm_en`                     | GPIO | int    | —       | Gate driver EN pin (InEn mode)                 |
| `boot_refresh_ns`            | ns   | float  | 1500    | Min LS on-time to refresh HS bootstrap cap     |
| `panel_en`                   | GPIO | int    | —       | Panel/input backflow enable switch pin         |
| `panel_sd`                   | GPIO | int    | —       | Panel/input backflow shutdown switch pin       |
| `led_WS2812` / `led_WS2812B` | GPIO | int    | —       | WS2812 status LED data pin (alt key)           |
| `led_simple`                 | GPIO | int    | —       | Plain on/off status LED pin                    |
| `fan_pwm`                    | GPIO | int    | —       | Cooling fan PWM pin                            |
| `ads_alert`                  | GPIO | int    | —       | ADS1x15 ALERT/RDY pin                          |
| `adc_fake_freq`              | Hz   | int    | 3000    | Mock ADC total fake sample rate (all channels) |

## sensor.conf — channel map, divider ratios, calibration

Global keys:

| key                              | unit | type   | default | description                                                    |
|----------------------------------|------|--------|---------|----------------------------------------------------------------|
| `adc`                            |      | string | —       | Default ADC backend for all channels                           |
| `expected_hz`                    | Hz   | int    | 0       | Expected control-loop sample rate (lower-bound check; 0 = off) |
| `power_conversion_eff`           |      | float  | 0.95    | Assumed converter efficiency for the virtual current sensor    |
| `ignore_calibration_constraints` |      | bool   | 0       | Bypass ADC calibration sanity constraints                      |
| `esp32adc1_sr`                   | Hz   | int    | —       | Internal ADC1 continuous-mode raw sample rate                  |
| `esp32adc1_avg`                  |      | int    | —       | Internal ADC1 hardware averaging count per sample              |

Per-channel keys, prefix `vin_` / `vout_` / `iin_` / `iout_` / `ntc_`:

| suffix      | unit | type   | description                                                                    |
|-------------|------|--------|--------------------------------------------------------------------------------|
| `_adc`      |      | string | ADC backend for this channel                                                   |
| `_ch`       |      | int    | ADC channel index (255 = absent)                                               |
| `_rh`       | Ω    | float  | Voltage divider upper (high-side) resistor (voltage channels)                  |
| `_rl`       | Ω    | float  | Voltage divider lower resistor (voltage channels)                              |
| `_factor`   |      | float  | Linear scale factor raw ADC → physical, sign sets direction (current channels) |
| `_midpoint` |      | float  | Zero/offset midpoint subtracted before scaling (current channels)              |
| `_filt_len` |      | int    | Filter window length (samples)                                                 |

See [Topology notes & examples](#topology-notes--examples) below for worked ACS712 and bare-ESP32 configs.

## limits.conf — protection cutouts

| key                        | unit | type  | default | description                                       |
|----------------------------|------|-------|---------|---------------------------------------------------|
| `vin_max`                  | V    | float | —       | Maximum input voltage before protection cutout    |
| `vin_min`                  | V    | float | —       | Minimum input voltage; below this converter stops |
| `vout_max`                 | V    | float | —       | Maximum output voltage before protection cutout   |
| `iin_max`                  | A    | float | —       | Maximum input current limit                       |
| `iout_max`                 | A    | float | —       | Maximum output current limit                      |
| `iout_short`               | A    | float | —       | Output short-circuit current threshold            |
| `p_max`                    | W    | float | —       | Maximum power (used for thermal derating)         |
| `temp_max`                 | °C   | float | 90      | Maximum temperature before shutdown               |
| `temp_derate`              | °C   | float | —       | Temperature where power derating begins           |
| `reverse_current_paranoia` |      | bool  | 0       | Enable aggressive reverse-current protection      |

## coil.conf — inductor

| key  | unit | type  | default | description                                                                |
|------|------|-------|---------|----------------------------------------------------------------------------|
| `L0` | H    | float | —       | Coil inductance (for ripple-current computation; undershot 5% for DC bias) |

## converter.conf — topology

| key          | unit | type  | default | description                                                |
|--------------|------|-------|---------|------------------------------------------------------------|
| `topo`       |      | enum  | —       | Converter topology: `buck` or `boost`                      |
| `forced_pwm` |      | bool  | 0       | Force CCM PWM even at light loads (see notes below)        |
| `vout_max`   | V    | float | —       | Legacy output voltage limit (real one is in `limits.conf`) |

## charger.conf — battery termination

| key                 | unit | type  | default    | description                                                    |
|---------------------|------|-------|------------|----------------------------------------------------------------|
| `vout_max`          | V    | float | —          | Max pack/output charge-target voltage                          |
| `cv_eoc`            | V    | float | 3.6        | End-of-charge voltage of highest cell at tail current          |
| `cv_float`          | V    | float | 3.37       | Float cell voltage where termination line meets zero current   |
| `vout_max_fallback` | V    | float | build-time | Max output voltage when BMS data missing; 0 disables converter |
| `ibat_max`          | A    | float | 20         | Maximum battery charge current limit                           |
| `bat_c`             | Ah   | float | —          | Effective battery pack capacity                                |
| `tail_c_rate`       | C    | float | 0.05       | End-of-charge tail current as fraction of capacity             |
| `recharge_dod`      |      | float | 0.20       | Depth-of-discharge since full to release termination           |

## tracker.conf — MPPT

| key                 | unit | type  | default | description                                              |
|---------------------|------|-------|---------|----------------------------------------------------------|
| `target_duty_cycle` |      | float | 0.0     | Fixed duty cycle fraction; if > 0 disables MPPT tracking |

---

## wifi.conf — Wi-Fi credentials + roaming

SSIDs are pattern-based; add as many `ssid_<name>` / `ssid_<name>_psk` pairs as needed (also via the
`wifi-add ssid:psk` console command). The primary SSID may instead live in NVS (`wifi_ssid` /
`wifi_psk`), which takes precedence.

| key               | unit | type   | default | description                                                                                                  |
|-------------------|------|--------|---------|--------------------------------------------------------------------------------------------------------------|
| `ssid_<name>`     |      | string | —       | SSID to join                                                                                                 |
| `ssid_<name>_psk` |      | string | —       | Passphrase for that SSID                                                                                     |
| `switch_delay`    | s    | int    | 30      | Keep retrying the lost AP (router reboot) before roaming to another configured network; 0 = roam immediately |

## mqtt.conf — broker + BMS coupling

| key                       | unit | type   | default | description                                             |
|---------------------------|------|--------|---------|---------------------------------------------------------|
| `broker_uri`              |      | string | —       | MQTT broker URI, e.g. `mqtt://host:port`                |
| `username`                |      | string | —       | Broker login username                                   |
| `password`                |      | string | —       | Broker login password                                   |
| `cell_voltages_max_topic` |      | string | —       | Topic for BMS highest cell voltage                      |
| `ibat_topic`              |      | string | —       | Topic for battery current from BMS                      |
| `ibat_lim_topic`          |      | string | —       | Topic for battery charge current limit                  |
| `cmd_input`               |      | bool   | 0       | Accept console commands over MQTT (`pv/log/<host>/cmd`) |
| `enabled`                 |      | bool   | 1       | Start this service at boot                              |
| `log_level`               |      | enum   | info    | Verbosity: `error`, `warn` or `info`                    |

## tele.conf — InfluxDB telemetry

| key             | unit | type   | default | description                                        |
|-----------------|------|--------|---------|----------------------------------------------------|
| `influxdb_host` |      | string | —       | InfluxDB host IP for UDP telemetry (port 8086)     |
| `compressor`    |      | string | tamp    | Binary wire compressor (`WITH_BINARY_TELE` builds) |
| `enabled`       |      | bool   | 1       | Start this service at boot                         |
| `log_level`     |      | enum   | info    | Verbosity: `error`, `warn` or `info`               |

## ftp.conf — config access over LAN

NVS credentials take precedence; the conf values are the fallback.

| key         | unit | type   | default | description                          |
|-------------|------|--------|---------|--------------------------------------|
| `ftp_user`  |      | string | —       | FTP username                         |
| `ftp_pass`  |      | string | —       | FTP password                         |
| `enabled`   |      | bool   | 1       | Start this service at boot           |
| `log_level` |      | enum   | info    | Verbosity: `error`, `warn` or `info` |

## telnet.conf — remote console

| key         | unit | type | default | description                          |
|-------------|------|------|---------|--------------------------------------|
| `enabled`   |      | bool | 1       | Start this service at boot           |
| `log_level` |      | enum | info    | Verbosity: `error`, `warn` or `info` |

## scope.conf — raw ADC streaming

| key         | unit | type | default | description                          |
|-------------|------|------|---------|--------------------------------------|
| `enabled`   |      | bool | 1       | Start this service at boot           |
| `log_level` |      | enum | info    | Verbosity: `error`, `warn` or `info` |

## lcd.conf — status display

| key         | unit | type | default | description                                |
|-------------|------|------|---------|--------------------------------------------|
| `addr`      |      | int  | 0       | LCD I2C address (0 = auto-probe 0x27/0x3F) |
| `enabled`   |      | bool | 0       | Start this service at boot                 |
| `log_level` |      | enum | info    | Verbosity: `error`, `warn` or `info`       |

## ble.conf — BLE/NUS console (`WITH_BLE` builds)

| key            | unit | type   | default   | description                                  |
|----------------|------|--------|-----------|----------------------------------------------|
| `ble_security` |      | string | justworks | Pairing mode: `justworks` or `secure`        |
| `ble_passkey`  |      | int    | 0         | Static passkey for secure pairing (0 = none) |
| `enabled`      |      | bool   | 1         | Start this service at boot                   |
| `log_level`    |      | enum   | info      | Verbosity: `error`, `warn` or `info`         |

## pprof.conf — sampling profiler

| key            | unit | type | default | description                                                             |
|----------------|------|------|---------|-------------------------------------------------------------------------|
| `sprofiler_hz` | Hz   | int  | 0       | Sampling profiler frequency, ~100–300 (needs OpenOCD attached); 0 = off |

---

# Topology notes & examples

## Sensors

Configure the voltage and current sensors in `sensor.conf` to match your topology and chips.

There are four sensors (Vin, Vout, Iin, Iout). A topology can have one or two current sensors; with
a single current sensor the other is computed from the voltage ratio and `power_conversion_eff`. The
full key list is in [sensor.conf](#sensorconf--channel-map-divider-ratios-calibration) above.

```
adc = ina226         # ADC backend for all channels (ina226, ads1015, ads1115, esp32adc1)

vin_ch = 2           # Vin ADC channel
vin_rh = 200e3       # voltage divider, upper (high-side) resistor
vin_rl = 7.5e3       # voltage divider, lower resistor

vout_ch = 0          # Vout ADC channel
vout_rh = 47e3       # voltage divider, upper resistor
vout_rl = 47e3       # voltage divider, lower resistor

iout_ch = 1          # Iout ADC channel
iout_factor = 1      # raw -> A scale (sign sets direction)
iout_midpoint = 0    # zero offset
iout_filt_len = 30   # filter window (samples)

#iin_ch = 255        # 255 = absent -> Iin becomes a virtual sensor
#iin_factor = -20.15 # sensitivity (A/V)
#iin_midpoint = 1.88 # zero offset (e.g. ACS712)
#iin_filt_len = 30

expected_hz = 80             # loop-rate watchdog lower bound (0 disables)
power_conversion_eff = 0.97  # assumed efficiency for the virtual current sensor
```

## ADC

Pick the ADC backend with `adc`, or per channel with `<chn>_adc`. Implemented:

* `ina226`
* `ads1115`
* `ads1015`
* `esp32adc1` — internal continuous-mode ADC, no external chip (see [Internal ADC.md](Internal%20ADC.md))

Not yet implemented:

* `ina228`

## Voltage sensors `vin`, `vout`

Specify the resistor values of the ADC input voltage divider network.
The firmware uses (hardcoded) ADC input impedance and resistor values to compute the gain.

```
vout_rh = 47e3    # upper resistor of voltage divider
vout_rl = 47e3    # lower resistor
```

## Converter.conf

### `forced_pwm`

With the default value `0`, the converter runs in DCM under light load. The controller uses the coil
inductance (`coil.conf::L0`) plus the input and output voltages to decide whether to operate in DCM
or CCM.

Set to `1` to disable diode emulation and always run in CCM — *forced PWM* mode, with these
characteristics:

* less output noise, because the inductor never free-wheels
  (see [wave forms](https://www.nisshinbo-microdevices.co.jp/en/faq/083.html))
* much better output regulation during load changes, useful for a PSU
* lower efficiency: reverse coil current shuttles energy back and forth between output and input
* a buck converter in forced PWM can easily boost voltage from output back to input

Forced PWM is useful if you want to use the converter as power supply.

## ACS712

The ACS712 sensitivity is 66mV/A. Output is scaled with a 10k+3.3k voltage divider to match the ADC voltage range.
This is encoded into `iin_factor`.

Specify the ACS712 midpoint voltage with `iin_midpoint` (or `iout_midpoint`). This ACS712 has a
2.5V midpoint, scaled through the same 10k + 3.3k divider: `2.5V * 10k/(10k+3.3k)`.

```
iin_factor=-20.15  # sensitivity = -1/0.066 * (10k+3.3k)/10k
iin_midpoint=1.88  # midpoint    = 2.5V * 10k/(10k+3.3k)
```

## Bare ESP32 example

A minimal `sensor.conf` for an ESP32 with no external ADC:

```
adc = esp32adc1

vin_ch = 4          # ch4 = GPIO5
vin_rh = 200e3      # voltage divider, upper resistor
vin_rl = 1.5e3      # voltage divider, lower resistor

vout_ch = 5         # ch5 = GPIO6
vout_rh = 47e3      # voltage divider, upper resistor
vout_rl = 1e3       # voltage divider, lower resistor

iin_ch = 3          # ch3
iin_factor = 20
iin_midpoint = 0
iin_filt_len = 30

iout_filt_len = 30  # Iout is the virtual sensor here

expected_hz = 80
power_conversion_eff = 0.97

ignore_calibration_constraints = 1  # skip noise/range checks (NOT for production!)
```

This runs the firmware on a bare ESP32, useful for testing things other than the ADC and PWM. With
the ADC pins left floating the readings are garbage with a high stddev, which is why
`ignore_calibration_constraints = 1` is needed here.
