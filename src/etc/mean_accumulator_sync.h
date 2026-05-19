#pragma once

#include "freertos/FreeRTOS.h" // portMUX_TYPE

#include <cassert>
#include <cstdint>
#include <limits>

// Thread-safe SPSC variant of MeanAccumulator (math/statmath.h). Uses
// portMUX_TYPE: SMP-safe spinlock that also disables interrupts on the
// current core, so the critical section is safe against same-core ISRs.
// Don't call from an ISR (use the *_ISR portmux macros if that ever becomes
// necessary). Pick this variant only when add() and tryPop() may run on
// different threads (e.g. MQTT callback producer, RT-loop consumer).
struct MeanAccumulatorSync {
private:
    float sum = 0.f;
    uint16_t num = 0;
    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

public:
    void add(float x) {
        portENTER_CRITICAL(&mux);
        assert(num < std::numeric_limits<decltype(num)>::max());
        sum += x;
        ++num;
        portEXIT_CRITICAL(&mux);
    }

    void clear() {
        portENTER_CRITICAL(&mux);
        sum = 0.f;
        num = 0;
        portEXIT_CRITICAL(&mux);
    }

    // Atomic check-and-pop: if at least `minSamples` are accumulated, write
    // the mean to `out`, reset state, and return true. Otherwise leave state
    // unchanged and return false. Folding the gate into the pop avoids the
    // check/pop race the plain MeanAccumulator has across threads.
    bool tryPop(uint16_t minSamples, float &out) {
        portENTER_CRITICAL(&mux);
        bool ready = num >= minSamples;
        if (ready) {
            out = sum / num;
            sum = 0.f;
            num = 0;
        }
        portEXIT_CRITICAL(&mux);
        return ready;
    }
};
