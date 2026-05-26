#include "scope_service.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <mdns.h>

#include "telemetry.h"   // getHostname

// The global `scope` pointer (declared in scope.h) lives with the service that owns the scope
// object. Was previously aliased from main.cpp, which was a layering inversion.
Scope *scope = &scopeService.scopeObj;

bool ScopeService::onStart() {
    if (!WiFi.isConnected()) return false;
    scopeObj.hostname = getHostname();
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
    mdns_service_remove("_scope", "_tcp"); // else a restart hits "Service already exists" on re-add
}

void ScopeService::onTick() {
    scopeObj.update();
    if (scopeObj.connected) scopeObj.netLoop(); // blocks ~1 tick
}
