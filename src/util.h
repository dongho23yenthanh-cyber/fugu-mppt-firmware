#pragma once

#include <cstdint>

#define RT_CORE 1
#define NON_RT_CORE 0

using time_us = uint64_t; // monotonic microseconds, sourced from esp_timer_get_time()
using time_ms = uint64_t; // monotonic milliseconds, derived from wallClockUs()

extern time_us loopWallClockUs_;

inline const time_us &wallClockUs() { return loopWallClockUs_; }

inline time_ms wallClockMs() { return loopWallClockUs_ / 1000ULL; }


void scan_i2c();

void assertPinState(uint8_t pin, bool digitalVal, const char *pinName = nullptr, bool weakBackPull = false);

#define assert_throw(cond, msg) do { if(!(cond)) throw std::runtime_error(msg " (" #cond ") is false"); } while(0)

#define ESP_ERROR_CHECK_THROW(x) do {                                               \
        esp_err_t err_rc_ = (x);                                                    \
        if (unlikely(err_rc_ != ESP_OK)) {                                          \
            _esp_error_check_failed_without_abort(err_rc_, __FILE__, __LINE__,      \
                                    __ASSERT_FUNC, #x);                             \
            throw std::runtime_error(#x);                                           \
            }                                                                       \
    } while(0)


template<typename T>
T absdiff(const T &lhs, const T &rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}


float strntof(const char *dat, int len);