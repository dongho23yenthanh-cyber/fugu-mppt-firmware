#pragma once

// TelemetryService: governs the core-0 InfluxDB point flush over UDP (connectionless, so onStart
// only needs Wi-Fi up). Touches the `mppt` global (defined in main.cpp).

#include <WiFi.h>

#include "../service.h"
#include "../mppt.h"
#include "telemetry.h"

extern MpptController mppt;

class TelemetryService : public Service {
public:
    TelemetryService() : Service("telemetry", "/littlefs/conf/tele.conf", /*requiresNetwork*/ true) {}

protected:
    bool onStart() override { return WiFi.isConnected(); } // UDP is connectionless, nothing to open
    void onStop() override {}
    void onTick() override {
        mppt.telemetry();
        telemetryFlushPointsQ(mppt.tele.influxdbHost);
    }
};

inline TelemetryService telemetryService;
