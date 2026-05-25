#pragma once
#include "WiFiClient.h"
#include "WiFiServer.h"

// Stand-in for the arduino-esp32 WiFi global. Only the methods FtpServer
// actually calls during command processing are implemented.
class _HostWiFi {
public:
    IPAddress localIP() const    { return IPAddress(127, 0, 0, 1); }
    IPAddress subnetMask() const { return IPAddress(255, 0, 0, 0); }
    IPAddress softAPIP() const   { return IPAddress(127, 0, 0, 1); }
};
extern _HostWiFi WiFi;
