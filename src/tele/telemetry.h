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

void telemetryAddPoint(LineProtocol &p, uint16_t maxQueue = 40);

void telemetryFlushPointsQ(const IPAddress &addr);


class ADC_Sampler;
struct Sensor;

void dcdcDataChanged(const ADC_Sampler &dcdc, const Sensor &sensor);

//void onTelnetConnect(String ip);
//void onTelnetDisconnect(String ip);

bool handleCommand(const String &inp);

