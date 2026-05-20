# todo

- questdb binary wire protocol
- hardware tests https://github.com/fl4p/fugu-mppt-firmware/issues/44
    - backflow switch
    - HS switch (and short LS)
    - output impedance measurement https://github.com/fl4p/fugu-mppt-firmware/issues/47
        -
            * detect high impedance battery connection

        * https://h.fabi.me/grafana/d/f4a22deb-8528-427d-9473-4e7b06c6d874/fugu-mppt?orgId=1&from=1741693099256&to=1741704737323

- move away from floating point arithmetic (https://github.com/espressif/idf-extra-components/tree/master/iqmath)
- make notch filter pluggable
  -provisioning overrides
    * eg ina22x_resistor, voltage dividers etc that persists when flashing a new provisioning image
    * also wifi, coil. "extend" a config set?

$$V_{term} = CVmin + I \cdot \frac{CVmax - CVmin}{k\cdot C} $$

Once the highest cell reaches this voltage, the charger limits pack voltage to the current value.

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

```

TODO nan is injected into filtering

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


* boost mode, forced_pwm, reverse current
    * the largerDecrease update block causes excessive reverse current into the power supply (bat/LV terminal)
    * ignoring largerDecrease fixes the problem (TODO: verify, is this still true?)
* iout midpoint calibration fix! with ina226 we get an offset of
  0.9A ! https://github.com/fl4p/fugu-mppt-firmware/issues/28
* store last warnings, errors
* simulate ina226 external reset
* inspect ADC noise with scope-client
* manual sync control to find inductivity

# filtering

- med3 doesnt really help
- consider kalman
- consider smaller f_cut for Iout RC-filter (larger R)
- tracker accumulation buffer







