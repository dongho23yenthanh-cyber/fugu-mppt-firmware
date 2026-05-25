#pragma once
#include <cstdint>
#include <cstdio>

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

    // Lightweight toString() — returns a stack-allocated buffer wrapper.
    struct _Str { char buf[16]; const char *c_str() const { return buf; } };
    _Str toString() const {
        _Str s;
        std::snprintf(s.buf, sizeof(s.buf), "%u.%u.%u.%u", _b[0], _b[1], _b[2], _b[3]);
        return s;
    }

private:
    uint8_t _b[4];
};
