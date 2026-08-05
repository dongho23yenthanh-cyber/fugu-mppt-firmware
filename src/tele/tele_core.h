#pragma once

// Internal header for the WiFi-free telemetry core (tele_core.cpp) and its
// UDP consumer (telemetry.cpp). Producers use the public telemetry.h API.

#include <string>
#include "../etc/readerwriterqueue.h"

// SPSC: producer = net-loop (telemetryAddPoint), consumer = UDP flushTask only.
extern moodycamel::ReaderWriterQueue<std::string> pointsQ;

// True while the UDP flush task runs (TelemetryService onStart/onStop); without a
// consumer, points are not queued.
extern bool g_teleUdpActive;
