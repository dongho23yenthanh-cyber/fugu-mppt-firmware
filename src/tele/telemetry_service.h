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
    bool onStart() override {
        if (!WiFi.isConnected()) return false;          // UDP is connectionless, nothing to open
        xTaskCreatePinnedToCore(flushTask, "teleflush", 4096,
                                &mppt.tele.influxdbHost, 1, &_flushTask, 0);
        return true;
    }
    void onStop() override {                            // delete the task -> frees CPU + its stack
        if (_flushTask) { vTaskDelete(_flushTask); _flushTask = nullptr; }
    }
    void onTick() override {
        mppt.telemetry();   // produce only; flushTask does the compress+send
    }

private:
    // Drains/compresses/sends the point queue on its own core-0 task, so compression (tens of ms
    // with tamp) never stalls production. SPSC: producer = onTick thread, consumer = this task.
    [[noreturn]] static void flushTask(void *arg) {
        const IPAddress *host = static_cast<const IPAddress *>(arg);
        for (;;) {
            telemetryFlushPointsQ(*host);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    TaskHandle_t _flushTask = nullptr;
};

inline TelemetryService telemetryService;
