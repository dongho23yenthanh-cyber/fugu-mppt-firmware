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
    TelemetryService() : Service("tele", "/littlefs/conf/tele.conf", /*requiresNetwork*/ true) {}

    // The UDP target it flushes points to, once a host is configured/resolved.
    std::string statusDetail() const override {
        const auto &host = mppt.tele.influxdbHost;
        return host ? std::string("→ ") + host.toString().c_str() : std::string();
    }

protected:
    bool onStart() override { return WiFi.isConnected(); } // UDP is connectionless, nothing to open
    void onStop() override {}
    void onTick() override {
        mppt.telemetry();
        telemetryFlushPointsQ(mppt.tele.influxdbHost);
    }
};

inline TelemetryService telemetryService;
