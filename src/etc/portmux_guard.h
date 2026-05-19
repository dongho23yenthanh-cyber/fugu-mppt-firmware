#pragma once

#include "freertos/FreeRTOS.h" // portMUX_TYPE, portENTER_CRITICAL, portEXIT_CRITICAL

// RAII wrapper around portMUX_TYPE: enters the critical section in the ctor,
// exits in the dtor. Use this rather than the bare portENTER/EXIT pair so
// early returns and exceptions can't leak the lock.
//
// Don't use from an ISR — the IDF *_ISR variants are required there.
//
// The class is [[nodiscard]] so the compiler warns on the classic footgun
// `PortMuxGuard(mux);` (a discarded prvalue temporary that releases the lock
// at the end of the full-expression). Always bind to a named variable:
//   PortMuxGuard _{mux};
class [[nodiscard]] PortMuxGuard {
    portMUX_TYPE &_mux;

public:
    explicit PortMuxGuard(portMUX_TYPE &mux) : _mux(mux) { portENTER_CRITICAL(&_mux); }
    ~PortMuxGuard() { portEXIT_CRITICAL(&_mux); }

    PortMuxGuard(const PortMuxGuard &) = delete;
    PortMuxGuard &operator=(const PortMuxGuard &) = delete;
    PortMuxGuard(PortMuxGuard &&) = delete;
    PortMuxGuard &operator=(PortMuxGuard &&) = delete;
};
