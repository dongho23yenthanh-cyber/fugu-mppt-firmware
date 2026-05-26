#pragma once
// Compile-time guard: keep RT_CORE exclusively for the high-priority RT loop (ADC/MPPT/PWM).
// Any subsystem task pinned to RT_CORE would steal cycles from that hot path → loop lag.
// Errors fire if a task is mis-placed; warnings flag suspicious configs that aren't fatal.
//
// IDF 5.5 renamed several Kconfig symbols: some are now per-CPU booleans
// (CONFIG_X_AFFINITY_CPU0/1) instead of integer cores. The RT_CORE_HAS_* aliases below
// map the per-CPU form back to a single "is on RT_CORE" bit for uniform checking.

#include <sdkconfig.h>

#ifndef RT_CORE
#error "RT_CORE must be defined before including rt_core_check.h"
#endif

#if RT_CORE != 0 && RT_CORE != 1
#error "RT_CORE must be 0 or 1"
#endif

// --- integer-style: CONFIG_X is the CPU number (0, 1, or 0x7FFFFFFF for NO_AFFINITY) ---

#if CONFIG_ARDUINO_RUNNING_CORE == RT_CORE \
 or CONFIG_ARDUINO_EVENT_RUNNING_CORE == RT_CORE \
 or CONFIG_ARDUINO_UDP_RUNNING_CORE == RT_CORE \
 or CONFIG_ARDUINO_SERIAL_EVENT_TASK_RUNNING_CORE == RT_CORE
#error "Arduino runtime is pinned to RT_CORE"
#endif

#if CONFIG_ESP_MAIN_TASK_AFFINITY == RT_CORE
#error "ESP main task is pinned to RT_CORE"
#endif

#if CONFIG_ESP_TIMER_TASK_AFFINITY == RT_CORE
#error "esp_timer task is pinned to RT_CORE"
#endif

#if CONFIG_LWIP_TCPIP_TASK_AFFINITY == RT_CORE
#error "LWIP TCPIP task is pinned to RT_CORE"
#endif

#if CONFIG_MDNS_TASK_AFFINITY == RT_CORE
#error "mDNS task is pinned to RT_CORE"
#endif

#if CONFIG_PTHREAD_TASK_CORE_DEFAULT == RT_CORE
#error "Default pthread core is RT_CORE"
#endif

#if defined(CONFIG_BT_NIMBLE_PINNED_TO_CORE) && CONFIG_BT_NIMBLE_PINNED_TO_CORE == RT_CORE
#error "NimBLE host task is pinned to RT_CORE"
#endif

// --- per-CPU boolean style (IDF 5.5+) — alias to a single RT_CORE bit ---

#if RT_CORE == 0
#define RT_CORE_HAS_ESP_TIMER_ISR  CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0
#define RT_CORE_HAS_WIFI_TASK      CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0
#define RT_CORE_HAS_MQTT_TASK      CONFIG_MQTT_USE_CORE_0
#else
#define RT_CORE_HAS_ESP_TIMER_ISR  CONFIG_ESP_TIMER_ISR_AFFINITY_CPU1
#define RT_CORE_HAS_WIFI_TASK      CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1
#define RT_CORE_HAS_MQTT_TASK      CONFIG_MQTT_USE_CORE_1
#endif

#if RT_CORE_HAS_WIFI_TASK
#error "Wi-Fi task is pinned to RT_CORE"
#endif

#if RT_CORE_HAS_MQTT_TASK
#error "MQTT task is pinned to RT_CORE"
#endif

// esp_timer ISR placement. Inverted vs the old check: nothing in this firmware uses
// ESP_TIMER_ISR dispatch, so the ISR on RT_CORE is pure preemption overhead. Want it on
// the non-RT core. Flip back to a warning-if-not-on-RT_CORE if you wire a high-rate
// ESP_TIMER_ISR safety callback that needs RT-core latency.
#if RT_CORE_HAS_ESP_TIMER_ISR
#error "esp_timer ISR is on RT_CORE (preempts the RT loop; set CONFIG_ESP_TIMER_ISR_AFFINITY_CPU<non-RT> instead)"
#endif

// FreeRTOS tick fires every 1ms on each core (not configurable away). FreeRTOS timer
// service task is NO_AFFINITY by default — fine, but worth surfacing if it gets pinned.
#if defined(CONFIG_FREERTOS_TIMER_SERVICE_TASK_CORE_AFFINITY) && \
    CONFIG_FREERTOS_TIMER_SERVICE_TASK_CORE_AFFINITY == RT_CORE
#warning "FreeRTOS timer service task is pinned to RT_CORE"
#endif
