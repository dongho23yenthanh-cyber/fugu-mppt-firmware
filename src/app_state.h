#pragma once
// Bundled mode/state flags owned by main.cpp. Single struct so cli.cpp and other consumers see a
// documented surface (`g_app.manualPwm`) instead of a sprawl of file-scope externs. Components
// (mppt, converter, adcSampler, sensors, led, nvs) stay as named globals — they're the right
// granularity already.

#include <cstdint>

struct AppState {
    bool manualPwm = false;
#ifdef WITH_NETW
    bool disableWifi = false;
    uint32_t wifiReenableMs = 0; // wallClockMs() deadline to auto re-enable WiFi (0 = never)
#endif
    bool usbConnected = false;
    bool setupErr = false;
    uint16_t loopRateMin = 0;
    unsigned long maxLoopLag = 0;
#if CAPTURE_LOOP_DT
    unsigned long maxLoopDT = 0;
#endif
};

extern AppState g_app;
