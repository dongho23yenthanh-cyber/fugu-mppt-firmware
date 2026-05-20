#pragma once

#include <string>

struct LcdValues {
    float Vin;
    float Vout;
    float Iin;
    float Iout;
    float Temp;
};

class LiquidCrystal_I2C;

class LCD {
    LiquidCrystal_I2C *lcd = nullptr;
    unsigned long lastDrawTime = 0;
    unsigned long msgUntil = 0;
    unsigned long lightUntil = 0;

    unsigned long lastInit = 0;
public:
    // configuredAddr (if nonzero) is probed first; init falls back to the standard PCF8574 backpack
    // addresses {0x27, 0x3F} when it's zero or doesn't ACK on the bus.
    bool init(uint8_t configuredAddr = 0);
    void periodicInit();

    void displayMessage(const std::string &msg, uint16_t timeoutMs);
    void displayMessageF(const std::string &msg, uint16_t timeoutMs, ...);

    void updateValues(const LcdValues &values);

    explicit operator bool() const {
        return lcd != nullptr;
    }
};