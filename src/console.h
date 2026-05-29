#pragma once

#include <esp_log.h>          // ESP_LOGW (LOG_VALUE_* helpers below)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>   // QueueHandle_t

void uartInit(int port_num);

void consoleInit();

int console_write_usb(const char *buf, unsigned int len);

void loopUart(unsigned long nowMs);

// Generic line-based console driver: reads from read(), echoes via write(), and dispatches
// completed lines to handleCommand(). Reused by every transport (UART, USB, BLE).
void loopConsole(int read(char *buf, size_t len), int write(const char *buf, size_t len), unsigned long nowMs);

// Installed by a transport whose output is buffered and drained asynchronously from its own loop
// (BLE), so a long-running blocking command (e.g. an OTA download) can push pending output mid-run
// instead of stalling until it returns. Null for synchronous transports (UART/USB). consoleFlush()
// is the no-arg entry point such commands call.
extern void (*consoleFlushHook)();
void consoleFlush();

extern unsigned long lastTimeOutUs;



static constexpr int UART_BUF_SIZE = 1024;
extern QueueHandle_t uart_queue;



inline void LOG_VALUE_IGNORED(const char * tag, const char *name, size_t len, const char *str) {
    ESP_LOGW(tag, "%s ignored, invalid value '%.*s'",  name, len, str);
}

inline void LOG_VALUE_NOT_FINITE(const char * tag, const char *name, const char *group) {
    ESP_LOGW(tag, "%s in %s is not finite",  name, group);
}

