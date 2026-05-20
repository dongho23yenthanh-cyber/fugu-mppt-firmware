#pragma once

// TelnetService: remote console on port 23 while Wi-Fi is up. The telnet client also doubles as a
// log sink, so onStop() drops that sink before the server dies. Owns the ESPTelnet object, its
// event handlers and lifecycle (implemented in telnet_service.cpp); no dependency on the main.cpp
// globals.

#include <ESPTelnet.h>

#include "../service.h"

class TelnetService : public Service {
public:
    TelnetService() : Service("telnet", "/littlefs/conf/telnet.conf", /*requiresNetwork*/ true) {
    }


protected:
    bool onStart() override;

    void onStop() override;

    void onTick() override;

private:
    void setupTelnet();

    void closeConnection();

    // ESPTelnet event handlers. Static (function-pointer callbacks), so they reach the server
    // through the global `telnetService` instance rather than `this`.
    static void onConnect(String ip);

    static void onDisconnect(String ip);

    ESPTelnet telnet;
};

inline TelnetService telnetService;
