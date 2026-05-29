#pragma once

class String; // Arduino String (handleCommand); forward-declared to avoid pulling <Arduino.h>

// Console command layer. setupCli() builds the SimpleCLI command table once at boot;
// handleCommand() parses one whole input line (from UART/USB/telnet/BLE/MQTT — same string
// protocol) through it and returns false on a parse error or handler-rejected input, true
// otherwise. See doc/Console.md.
void setupCli();
bool handleCommand(const String &inp);
