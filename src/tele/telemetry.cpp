#include "telemetry.h"
#include "telemetry_service.h"   // transport methods (flushTask/flushQueue) are defined here

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

#include "compress.h"
#include "../conf.h"
SymbolTable g_symtab;                    // shared interning table for the binary wire (telemetry.h)
bool g_teleBinary = false;               // set by TelemetryService::onStart; default = text
static Compressor *s_teleCompressor = nullptr;   // cached compressor for the binary wire


WiFiMulti wifiMulti;
//WiFiUDP udp;
AsyncUDP asyncUdp;

bool noSsid = true;
bool timeSynced = false;

// Grace period to keep retrying the AP we lost before roaming to another configured network
// (the router might just be rebooting). 0 disables. Set via wifi.conf::switch_delay (seconds).
static uint32_t wifiSwitchDelayMs = 30 * 1000;

IPAddress ha_host{};

extern KeyValueStorage nvs;

void wifi_load_conf() {
    ConfFile wifiConf{"/littlefs/conf/wifi.conf"};

    auto starts_with = [](const std::string &s, const std::string &t) { return s.substr(0, t.length()) == t; };
    auto ends_with = [](const std::string &s, const std::string &t) { return s.substr(s.length() - t.length()) == t; };

    noSsid = true;

    wifiSwitchDelayMs = (uint32_t) wifiConf.getLong("switch_delay", 30) * 1000;

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
        WiFi.setSleep(WIFI_PS_MIN_MODEM); // modem-sleep between DTIM beacons; cuts RF idle power/heat, keeps console+MQTT latency low
    }
}

void disconnect_wifi(bool off) {
    MDNS.end(); // unregister netif hooks + leave the mcast group while the netif is still valid
    WiFi.disconnect(off);
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
    static unsigned long disconnectedSince = 0;
    static std::string prevSsid;

    if (noSsid) return false;

    if (!disconnectedSince) disconnectedSince = millis();

    // While the AP we last held may just be rebooting, keep retrying that same network (via the
    // station's stored config) instead of roaming to another configured AP. wifiMulti.run() rescans
    // and picks the strongest visible AP, so only fall back to it once switch_delay has elapsed.
    bool stick = wifiSwitchDelayMs and !prevSsid.empty()
                 and (millis() - disconnectedSince < wifiSwitchDelayMs);

    // 30 s backoff after a failed attempt before rescanning/roaming - but while sticking, skip it so
    // we keep retrying the lost AP and re-acquire it the moment it finishes rebooting.
    if (lastTimeout and !stick and (micros() - lastTimeout < 30 * 1000 * 1000)) return false;

    ESP_LOGI("tele", "Connecting WiFi...");
    auto t_start = millis();
    if (stick) WiFi.reconnect();
    while ((stick ? WiFi.status() : static_cast<wl_status_t>(wifiMulti.run())) != WL_CONNECTED) {
        delay(50);
        if (millis() - t_start > 6000) {
            ESP_LOGW("tele", "WiFi connection timeout");
            if (!stick) lastTimeout = micros();
            return false;
        }
    }

    disconnectedSince = 0;
    prevSsid = WiFi.SSID().c_str();
    ESP_LOGI("tele", "Connected to WiFi %s, RSSI %d IP %s", prevSsid.c_str(), (int) WiFi.RSSI(),
             WiFi.localIP().toString().c_str());

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

// Load tele.conf::binary at service start; cache so the hot path doesn't read
// littlefs per point. The binary wire is always tamp-compressed; on text the
// compressor stays null.
void teleLoadWireConf() {
    ConfFile conf{"/littlefs/conf/tele.conf"};
    g_teleBinary = conf.getLong("binary", 0) != 0;
    s_teleCompressor = g_teleBinary ? &compressorByName("tamp") : nullptr;
}

// Compress one batch of concatenated (already length-prefixed) wire frames, tag
// it with the compressor id, and send as one self-contained UDP datagram.
static void sendBinaryBatch(const IPAddress &dst, uint16_t port, Compressor &comp, std::string &raw) {
    if (raw.empty()) return;
    static std::string out;
    std::string body;
    if (comp.packet((const uint8_t *) raw.data(), raw.size(), body)) {
        out.assign(1, (char) comp.id());     // 1-byte tag so the receiver picks the decompressor
        out += body;
        if (out.size() > CONFIG_TCP_MSS)
            ESP_LOGW("tele", "binary datagram %u B > MSS (raw %u)", (unsigned) out.size(), (unsigned) raw.size());
        bytesSent += asyncUdp.writeTo((const uint8_t *) out.data(), out.size(), dst, port);
    }
    raw.clear();
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

void telemetryAddPoint(TelePoint &p, uint16_t maxQueue) {
    assert(p.hasTime());

    if (pointsQ.size_approx() < maxQueue)
        pointsQ.enqueue(p.takeWire());
}

// TelemetryService transport lives here (where the queue/UDP/compressor statics are file-local);
// the header stays free of AsyncUDP/compress includes. Producers stay decoupled: they only call
// the free telemetryAddPoint() above.

// Drains/compresses/sends the point queue on its own core-0 task, so compression (tens of ms with
// tamp) never stalls production. SPSC: producer = onTick thread, consumer = this task.
void TelemetryService::flushTask(void *arg) {
    auto *self = static_cast<TelemetryService *>(arg);
    for (;;) {
        self->flushQueue(mppt.tele.influxdbHost);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Sends AT MOST ONE datagram per call, and ONLY when the batch is full (~MSS). flushTask calls
// this at a fixed cadence, so full datagrams leave one-at-a-time rather than bursting — back-to-
// back UDP sends drop more, especially across NAT. A partially-filled batch waits for more points
// (latency is acceptable); no small datagrams go on the wire.
void TelemetryService::flushQueue(const IPAddress &addr) {
    if (noSsid) return;
    constexpr auto port = 8086;
    if (g_teleBinary && s_teleCompressor) {
        Compressor &comp = *s_teleCompressor;
        const size_t cap = comp.maxBatchRaw(CONFIG_TCP_MSS);
        static std::string batch;
        std::string frame;
        try {
            while (pointsQ.try_dequeue(frame)) {
                if (!batch.empty() && batch.size() + frame.size() > cap) {  // batch full -> send, carry frame over
                    sendBinaryBatch(addr, port, comp, batch);                // one full datagram per call
                    batch = std::move(frame);
                    return;
                }
                batch += frame;
            }
            // batch not full -> hold for more points
        } catch (const std::bad_alloc &) {
            // Heap exhausted (fragmentation during a WiFi flap). Telemetry is non-critical: drop the
            // batch and free its buffer rather than let the throw escape the task and panic the device.
            std::string().swap(batch);
            ESP_LOGW("tele", "flushQueue OOM, dropped batch");
        }
    } else {
        constexpr size_t MTU = CONFIG_TCP_MSS;
        static String msg;
        std::string lp;
        while (pointsQ.try_dequeue(lp)) {
            if (msg.length() && msg.length() + lp.length() + 1 > MTU) {  // full -> send, carry line over
                udpFlushString(addr, port, msg);
                msg = String(lp.c_str()); msg += '\n';
                return;
            }
            msg += lp.c_str(); msg += '\n';
        }
    }
}


extern VIinVout<const Sensor *> sensors;

void dcdcDataChanged(const ADC_Sampler &dcdc, const Sensor &sensor) {
    if (timeSynced && sensor.params.rawTelemetry && !sensor.params.teleName.empty() && WiFi.isConnected()) {
        auto point = makeTelePoint("mppt");
        point.addTag("device", getHostname().c_str());
        point.addField(sensor.params.teleName.c_str(), sensor.last, 3);
        point.setTimeMs();
        telemetryAddPoint(point, 600);
    }
}


