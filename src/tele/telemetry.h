#pragma once

//#include "../adc/sampling.h"
//#include "../store.h"
#include <string>
#include <Arduino.h>


extern bool timeSynced;


const char *getChipId();

const std::string &getHostname(bool reload = false);

std::string getDeviceId();

void add_ap(const std::string &ssid, const std::string &psk);

void wifi_load_conf();

void connect_wifi_async();

bool wait_for_wifi();

void wifiLoop(bool connect = false);


#include "line_protocol.h"

// Telemetry point type is chosen at build time. WITH_BINARY_TELE swaps the text
// influx line for the binary symbol-table protocol (see sym_line_protocol.h);
// producers stay identical because both expose the same addTag/addField surface
// and build via makeTelePoint(). Default (unset) keeps the text path so plain
// InfluxDB UDP ingestion is unchanged.
#if WITH_BINARY_TELE
#include "sym_line_protocol.h"
extern SymbolTable g_symtab;                 // one per device (device id is fixed) -> shared table
using TelePoint = BinaryLineProtocol;
inline TelePoint makeTelePoint(const char *measurement) { return BinaryLineProtocol(g_symtab, measurement); }
#else
using TelePoint = LineProtocol;
inline TelePoint makeTelePoint(const char *measurement) { return LineProtocol(measurement); }
#endif

void telemetryAddPoint(TelePoint &p, uint16_t maxQueue = 40);

#if defined(BENCH_TELE) && WITH_BINARY_TELE
void benchTele();   // one-shot encode/compress microbench, prints via ESP_LOGW("bench", ...)
#endif

class ADC_Sampler;
struct Sensor;

void dcdcDataChanged(const ADC_Sampler &dcdc, const Sensor &sensor);

//void onTelnetConnect(String ip);
//void onTelnetDisconnect(String ip);

bool handleCommand(const String &inp);

