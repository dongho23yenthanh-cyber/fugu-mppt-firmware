#pragma once

#include "freertos/FreeRTOS.h"

#include "math/statmath.h"
#include "portmux_guard.h"
#include "util.h"


/**
 * Thread safe coulomb counter using  trapezoidal integration to track battery DoD.
 * maxDt = 30 s drops integration across BMS dropouts longer than that
 * Thread-safe.
 */
class CoulombCounter {
    TrapezoidalIntegrator<> _integrator{1.f / 3600e6f, 30'000'000UL};
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
    void updateBatCurrent(float ibat) { updateBatCurrent(ibat, wallClockUs()); }

    void updateBatCurrent(float ibat, unsigned long nowUs) {
        PortMuxGuard _{_mux};
        _integrator.add(-ibat, nowUs);
    }

    [[nodiscard]] float ahSinceFull() const {
        PortMuxGuard _{_mux};
        return (float) _integrator.get();
    }

    void markFull() {
        PortMuxGuard _{_mux};
        _integrator.reset();
    }
};
