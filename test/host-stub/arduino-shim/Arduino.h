// Reusable Arduino host-test shim. Provides just enough of the Arduino C++
// API to compile and run Arduino libraries (FtpServer, ESPTelnet, etc.) as
// plain host processes. Backed by BSD sockets and std::filesystem.
//
// Pulled in via -I test/host-stub/arduino-shim ahead of any real Arduino
// include path. Pair with arduino_shim.cpp for the runtime implementations.

#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef ARDUINO
#define ARDUINO 200
#endif
#ifndef ARDUINO_ARCH_ESP32
#define ARDUINO_ARCH_ESP32
#endif
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 3
#endif

// ---- pgmspace -------------------------------------------------------------
#define PROGMEM
#ifndef PSTR
#define PSTR(x) (x)
#endif
#ifndef FPSTR
#define FPSTR(x) (x)
#endif
#ifndef F
#define F(x) (x)
#endif
#define strcmp_P  strcmp
#define strncmp_P strncmp
#define strcpy_P  strcpy
#define strncpy_P strncpy
#define snprintf_P snprintf
#define vsnprintf_P vsnprintf
#define strcmp_PF  strcmp
#define strncmp_PF strncmp

// ---- timing ---------------------------------------------------------------
uint32_t millis();
void     delay(uint32_t ms);
void     delayMicroseconds(uint32_t us);
void     yield();

// ---- String ---------------------------------------------------------------
class String {
public:
    String() = default;
    String(const char *s) : _s(s ? s : "") {}
    String(const std::string &s) : _s(s) {}
    String(int n)           { char b[16]; std::snprintf(b, sizeof(b), "%d",  n); _s = b; }
    String(unsigned n)      { char b[16]; std::snprintf(b, sizeof(b), "%u",  n); _s = b; }
    String(long n)          { char b[24]; std::snprintf(b, sizeof(b), "%ld", n); _s = b; }
    String(unsigned long n) { char b[24]; std::snprintf(b, sizeof(b), "%lu", n); _s = b; }

    const char *c_str() const { return _s.c_str(); }
    size_t      length() const { return _s.size(); }
    char        operator[](size_t i) const { return _s[i]; }

    String &operator+=(const char *s)   { _s.append(s ? s : ""); return *this; }
    String &operator+=(const String &o) { _s.append(o._s);       return *this; }
    String &operator+=(char c)          { _s.push_back(c);       return *this; }

    String operator+(const char *s)   const { String r=*this; r += s; return r; }
    String operator+(const String &o) const { String r=*this; r += o; return r; }

    bool operator==(const char *s)   const { return _s == (s ? s : ""); }
    bool operator==(const String &o) const { return _s == o._s; }
    bool operator!=(const char *s)   const { return !(*this == s); }

private:
    std::string _s;
};

inline String operator+(const char *l, const String &r) { return String(l) + r; }

// ---- IPAddress ------------------------------------------------------------
class IPAddress {
public:
    IPAddress() : _b{0,0,0,0} {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : _b{a,b,c,d} {}
    explicit IPAddress(uint32_t v) {
        _b[0] = (uint8_t)(v);       _b[1] = (uint8_t)(v >> 8);
        _b[2] = (uint8_t)(v >> 16); _b[3] = (uint8_t)(v >> 24);
    }
    uint8_t &operator[](int i)             { return _b[i]; }
    uint8_t  operator[](int i) const       { return _b[i]; }
    bool     operator==(const IPAddress &o) const {
        return _b[0]==o._b[0] && _b[1]==o._b[1] && _b[2]==o._b[2] && _b[3]==o._b[3];
    }
    uint32_t v4() const {
        return (uint32_t)_b[0] | ((uint32_t)_b[1] << 8)
             | ((uint32_t)_b[2] << 16) | ((uint32_t)_b[3] << 24);
    }
    operator uint32_t() const { return v4(); }
    String toString() const {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", _b[0], _b[1], _b[2], _b[3]);
        return String(buf);
    }
private:
    uint8_t _b[4];
};

// ---- Print / Stream / Client / Server -------------------------------------
class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buf, size_t n) {
        size_t w = 0; while (n--) w += write(*buf++); return w;
    }
    size_t write(const char *s) { return s ? write((const uint8_t *)s, std::strlen(s)) : 0; }

    size_t print(const char *s)          { return write(s); }
    size_t print(const String &s)        { return write(s.c_str()); }
    size_t print(char c)                 { return write((uint8_t)c); }
    size_t print(int n)                  { char b[16]; int k=std::snprintf(b,sizeof(b),"%d",n);  return write((const uint8_t*)b,(size_t)k); }
    size_t print(unsigned n)             { char b[16]; int k=std::snprintf(b,sizeof(b),"%u",n);  return write((const uint8_t*)b,(size_t)k); }
    size_t print(long n)                 { char b[24]; int k=std::snprintf(b,sizeof(b),"%ld",n); return write((const uint8_t*)b,(size_t)k); }
    size_t print(unsigned long n)        { char b[24]; int k=std::snprintf(b,sizeof(b),"%lu",n); return write((const uint8_t*)b,(size_t)k); }

    size_t println()                     { return write((const uint8_t*)"\r\n", 2); }
    size_t println(const char *s)        { size_t w=write(s); return w + println(); }
    size_t println(const String &s)      { return println(s.c_str()); }
    template <class T> size_t println(T v){ size_t w=print(v); return w + println(); }

    int printf(const char *fmt, ...) {
        char buf[256];
        va_list ap; va_start(ap, fmt);
        int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n > 0) write((const uint8_t *)buf, (size_t)std::min(n, (int)sizeof(buf) - 1));
        return n;
    }
    virtual void flush() {}
};

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t readBytes(char *buf, size_t n) {
        size_t r = 0;
        while (r < n) { int c = read(); if (c < 0) break; buf[r++] = (char)c; }
        return r;
    }
};

class Client : public Stream {
public:
    using Stream::read;
    virtual int  connect(IPAddress ip, uint16_t port) = 0;
    virtual int  connect(const char *host, uint16_t port) = 0;
    virtual int  read(uint8_t *buf, size_t size)          = 0;
    virtual void stop()                                   = 0;
    virtual uint8_t connected()                           = 0;
    virtual operator bool()                               = 0;
    virtual IPAddress remoteIP()                          = 0;
};

class Server : public Print {
public:
    virtual void begin(uint16_t port = 0) = 0;
};

// ---- Serial stub (referenced by some libs at link time) -------------------
struct _HostSerial : public Stream {
    void begin(unsigned long) {}
    int  available() override { return 0; }
    int  read() override      { return -1; }
    int  peek() override      { return -1; }
    size_t write(uint8_t c) override { std::putchar(c); return 1; }
    using Print::write;
};
extern _HostSerial Serial;

// ESP class stub — FtpServer.cpp uses ESP.getFreeHeap() for diagnostics.
struct _HostESP {
    uint32_t getFreeHeap() const { return 0xFFFFFFu; }
};
extern _HostESP ESP;

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
