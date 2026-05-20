#include "scope_service.h"

#include <WiFi.h>
#include <ESPmDNS.h>

bool ScopeService::onStart() {
    if (!WiFi.isConnected()) return false;
    scopeObj.end();
    if (!scopeObj.begin(24)) {
        ESP_LOGE("scope", "scope setup failed");
        return false;
    }
    if (!MDNS.addService("_scope", "_tcp", 24))
        ESP_LOGE("scope", "scope MDNS add failed");
    ESP_LOGI("scope", "Scope server listening on port 24");
    return true;
}

void ScopeService::onStop() {
    scopeObj.end();
    scopeObj.connected = false;
}

void ScopeService::onTick() {
    scopeObj.update();
    if (scopeObj.connected) scopeObj.netLoop(); // blocks ~1 tick; serves as the yield
}
