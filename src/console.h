#pragma once

#include <Arduino.h>          // Serial (used by consoleInit below)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>   // QueueHandle_t

void uartInit(int port_num);

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



inline void consoleInit() {
    Serial.begin(115200);
    //ESP_ERROR_CHECK(esp_usb_console_init()); // using JTAG

#ifndef CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    //usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
    //        .tx_buffer_size = 1024,
    //        .rx_buffer_size = 1024,
    //};
    //ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_config));
#endif

#if CONFIG_IDF_TARGET_ESP32S3 and !CONFIG_ESP_CONSOLE_UART_DEFAULT
    // for unknown reason need to initialize uart0 for serial reading (see loop below)
    // Serial.available() works under Arduino IDE (for both ESP32,ESP32S3), but always returns 0 under platformio
    // so we access the uart port directly. on ESP32 the Serial.begin() is sufficient (since it uses the uart0)
    // see esp-idf vfs_console.c
    uartInit(0);
#endif

}

inline void LOG_VALUE_IGNORED(const char * tag, const char *name, size_t len, const char *str) {
    ESP_LOGW(tag, "%s ignored, invalid value '%.*s'",  name, len, str);
}

inline void LOG_VALUE_NOT_FINITE(const char * tag, const char *name, const char *group) {
    ESP_LOGW(tag, "%s in %s is not finite",  name, group);
}

