#include "sensor_setup.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <string>
#include <unordered_map>

#include "adc/adc.h"
#include "adc/ads.h"
#include "adc/adc_esp32_cont.h"
#include "adc/mock.h"
#include "adc/ina226.h"
#include "adc/sampling.h"
#include "mppt.h"
#include "conf.h"
#include "util.h"

// Globals owned by main.cpp.
extern ADC_Sampler adcSampler;
extern VIinVout<const Sensor *> sensors;
extern MpptController mppt;
extern float conversionEfficiency;
extern uint16_t loopRateMin;

static AsyncADC<float> *createAdcInstance(const std::string &adcName, const ConfFile &boardConf,
                                          const ConfFile &sensConf, const std::string &chnDebug) {
    AsyncADC<float> *adc;
    if (adcName == "ads1115" or adcName == "ads1015") {
        adc = new ADC_ADS(adcName == "ads1115");
    } else if (adcName == "ina226") {
        adc = new ADC_INA226();
    } else if (adcName == "esp32adc1") {
        // assert_throw(ntc_ch== 255, "adc1 conflicts ntc impl TODO fix");
        adc = new ADC_ESP32_Cont(sensConf);
        ((ADC_ESP32_Cont *) adc)->setNtcCh(sensConf.getByte("ntc_ch", 255));
    } else if (adcName == "fake") {
        adc = new ADC_Fake();
    } else {
        throw std::runtime_error("unknown ADC '" + adcName + "'" + " " + chnDebug);
    }

    if (!adc->init(boardConf)) {
        delete adc;
        throw std::runtime_error("failed to initialize ADC " + adcName);
    }

    return adc;
}

void setupSensors(const ConfFile &boardConf, const Limits &lim) {
    loopWallClockUs_ = micros();
    heap_caps_check_integrity_all(true);

    /// compute voltage factor for a resistor divider network with 2 parallel R_low (Rh+(Rl+Ra))
    auto adcVDiv = [](float Rh, float Rl, float Ra) {
        auto rl = 1 / (1 / Rl + 1 / Ra);
        return LinearTransform{(Rh + rl) / rl, 0.f};
    };

    ConfFile sensConf{"/littlefs/conf/sensor.conf"};

    if (!sensConf) {
        throw std::runtime_error("no sensor conf");
    }

    loopRateMin = sensConf.getByte("expected_hz", 0);
    conversionEfficiency = sensConf.f("power_conversion_eff", 0.95f);
    assert_throw(conversionEfficiency > 0.5f and conversionEfficiency < 1.0f, "");

    struct p {
        SensorParams params;
        AsyncADC<float> *adc;
        uint16_t filtLen;
    };
    std::unordered_map<std::string, p> params;

    auto defAdcName = sensConf.getString("adc", "");
    std::unordered_map<std::string, AsyncADC<float> *> adcs{};
    for (auto chn_: {"ntc", "vin", "iin", "iout", "vout",}) {
        auto chn = std::string(chn_);
        auto chNum = sensConf.getByte(chn + '_' + "ch", 255);
        auto an = sensConf.getString(chn + '_' + "adc", defAdcName);

        if (chNum != 255 && adcs.find(an) == adcs.end()) {
            adcs[an] = createAdcInstance(an, boardConf, sensConf, chn);
        }
        auto adc = chNum != 255 ? adcs[an] : nullptr;

        LinearTransform lt{1.f, 0.f};

        if (chNum != 255) {
            if (chn[0] == 'v') {
                lt = adcVDiv(
                    sensConf.f(chn + '_' + "rh"),
                    sensConf.getFloat(chn + '_' + "rl"),
                    adc->getInputImpedance(chNum)
                );
            } else if (chn[0] == 'i') {
                lt = {
                    sensConf.f(chn + '_' + "factor", 1.f),
                    sensConf.f(chn + '_' + "midpoint", 0.f)
                };
            }
        }

        auto filtLen = (uint16_t) sensConf.getLong(chn + '_' + "filt_len");

        params.emplace(chn, p{
                           .params = SensorParams{
                               .adcCh = chNum,
                               .transform = lt,
                               .calibrationConstraints = {},
                               .teleName = chn,
                               .unit = ' ',
                               .rawTelemetry = false,
                           },
                           .adc = adc,
                           .filtLen = filtLen,
                       });

        //ESP_LOGI("main", "Initialized ADC %s (V/I)(i/o)_ch = (%i %i %i %i) , exp.LoopRate=%hu", adcName.c_str(), Vin_ch,
        //         Iin_ch,
        //         Vout_ch, Iout_ch, loopRateMin);
    }

    //Vin_transform.factor *= sensConf.f("vin_calib", 1.f);
    //Vout_transform.factor *= sensConf.f("vout_calib", 1.f);

    params.find("vin")->second.params.calibrationConstraints = {lim.Vin_max, 1.8f, false};
    params.find("vin")->second.params.unit = 'V';
    params.find("vin")->second.params.rawTelemetry = true;
    sensors.Vin = adcSampler.addSensor(params.find("vin")->second.adc,
                                       params.find("vin")->second.params,
                                       lim.Vin_max,
                                       params.find("vin")->second.filtLen); // filtLen = 60


    {
        auto &iin(params.find("iin")->second);

        if (iin.params.adcCh != 255) {
            iin.params.calibrationConstraints = {lim.Iin_max * 0.05f, .1f, true};
            iin.params.unit = 'A';
            sensors.Iin = adcSampler.addSensor(
                iin.adc,
                iin.params,
                lim.Iin_max,
                iin.filtLen);
        } else {
            sensors.Iin = adcSampler.addVirtualSensor(
                [&]() {
                    if (std::abs(sensors.Iout->last) < .01f or sensors.Vin->last < 0.1f)
                        return 0.f;
                    return sensors.Iout->last * sensors.Vout->last / sensors.Vin->last / conversionEfficiency;
                },
                iin.filtLen,
                iin.params.teleName.c_str(),
                iin.params.unit);
        }
    }


    {
        auto &iout(params.find("iout")->second);

        if (iout.params.adcCh != 255) {
            iout.params.calibrationConstraints = {lim.Iout_max * 0.05f, .1f, true};
            iout.params.unit = 'A';
            iout.params.rawTelemetry = true;
            sensors.Iout = adcSampler.addSensor(
                iout.adc,
                iout.params,
                lim.Iout_max,
                iout.filtLen);
        } else {
            sensors.Iout = adcSampler.addVirtualSensor(
                [&]() {
                    if (std::abs(sensors.Iin->last) < .05f or sensors.Vout->last < 0.1f)
                        return 0.f;
                    return sensors.Iin->last * sensors.Vin->last / sensors.Vout->last * conversionEfficiency;
                },
                iout.filtLen,
                iout.params.teleName.c_str(),
                iout.params.unit
            );
        }
    }

    {
        auto &ntc(params.find("ntc")->second);
        if (ntc.params.adcCh != 255) {
            ntc.params.calibrationConstraints = {2.8f, .1f, false};
            ntc.params.unit = 'C';
            auto sense = adcSampler.addSensor(ntc.adc, ntc.params, 2.8f, 50);
            mppt.ntc.setValueRef(sense->ewm.avg.get());
        }
    }


    {
        // notice that Vout should be the last sensor for lowest latency
        auto &vout(params.find("vout")->second);
        vout.params.calibrationConstraints = {lim.Vout_max, .7f, false};
        vout.params.unit = 'V';
        sensors.Vout = adcSampler.addSensor(vout.adc, vout.params, lim.Vout_max, vout.filtLen); // 60
    }

    adcSampler.ignoreCalibrationConstraints = sensConf.getByte("ignore_calibration_constraints", 0);
    if (adcSampler.ignoreCalibrationConstraints)
        ESP_LOGW("main", "Skipping ADC range and noise checks.");
    heap_caps_check_integrity_all(true);
}
