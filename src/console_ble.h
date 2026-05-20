#pragma once

#include <string>
#include <cstdint>

// BLE serial console over the Nordic UART Service (NUS). A new transport for handleCommand(),
// reachable without Wi-Fi. ESP32-S3 supports BLE only (no classic SPP). All functions are
// no-ops unless the firmware is built with -DWITH_BLE (see main/CMakeLists.txt).
//
// security: "none" | "justworks" | "passkey".
//   none      - open, no pairing (any client may issue commands).
//   justworks - encrypted link, no passkey (reliable with Web Bluetooth).
//   passkey   - MITM-authenticated pairing using `passkey` (000000..999999); for native clients.

void bleConsoleBegin(const std::string &deviceName, const std::string &security, uint32_t passkey);

void bleConsoleEnd();

void bleConsoleLoop(unsigned long nowMs); // drives the RX queue through loopConsole(); call on core 0

bool bleConsoleConnected();
