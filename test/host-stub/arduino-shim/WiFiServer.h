#pragma once
#include "Arduino.h"
#include "WiFiClient.h"

// POSIX listening socket. Matches the subset of arduino-esp32 WiFiServer
// used by SimpleFTPServer.
class WiFiServer : public Server {
public:
    explicit WiFiServer(uint16_t port = 0);
    ~WiFiServer() override;
    void begin(uint16_t port = 0) override;
    void end();
    WiFiClient accept();
    bool hasClient();
    operator bool() const { return _listenFd >= 0; }

    // Print interface — unused but Server requires it.
    size_t write(uint8_t) override { return 0; }
    using Print::write;

    // Discover the ephemeral port the kernel assigned after begin(0).
    uint16_t port() const { return _port; }

private:
    int      _listenFd = -1;
    uint16_t _port = 0;
};
