```


> Once the highest cell reaches this voltage, charging is completed!

True, however this doesn't necessarily mean that the charger completely shuts down, if there are loads connected. So I avoided having a "terminated" state by just letting the charger follow the termination condition. In case of zero load, the charger will quickly shut down.




waveshare firmware:

  sta.ssid        : BLOB  WSCSBZY
  sta.pswd        : BLOB  waveshare0755


partitions table:
esptool.py read_flash 0x8000 0xc00 ws-ptable.img
en_esp32part.py ws-ptable.img 
# ESP-IDF Partition Table
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,20K,
otadata,data,ota,0xe000,8K,
app0,app,ota_0,0x10000,1280K,
app1,app,ota_1,0x150000,1280K,
spiffs,data,spiffs,0x290000,1408K,
coredump,data,coredump,0x3f0000,64K,

esptool.py read_flash 0x9000 20k nvs_readout.bin

```


>
> > My bad. It is just another way to express the cutoff-charging logic. There is no difference, they're identical.

I see, I thought with AutoCVL you were referring to the constant cell-voltage regulation.

> (Ah is cell_capacity)

ok, then its the same


I merged the two independent logics I described in my earlier comment and it now computes the termination cell voltage:

$$V_{term} = CVmin + I \cdot \frac{CVmax - CVmin}{k\cdot C} $$

Once the highest cell reaches this voltage, the charger limits pack voltage to the current value.

So I did some initial testing with a 8s pack of 280Ah prismatic Lifepo4 cells from Ali-Express (about 3.5 years old, ~300 cycles).




```
I (7275) ina22x: MfrID: 0x5449, DeviceID: 0x2260

assert failed: bool ADC_INA226::testConvReadyAlert(uint8_t, uint8_t) ina226.h:289 ((tBusyWait - tWrite) < (100 + (140 + 10) * 2 * 4))


Backtrace: 0x4037eee5:0x3fceb700 0x4037eead:0x3fceb720 0x403879d1:0x3fceb740 0x420154c9:0x3fceb860 0x42025cb1:0x3fceb8b0 0x4201eb9a:0x3fceb8f0 0x4201e4a6:0x3fceb9a0 0x420207cd:0x3fceba60 0x42021bd5:0x3fcebea0 0x4200f6ae:0x3fcec1a0 0x4037fc31:0x3fcec1c0
--- 0x4037eee5: panic_abort at /Users/fab/dev/esp/idf5.5/components/esp_system/panic.c:469
--- 0x4037eead: esp_system_abort at /Users/fab/dev/esp/idf5.5/components/esp_system/port/esp_system_chip.c:87
--- 0x403879d1: __assert_func at /Users/fab/dev/esp/idf5.5/components/newlib/src/assert.c:80
--- 0x420154c9: ADC_INA226::testConvReadyAlert(unsigned char, unsigned char) at /Users/fab/dev/pv/fugu-mppt-firmware/src/adc/ina226.h:289
--- 0x42025cb1: ADC_INA226::resetPeripherals() at /Users/fab/dev/pv/fugu-mppt-firmware/src/adc/ina226.h:133
--- 0x4201eb9a: ADC_INA226::init(ConfFile const&) at /Users/fab/dev/pv/fugu-mppt-firmware/src/adc/ina226.h:73
--- 0x4201e4a6: createAdcInstance(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, ConfFile const&, ConfFile const&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&) at /Users/fab/dev/pv/fugu-mppt-firmware/src/main.cpp:113                                              
--- 0x420207cd: setupSensors(ConfFile const&, Limits const&) at /Users/fab/dev/pv/fugu-mppt-firmware/src/main.cpp:158
--- 0x42021bd5: setup() at /Users/fab/dev/pv/fugu-mppt-firmware/src/main.cpp:409
--- 0x4200f6ae: loopTask(void*) at /Users/fab/dev/pv/fugu-mppt-firmware/managed_components/espressif__arduino-esp32/cores/esp32/main.cpp:67
--- 0x4037fc31: vPortTaskWrapper at /Users/fab/dev/esp/idf5.5/components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c:139
```

```
key                                  num       tot   mean    max maxNum    min minNum
adc.update.getSample           104834638 2681848562     25   1913 18278283    162  24089
adc.update.hasData             236982334 756594191      3   1451 222432873      3      6
protect                         23517882 2041885776     86   1423 4965183      4  89421
mppt.update.startSweep                22     18495    840   1035      4    733      2
adc.update.handleSensorCalib   177637329 222435870      1    951 2698051      0      1
mppt.update.pwm                 24280981 661673915     27    928 744390      5 4920830
mppt.startSweep                     1352    849377    628    880    803    460    197
mppt.update.en                  24280982 1103875793     45    845 2801210     12 4361596
mppt.update.stopSweep                117     75009    641    789     93    525    108
mppt.update.tracker             24280982 712484028     29    689 15636108      2 3983425
protectLf                       23517882 388116704     16    355 23517876      0    307
adc.update.addSample           177637335 988071831      5    306 70583958      4      1
mppt.update                     47849552 926762044     19    303 31412587      0      5
mppt.update.control             24280982 1303974673     53    259 6297845     29 4920834
adc.update.read                 83518925 2599547381     31    210 5935929      7   5494
adc.update.AddSampleVirtual    117400470 2394175396     20    164 88952819      4 107786
loopRTNewData                   82780284 512381756      6    154 9718720      0     10
mppt.update.thermals            24281004  82039769      3    141 6297838      1      1
protect.pre                     75258331 525189391      6     88 8048886      0      5
micros                         118491198 211531302      1     79 20612009      1      2
mppt.update.meterAdd            24281004 432160870     17     72 18882841      6    421
start                          118491197 293160562      2     70 12443381      0      2
mppt.update.sweeping             3806195  80043284     21     63 3037180     10      3
adc.update.startReading        104834594 552622292      5     62 18278250      0      1
mppt.startSweep.pre                 1352     22280     16     56    837     10   1309
adc.update                     118491199 129832424      1     53 20612204      0      2
mppt.update.pre                 47849552  60561306      1     31 11455158      0      1
adc.update.pre                 118491199  14118961      0     24 118070477      0      0

```

```
V=45.3/28.01 I=12.1/19.00A 549.1W 35℃37℃ 444sps  0㎅/s CCM(H|L|Lm)=1285| 754| 762 st=⇡MPPT,1 lag=3107㎲ N=384851 rssi=-52
V=45.4/28.06 I=12.1/18.95A 546.4W 35℃37℃ 444sps  0㎅/s CCM(H|L|Lm)=1287| 756| 760 st=⇡MPPT,1 lag=3107㎲ N=386187 rssi=-53
sweep
I (4481957) main: received serial command: 'sweep'
PWM disabled (duty cycle was 1288)

I (4481968) mppt: Start sweep
I (4481972) mppt: Start calibration
I (4481976) sensor: vin reset calibration
I (4481980) sensor: iout reset calibration
I (4481982) sensor: ntc reset calibration
I (4481986) sensor: vout reset calibration
I (4481991) main: OK: sweep
V=69.5/26.35 I= nan/ 0.07A   nanW 35℃37℃ 2743205264sps  0㎅/s CCM(H|L|Lm)=   0|   0| 759 st=   CV,1 lag=3107㎲ N=4 rssi=-53
(186504320): Current above threshold 18.80 (pwm=0)
(186504918): Sync rect enabled
I (4481974) mppt: Stop sweep after 871.45s at controlMode=CV (limIdx=1, tgt=29.00, act=27.88) PWM=0, MPP=(0.0W,PWM=0,0.0V)
I (4482022) plot: Not enough data to plot V
I (4482027) plot: Not enough data to plot D
I (4482050) store: Wrote /littlefs/stats (size 32)
I (4482053) flash: Wrote flash value /littlefs/stats
I (4482426) sampler: Sensor iout calibration: avg=0.1055 std=0.000332
I (4482427) sampler: Sensor iout offset-calibrated: 0.105500
I (4482433) sampler: Sensor vout calibration: avg=26.3244 std=0.000000
I (4482436) sampler: Sensor vin calibration: avg=69.4100 std=0.000000
I (4482875) sampler: Sensor iout calibration: avg=-0.0315 std=0.000457
I (4482875) sampler: Sensor iout offset-calibrated: -0.031536
I (4482875) sampler: Calibration done!
(187412263): Converter enabled
(187412518): Current below threshold nan (pwm=16)
(187412671): Back-flow switch disabled
(187412737): Sync rect disabled
(187421706): converter: CCM -> DCM (M=0.38, I=0.11, ∆I/2=5.51, pwm=16)
I (4482888) mppt: Near MPP, slow-down, set dutyCycle 1294 (powerSample=2.77)
V=69.4/25.91 I=0.05/ 0.12A   3.2W 35℃37℃ 144sps  0㎅/s DCM(H|L|Lm)=  18| 123| 123 st=⇣MPPT,1 lag=3611㎲ N=442 rssi=-53
I (4483880) mppt: Reset maxPower to 0 (<3.007 * 90%)
V=69.4/25.99 I=0.03/ 0.08A   2.3W 35℃37℃ 444sps  0㎅/s DCM(H|L|Lm)=  31| 123| 123 st=↑MPPT,1 lag=3611㎲ N=1778 rssi=-51
V=69.4/25.98 I=0.04/ 0.09A   2.5W 35℃37℃ 443sps  0㎅/s DCM(H|L|Lm)=  37| 123| 123 st=↓MPPT,1 lag=3611㎲ N=3114 rssi=-52
V=69.4/25.89 I=0.03/ 0.09A   2.4W 35℃37℃ 443sps  0㎅/s DCM(H|L|Lm)=  43| 123| 123 st=↑MPPT,1 lag=3611㎲ N=4450 rssi=-53
V=69.3/25.91 I=0.04/ 0.11A   3.0W 35℃37℃ 443sps  0㎅/s DCM(H|L|Lm)=  50| 123| 123 st=↑MPPT,1 lag=3611㎲ N=5787 rssi=-53
V=69.2/25.91 I=0.04/ 0.11A   2.8W 35℃37℃ 443sps  0㎅/s DCM(H|L|Lm)=  54| 123| 123 st=↑MPPT,1 lag=3611㎲ N=7123 rssi=-54
E (4500180) ina22x: 170000 ti
```


```
V=59.8/27.61 I=8.56/18.01A 511.8W 38℃40℃ 443sps  0㎅/s CCM(H|L|Lm)= 993|1054|1054 st=⇡MPPT,1 lag=3611㎲ N=596963 rssi=-51
sweep
I (7726171) main: received serial command: 'sweep'
PWM disabled (duty cycle was 984)

I (7726182) mppt: Start sweep
I (7726187) mppt: Start calibration
I (7726190) sensor: vin reset calibration
I (7726193) sensor: iout reset calibration
I (7726196) sensor: ntc reset calibration
I (7726199) sensor: vout reset calibration
I (7726202) main: OK: sweep
V=70.3/26.52 I= nan/-0.26A   nanW 38℃40℃ 3126459500sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=   CV,1 lag=3611㎲ N=3 rssi=-52
(3430717120): Current above threshold 17.89 (pwm=0)
(3430717724): Sync rect enabled
I (7726188) mppt: Stop sweep after 1346.25s at controlMode=CV (limIdx=1, tgt=29.00, act=27.39) PWM=0, MPP=(0.0W,PWM=0,0.0V)
I (7726241) plot: Not enough data to plot V
I (7726245) plot: Not enough data to plot D
I (7726268) store: Wrote /littlefs/stats (size 32)
I (7726271) flash: Wrote flash value /littlefs/stats
I (7726641) sampler: Sensor iout calibration: avg=0.0958 std=0.000157
I (7726641) sampler: Sensor iout offset-calibrated: 0.095813
I (7726645) sampler: Sensor vout calibration: avg=26.5019 std=nan
I (7726653) sampler: Sensor vin calibration: avg=70.2534 std=0.000000
I (7727089) sampler: Sensor iout calibration: avg=-0.0281 std=0.000636
I (7727090) sampler: Sensor iout offset-calibrated: -0.028057
I (7727090) sampler: Calibration done!
(3431626624): Back-flow switch disabled
V=70.3/26.49 I=0.03/ 0.08A   2.2W 38℃40℃ 172sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=529 rssi=-52
V=70.3/26.48 I=0.02/ 0.05A   1.5W 38℃40℃ 446sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=1871 rssi=-53
V=70.2/26.48 I=0.02/ 0.04A   1.2W 38℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=3212 rssi=-54
V=70.2/26.47 I=0.01/ 0.04A   1.1W 37℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=4554 rssi=-54
V=70.2/26.46 I=0.01/ 0.04A   1.0W 37℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=5895 rssi=-52
V=70.2/26.46 I=0.01/ 0.03A   0.9W 37℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=7236 rssi=-51
V=70.1/26.45 I=0.01/ 0.03A   0.8W 37℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=8577 rssi=-52
V=70.2/26.46 I=0.01/ 0.03A   0.8W 37℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=9918 rssi=-54
V=70.2/26.45 I=0.01/ 0.03A   0.8W 37℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=11259 rssi=-52
V=70.2/26.45 I=0.01/ 0.03A   0.7W 36℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=12600 rssi=-50
V=70.2/26.45 I=0.01/ 0.03A   0.8W 36℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=13941 rssi=-51
V=70.1/26.45 I=0.01/ 0.03A   0.7W 36℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=15282 rssi=-51
V=70.1/26.44 I=0.01/ 0.03A   0.7W 36℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=16623 rssi=-51
V=70.1/26.45 I=0.01/ 0.03A   0.7W 36℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=17964 rssi=-51
V=70.1/26.44 I=0.01/ 0.02A   0.6W 35℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=19305 rssi=-50
V=70.1/26.44 I=0.01/ 0.03A   0.7W 35℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=20646 rssi=-52
V=70.0/26.44 I=0.01/ 0.02A   0.7W 35℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=21988 rssi=-50
V=70.1/26.45 I=0.01/ 0.03A   0.8W 35℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=23330 rssi=-52
V=70.0/26.43 I=0.01/ 0.02A   0.7W 35℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=24672 rssi=-49
V=70.0/26.43 I=0.01/ 0.02A   0.6W 35℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=26014 rssi=-52
V=70.0/26.44 I=0.01/ 0.02A   0.6W 34℃40℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=27356 rssi=-50
V=70.0/26.43 I=0.01/ 0.02A   0.6W 34℃39℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=28698 rssi=-51
V=70.0/26.43 I=0.01/ 0.02A   0.6W 34℃39℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N/A,1 lag=3611㎲ N=30041 rssi=-52
V=70.0/26.43 I=0.01/ 0.02A   0.6W 34℃39℃ 445sps  0㎅/s CCM(H|L|Lm)=   0|   0|1063 st=  N
```


TODO nan is injected into filtering

* provisioning overrides
  * eg ina22x_resistor, voltage dividers etc that persists when flashing a new provisioning image
  * also wifi, coil. "extend" a config set?
* after calibration wait until ewm.avg is finite, filters have settled, then sweep

## components

add named components (mqtt, adc, etc)
* similar to a service
* wraps / abstracts a building block (hw driver or software component)
* interface:
  * begin(), end()
  * status
  * setLogLevel()
  * config namespace?
* user has access over console (e.g. restart, query stats, enable debug log, change config)

* detect high impedance battery connection
  * https://h.fabi.me/grafana/d/f4a22deb-8528-427d-9473-4e7b06c6d874/fugu-mppt?orgId=1&from=1741693099256&to=1741704737323

* 50hz/60hz band stop filter to remove inverter noise (notch filter)
  * https://www.youtube.com/watch?v=tpAA5eUb6eo

* boost mode, forced_pwm, reverse current
  * the largerDecrease update block causes excessive reverse current into the power supply (bat/LV terminal)
  * ignoring largerDecrease fixes the problem
* iout midpoint calibration fix! with ina226 we get an offset of
  0.9A ! https://github.com/fl4p/fugu-mppt-firmware/issues/28
* store last warnings, errors
* simulate ina226 external reset
* inspect ADC noise with scope-client
* manual sync control to find inductivity


# issues

# filtering

- med3 doesnt really help
- consider kalman
- consider smaller f_cut for Iout RC-filter (larger R)
- tracker accumulation buffer




# vibe

## filtering
* evaluate the filter pipeline (adc averaging, notch, med3, ewm). Do the components make sense? Is the order correct?
any other filter recommendation (kalman, multi-pass ewm ..) for the digital control loop? (Vout is the critical control variable)
* when the user connects to the console, i want the charger to print the last 20 warnings and errors
* detect high impedance battery connection






