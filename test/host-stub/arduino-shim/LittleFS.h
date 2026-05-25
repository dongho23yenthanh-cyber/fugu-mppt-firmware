#pragma once
#include "FS.h"

// Global stand-in for the arduino-esp32 LittleFS object. SimpleFTPServer
// references it by the unqualified name "LittleFS".
extern FS LittleFS;
