
- write a python script that interacts with the device over the serial console
    - test all possible console commands in a meaningful order
    - accept a cli parameter that informs if the device is using a mock setup (fake ADC readings, not driving any PWM)

set-config ble.conf enabled 0



# mqtt
- test send
  - ha discovery
  - value updates
  - console
- test rx
  - 


# mock adc test
using config/lab/dry_mock configuration files as a base.

## influxdb
this tests if the device generate proper telemetry data to influxdb.
* setup an udp socket listening on port 8086 on the test host (this computer)
*  './provision.py config/lab/dry_mock' (for real devices)
* connect to the device on the serial port
* configure 'tele.conf'
  * set `influxdb_host` to the LAN ip of the test host
  * `enabled 1`
  * reboot device
* now the udp socket should receive data in the InfluxDB line protocol format. Validate the protocol.


# wokwi
* use this configuration set for wokwi: [wokwi_mock](../config/lab/wokwi_mock)
* find the wokwi CI token in `.wifi`
* the wokwi gateway binary is in `~/Downloads/wokwigw_v2.0.1_macOS_ARM64/wokwigw-darwin_arm64`