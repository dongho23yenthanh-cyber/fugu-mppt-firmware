#pragma once

#include <atomic>

#include "math/statmath.h"
#include "util.h"


/**
 * Coulomb counter tracking battery DoD via trapezoidal integration.
 * maxDt = 30 s drops integration across BMS dropouts longer than that.
 *
 * Lock-free single-core SPSC. The integrator is mutated only by the producer
 * (BMS/MQTT task) in updateBatCurrent(); the consumer (loop task) reads the
 * published Ah snapshot and requests a reset via markFull(). Producer and
 * consumer are both pinned to core 0, so a 32-bit atomic load/store (lock-free
 * on Xtensa) is enough — no critical section.
 */
class CoulombCounter {
    TrapezoidalIntegrator<> _integrator{1.f / 3600e6f, 30'000'000ULL}; // producer-only
    std::atomic<float> _ahSinceFull{0.f}; // single writer: producer
    std::atomic<bool> _resetRequested{false}; // consumer -> producer

public:
    void updateBatCurrent(float ibat) { updateBatCurrent(ibat, wallClockUs()); }

    // producer thread
    void updateBatCurrent(float ibat, time_us nowUs) {
        if (_resetRequested.exchange(false, std::memory_order_relaxed))
            _integrator.reset();
        _integrator.add(-ibat, nowUs);
        _ahSinceFull.store((float) _integrator.get(), std::memory_order_relaxed);
    }

    // consumer thread. A pending markFull() reads as 0 until the producer
    // honors it, so the integrator stays single-writer without a revert window.
    [[nodiscard]] float ahSinceFull() const {
        if (_resetRequested.load(std::memory_order_relaxed)) return 0.f;
        return _ahSinceFull.load(std::memory_order_relaxed);
    }

    // consumer thread: zero now (via the pending flag) and reset the integrator
    // on the next producer update (~1 BMS cycle later).
    void markFull() { _resetRequested.store(true, std::memory_order_relaxed); }
};
