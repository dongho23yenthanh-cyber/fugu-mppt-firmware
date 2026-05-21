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

#if WITH_BINARY_TELE
#include "compress.h"
#include "../conf.h"
SymbolTable g_symtab;                    // shared interning table for the binary wire (telemetry.h)
#endif


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
    static unsigned long disconnectedSince = 0;
    static std::string prevSsid;

    if (noSsid) return false;
    if (lastTimeout and (micros() - lastTimeout < 30 * 1000 * 1000)) return false;

    if (!disconnectedSince) disconnectedSince = millis();

    // While the AP we last held may just be rebooting, keep retrying that same network (via the
    // station's stored config) instead of roaming to another configured AP. wifiMulti.run() rescans
    // and picks the strongest visible AP, so only fall back to it once switch_delay has elapsed.
    bool stick = wifiSwitchDelayMs and !prevSsid.empty()
                 and (millis() - disconnectedSince < wifiSwitchDelayMs);

    auto t_start = millis();
    if (stick) {
        ESP_LOGI("tele", "Reconnecting WiFi %s (sticky)...", prevSsid.c_str());
        WiFi.reconnect();
        while (WiFi.status() != WL_CONNECTED) {
            delay(50);
            if (millis() - t_start > 6000) {
                ESP_LOGW("tele", "WiFi reconnect timeout (%s)", prevSsid.c_str());
                lastTimeout = micros();
                return false;
            }
        }
    } else {
        ESP_LOGI("tele", "Connecting WiFi...");
        while (wifiMulti.run() != WL_CONNECTED) {
            delay(50);
            //Serial.print(".");
            if (millis() - t_start > 6000) {
                ESP_LOGW("tele", "WiFi connection timeout");
                lastTimeout = micros();
                return false;
            }
        }
    }

    disconnectedSince = 0;
    prevSsid = WiFi.SSID().c_str();
    ESP_LOGI("tele", "Connected to WiFi %s, RSSI %d IP %s", prevSsid.c_str(), (int) WiFi.RSSI(),
             WiFi.localIP().toString().c_str());

    _wifiConnected();

    return true;
}

#if !WITH_BINARY_TELE
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
#endif


#if WITH_BINARY_TELE
// Compressor for the binary wire, selected once from tele.conf (default tamp).
// Swap algorithms by changing the conf key — no code change at call sites.
static Compressor &teleCompressor() {
    static Compressor &c = compressorByName(ConfFile{"/littlefs/conf/tele.conf"}.c("compressor", "tamp"));
    return c;
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
#endif

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
#if WITH_BINARY_TELE
    Compressor &comp = teleCompressor();
    const size_t cap = comp.maxBatchRaw(CONFIG_TCP_MSS);
    static std::string batch;
    std::string frame;
    while (pointsQ.try_dequeue(frame)) {
        if (!batch.empty() && batch.size() + frame.size() > cap) {  // batch full -> send, carry frame over
            sendBinaryBatch(addr, port, comp, batch);                // one full datagram per call
            batch = std::move(frame);
            return;
        }
        batch += frame;
    }
    // batch not full -> hold for more points
#else
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
#endif
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

#if defined(BENCH_TELE) && WITH_BINARY_TELE
#include <esp_timer.h>
template<class P> static void fillBench(P &p) {       // representative mppt point (~19 fields)
    p.addTag("device", "bench-dev");
    p.addField("I", 12.34f, 3);     p.addField("Ui", 59.41f, 2);   p.addField("Uo", 26.80f, 2);
    p.addField("P", 330.2f, 2);     p.addField("P_smooth", 330.5f, 2);
    p.addField("E", 16411.5f, 1);   p.addField("E_today", 1234.5f, 1);
    p.addField("pwm_dir_f", -0.02f, 2); p.addField("mppt_state", (int) 4);
    p.addField("mcu_temp", 53.0f, 1);   p.addField("ntc_temp", 47.9f, 1);
    p.addField("pwm_duty", (int) 957);  p.addField("pwm_ls_duty", (int) 1090); p.addField("pwm_ls_max", (int) 1090);
    p.addField("pwm_dcm", false);
    p.addField("P_filt", 330.7f, 2); p.addField("P_prev", 331.0f, 2);
    p.addField("dP", -0.11f, 2);     p.addField("dP_thres", 0.0f, 2);
    p.addField("cv_lim_idx", (int) 0);
    p.setTimeMs();
}

void benchTele() {
    constexpr int N = 2000;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) { LineProtocol p("mppt"); fillBench(p); volatile auto s = p.takeWire().size(); (void) s; }
    int64_t t1 = esp_timer_get_time();
    { BinaryLineProtocol w(g_symtab, "mppt"); fillBench(w); (void) w.takeWire(); }  // warm the symbol table
    int64_t t2 = esp_timer_get_time();
    for (int i = 0; i < N; i++) { BinaryLineProtocol p(g_symtab, "mppt"); fillBench(p); volatile auto s = p.takeWire().size(); (void) s; }
    int64_t t3 = esp_timer_get_time();

    std::string batch;
    for (int i = 0; i < 40; i++) { BinaryLineProtocol p(g_symtab, "mppt"); fillBench(p); batch += p.takeWire(); }
    Compressor &comp = compressorByName("tamp");
    std::string out; constexpr int M = 200;
    int64_t t4 = esp_timer_get_time();
    for (int i = 0; i < M; i++) comp.packet((const uint8_t *) batch.data(), batch.size(), out);
    int64_t t5 = esp_timer_get_time();

    double textUs = double(t1 - t0) / N, binUs = double(t3 - t2) / N, tampUs = double(t5 - t4) / M;
    ESP_LOGW("bench", "tele encode: text %.2f us/pt | binary %.2f us/pt (%.2fx faster) | "
                      "tamp 40-pt batch %u->%u B in %.1f us (%.2f us/pt)",
             textUs, binUs, textUs / binUs, (unsigned) batch.size(), (unsigned) out.size(), tampUs, tampUs / 40.0);
}
#endif

