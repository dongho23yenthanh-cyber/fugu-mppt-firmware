#pragma once

// ScopeService: streams raw ADC over TCP port 24 for noise debugging, while Wi-Fi is up. Owns the
// scope object (server begin/end + net pump, implemented in scope_service.cpp). main.cpp's setup()
// registers the data channel on it via the global `scope` pointer (declared in scope.h), which the
// RT/ADC sampling path uses for lightweight access.

#include "../service.h"
#include "scope.h"

class ScopeService : public Service {
public:
    // The scope this service owns. The global `scope` pointer (declared in scope.h) is aimed at
    // this member so the RT/ADC sampling path keeps its lightweight pointer access; main.cpp's
    // setup() registers the data channel on it.
    Scope scopeObj;

    ScopeService() : Service("scope", "/littlefs/conf/scope.conf", /*requiresNetwork*/ true) {}

protected:
    bool onStart() override;
    void onStop() override;
    void onTick() override;
};

inline ScopeService scopeService;
