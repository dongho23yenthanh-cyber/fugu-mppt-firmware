#include "telemetry.h"

#include "../adc/sampling.h"
#include "line_protocol.h"
#include "../web/server.h"
#include "../store.h"

#include <WiFiMulti.h>
//#include <WiFiUdp.h>
#include <ESPmDNS.h>

#include <AsyncUDP.h>

//#include <Preferences.h>

#include <Arduino.h>

#include "../etc/readerwriterqueue.h"

#include "../storage/key-value.h"

WiFiMulti wifiMulti;
//WiFiUDP udp;
AsyncUDP asyncUdp;

bool noSsid = true;
bool timeSynced = false;

IPAddress ha_host{};

extern KeyValueStorage nvs;

void wifi_load_conf() {
    ConfFile wifiConf{"/littlefs/conf/wifi.conf"};

    auto starts_with = [](const std::string &s, const std::string &t) { return s.substr(0, t.length()) == t; };
    auto ends_with = [](const std::string &s, const std::string &t) { return s.substr(s.length() - t.length()) == t; };

    noSsid = true;

    nvs.open();
    auto ssid_def = nvs.readString("wifi_ssid", "");
    auto psk_def = nvs.readString("wifi_psk", "");
    nvs.close();
    if (!ssid_def.empty()) {
        wifiMulti.addAP(ssid_def.c_str(), psk_def.c_str());
        noSsid = false;
    }

    for (const auto &k: wifiConf.keys()) {
        if (starts_with(k, "ssid") && !ends_with(k, "_psk")) {
            auto ssid = wifiConf.getString(k).c_str();
            auto psk = wifiConf.c(k + "_psk", "");
            if (ssid == ssid_def and psk == psk_def) continue;
            if (!wifiMulti.addAP(wifiConf.getString(k).c_str(), psk)) {
                ESP_LOGW("tele", "Failed to add ap  %s", ssid);
            } else {
                ESP_LOGI("tele", "Add WiFi SSID %s (psk %s)", ssid, psk ? "***" : "<none>");
                noSsid = false;
            }
        }
    }
}

void add_ap(const std::string &ssid, const std::string &psk) {
    auto confPath = "/littlefs/conf/wifi.conf";
    ConfFile wifiConf{confPath, true};
    wifiConf.add({
        {"ssid_" + ssid, ssid},
        {"ssid_" + ssid + "_psk", psk.c_str()}
    });
    ESP_LOGI("tele", "Added Wifi AP %s to %s", ssid.c_str(), confPath);
    noSsid = false;
}

std::string getDeviceId() {
    return "fugu-" + std::string(getChipId());
}

const std::string &getHostname(bool reload) {
    static std::string hostname{};
    if (hostname.empty() or reload) {
        nvs.open();
        hostname = nvs.readString("hostname", getDeviceId());
        nvs.close();
    }
    return hostname;
}

void connect_wifi_async() {
    timeSynced = false;
    if (noSsid) wifi_load_conf();
    if (!noSsid) {
        NetworkManager::setHostname(getHostname().c_str());
        WiFi.mode(WIFI_STA);
    }
}


static bool timeSyncAsync(const char *tzInfo, const char *ntpServer1, const char *ntpServer2 = nullptr,
                   const char *ntpServer3 = nullptr) {
    static unsigned long tSyncStarted = 0;

    if (!tSyncStarted) {
        ESP_LOGI("tele", "Starting time sync");
        tSyncStarted = millis() + 1;
        configTzTime(tzInfo, ntpServer1, ntpServer2, ntpServer3);
    } else if (time(nullptr) > 1000000000l) {
        struct tm timeinfo;
        time_t nowSecs = time(nullptr);
        gmtime_r(&nowSecs, &timeinfo);
        ESP_LOGI("tele", "Time synced! %s UTC", asctime(&timeinfo));
        tSyncStarted = 0;
        return true;
    } else if ((millis() - tSyncStarted) > (20 * 1000)) {
        ESP_LOGW("tele", "Timeout syncing time! (%s)", ntpServer1);
        tSyncStarted = 0;
    }
    return false;
}


static void _wifiConnected() {
    if (!WiFi.isConnected()) return;

    String hostname = String(getHostname().c_str());

    MDNS.end();
    if (!MDNS.begin(hostname.c_str())) {
        // abc.local
        ESP_LOGE("tele", "Error setting up MDNS responder!");
    } else {
        ESP_LOGI("tele", "Set hostname %s", hostname.c_str());
    }

    MDNS.setInstanceName(hostname);

    ha_host = MDNS.queryHost("homeassistant.local");
    ESP_LOGI("tele", "%s resolved to %s", "homeassistant.local", ha_host.toString().c_str());

    // telnet / ftp / scope are now started by the ServiceManager (self-heals on the WiFi-up edge,
    // see loopNetwork_task). Their onStart() relies on the MDNS setup above being done first.
}

void wifiLoop(bool connect) {
    //static bool initialized = false;
    if (noSsid) return;

    if (connect && !WiFi.isConnected()) {
        wait_for_wifi();
    }

    if (unlikely(!timeSynced)) {
        if (timeSyncAsync("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nis.gov")) {
            timeSynced = true;
        }
    }

    //if (unlikely(!initialized) && WiFi.isConnected()) {
    //    initialized = true;
    //    _wifiConnected();
    // }
}

bool wait_for_wifi() {
    static unsigned long lastTimeout = 0;

    if (noSsid) return false;
    if (lastTimeout and (micros() - lastTimeout < 30 * 1000 * 1000)) return false;

    ESP_LOGI("tele", "Connecting WiFi...");
    auto t_start = millis();
    while (wifiMulti.run() != WL_CONNECTED) {
        delay(50);
        //Serial.print(".");
        if (millis() - t_start > 6000) {
            ESP_LOGW("tele", "WiFi connection timeout");
            lastTimeout = micros();
            return false;
        }
    }
    ESP_LOGI("tele", "Connected to WiFi, RSSI %d IP %s", (int) WiFi.RSSI(), WiFi.localIP().toString().c_str());

    _wifiConnected();

    return true;
}

static void udpFlushString(const IPAddress &host, uint16_t port, String &msg) {
    if (msg.length() > CONFIG_TCP_MSS) {
        ESP_LOGW("tele", "Payload len %d > TCP_MSS: %s", msg.length(), msg.substring(0, 200).c_str());
        msg.clear();
        return;
    }

    bytesSent += asyncUdp.writeTo((uint8_t *) msg.c_str(), msg.length(), host, port);

    //udp.beginPacket(host, port);
    //udp.print(msg);
    //udp.endPacket();

    msg.clear();
}


static void influxWritePointsUDP(const IPAddress &dst, moodycamel::ReaderWriterQueue<std::string> &q) {
    constexpr int MTU = CONFIG_TCP_MSS;
    // notice that MTU is not the UDP max message size (which is >64k?), here we use MTU from ip4 as a "safe" value

    if (noSsid) return;

    constexpr auto port = 8086;

    static String msg;

    std::string lp;
    while (q.try_dequeue(lp)) {
        if (msg.length() + lp.length() + 1 >= MTU) {
            udpFlushString(dst, port, msg);
        }
        msg += lp.c_str();
        msg += '\n';
    }
}

const char *getChipId() {
    static char ssid[25]{0};
    if (!strlen(ssid)) {
        // newlib-nano printf has no 64-bit support; split MAC into hi/lo 32-bit
        // words so the result stays identical to "%llX" for any 48-bit MAC.
        uint64_t mac = ESP.getEfuseMac();
        snprintf(ssid, 25, "%s-%lX%08lX", CONFIG_IDF_TARGET,
                 (unsigned long) (mac >> 32), (unsigned long) (mac & 0xFFFFFFFF));
    }
    return ssid;
}


moodycamel::ReaderWriterQueue<std::string> pointsQ{};

void telemetryAddPoint(LineProtocol &p, uint16_t maxQueue) {
    assert(p.hasTime());

    if (pointsQ.size_approx() < maxQueue)
        pointsQ.enqueue(p.takeLine());
}

void telemetryFlushPointsQ(const IPAddress &addr) {
    if (pointsQ.size_approx() > 40)
        influxWritePointsUDP(addr, pointsQ);
}

extern VIinVout<const Sensor *> sensors;

void dcdcDataChanged(const ADC_Sampler &dcdc, const Sensor &sensor) {
    if (timeSynced && sensor.params.rawTelemetry && !sensor.params.teleName.empty() && WiFi.isConnected()) {
        LineProtocol point("mppt");
        point.addTag("device", getHostname().c_str());
        point.addField(sensor.params.teleName.c_str(), sensor.last, 3);
        point.setTimeMs();
        telemetryAddPoint(point, 600);
    }
}

