#pragma once
#include "Arduino.h"

// POSIX-socket-backed Client. Same shape as arduino-esp32 WiFiClient.
class WiFiClient : public Client {
public:
    WiFiClient();
    explicit WiFiClient(int fd, IPAddress peer);
    WiFiClient(const WiFiClient &);
    WiFiClient &operator=(const WiFiClient &);
    WiFiClient(WiFiClient &&) noexcept;
    WiFiClient &operator=(WiFiClient &&) noexcept;
    ~WiFiClient() override;

    // Client interface
    int  connect(IPAddress ip, uint16_t port) override;
    int  connect(const char *host, uint16_t port) override;
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    using Print::write;
    int  available() override;
    int  read() override;
    int  read(uint8_t *buf, size_t size) override;
    int  peek() override;
    void flush() override;
    void stop() override;
    uint8_t connected() override;
    operator bool() override;
    IPAddress remoteIP() override;

    // arduino-esp32 API: Nagle-off toggle. No-op on host (loopback).
    void setNoDelay(bool) {}

private:
    // Reference-counted fd so copy-by-value (as arduino-esp32 does) doesn't
    // close the socket out from under the original holder.
    struct Shared {
        int       fd  = -1;
        int       refs = 1;
        IPAddress peer;
    };
    Shared *_s = nullptr;
    void _release();
    void _acquire(Shared *s);
};
