#pragma once


#include "../service.h"
#include "lcd.h"
#include "../adc/sampling.h"
#include "../mppt.h"

extern ADC_Sampler adcSampler;
extern VIinVout<const Sensor *> sensors;
extern MpptController mppt;

class LcdService : public Service {
public:
    LCD lcd;

    LcdService() : Service("lcd", "/littlefs/conf/lcd.conf", /*requiresNetwork*/ false,
                           /*enabledDefault*/ false) {}


    bool initLcd() {
        if (lcd) return true;
        // addr=0 (unset) is fine: LCD::init() auto-probes the standard 0x27/0x3F addresses and
        // returns false if nothing answers on the bus.
        uint8_t addr = ConfFile{_confPath, /*no_warn_if_not_open*/ true}.getByte("addr", 0);
        return lcd.init(addr);
    }

    // Forward to the owned LCD so callers use the service handle, not the wrapped lcd object.
    void displayMessage(const std::string &msg, uint16_t timeoutMs) { lcd.displayMessage(msg, timeoutMs); }

    [[nodiscard]] std::string statusDetail() const override { return lcd ? "ok" : ""; }

protected:

    bool onStart() override { return initLcd(); }
    void onStop() override { } // no hard teardown; tick simply stops when not Running
    void onTick() override {
        if (lcd && !adcSampler.adcStates.empty())
            lcd.updateValues(LcdValues{
                .Vin = sensors.Vin->ewm.avg.get(),
                .Vout = sensors.Vout->ewm.avg.get(),
                .Iin = sensors.Iin->ewm.avg.get(),
                .Iout = sensors.Iout->ewm.avg.get(),
                .Temp = mppt.ntc.last(),
            });
    }
};

inline LcdService lcdService;
