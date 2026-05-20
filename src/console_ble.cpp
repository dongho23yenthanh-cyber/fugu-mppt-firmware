#include "console_ble.h"

#ifdef WITH_BLE

#include <algorithm>
#include <mutex>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <os/os_mbuf.h> // os_msys_num_free(): peek NimBLE's free mbuf pool before notifying

#include "console.h"   // loopConsole
#include "logging.h"   // addLogCallback / removeLogCallback, ESP_LOG*
#include "etc/readerwriterqueue.h"
#include "etc/ota_ble.h" // OTA-over-BLE firmware push (FW characteristic data sink)

// Nordic UART Service. RX = client->device (write commands), TX = device->client (notify output).
// FW = client->device write-no-response firmware bytes (OTA push, bypasses the console line parser).
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_CHAR_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_FW_CHAR_UUID "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"

static const char *TAG = "ble";

static BLEServer *bleServer = nullptr;
static BLECharacteristic *txChar = nullptr;
static volatile bool deviceConnected = false;
// The Arduino-ESP32 BLE wrapper is not reinit-safe: BLEDevice::deinit() frees the controller but
// leaves the static BLEServer and its service map intact, so a second init+createService() hands
// back a stale BLEService over torn-down NimBLE handles and crashes. So we init the whole stack
// exactly once and, on stop/start, only toggle advertising. `bleStarted` tracks the advertising
// (logical service running) state; `bleInited` tracks the one-time stack bring-up.
static bool bleInited = false;
static bool bleStarted = false;

// Single-producer (NimBLE host task, onWrite) / single-consumer (network loop, bleRead) byte queue.
static moodycamel::ReaderWriterQueue<char> rxQueue{256};

// --- console read/write hooks fed to loopConsole() ---

static int bleRead(char *buf, size_t len) {
    size_t n = 0;
    char c;
    while (n < len && rxQueue.try_dequeue(c)) buf[n++] = c;
    return (int) n;
}

// Serializes access to the TX FIFO and notify() across tasks: console echo/responses run on the
// network loop while the log mirror (bleLogWrite) can fire from other core-0 tasks. Recursive so the
// rc=6 error log emitted inside notify() on the same task can't self-deadlock (it re-enters bleWrite).
static std::recursive_mutex txMutex;

// NimBLE can only emit roughly one notification per connection interval and has a small mbuf pool, so
// notifying a multi-line command response (e.g. `get-config`) in a tight loop exhausts it: then
// ble_gatts_notify_custom returns BLE_HS_ENOMEM (rc=6) and the packet is silently dropped, truncating
// the client's output. So bleWrite only appends to this FIFO; bleTxDrain() (pumped from the network
// loop) sends 20-byte chunks with backpressure — it stops the moment a notify fails and leaves the
// rest queued for the next tick, by when the pool has refilled.
static std::string txBuf;
static size_t txHead = 0;                    // consumed-prefix offset, amortizes pop-front
static constexpr size_t TX_BUF_CAP = 8192;   // bound the unsent backlog if a client stalls
static bool lastNotifyOk = true;             // set by TxCallbacks::onStatus, read right after notify()

// Right after connect the central is still on its default (slow) connection interval and the param
// update we request hasn't applied yet, so pushing the queued log backlog into the link overflows the
// controller's ACL buffers (the rc=6 burst). Hold the drain for a short settle window; the backlog
// then flushes cleanly. Re-armed on every connect.
// The settle window also defers our connection-parameter request: issuing a device-initiated LL
// connection update synchronously from the host connect callback — while justworks pairing/feature
// exchange is still in flight and Wi-Fi coex is active — races the peer's procedures and trips the
// controller assert lld_con.c:3275 (r_lld_con_param_update). We instead request it once from the
// network-loop drain after the window, off the host callback. Backpressure tolerates the brief
// slow-interval drain before the faster interval applies.
static volatile bool txArmSettle = false;
static volatile bool connParamsPending = false;
static unsigned long txConnectMs = 0;
static constexpr unsigned long TX_SETTLE_MS = 500;

static int bleWrite(const char *buf, size_t len) {
    if (!deviceConnected || !txChar) return 0;
    std::lock_guard<std::recursive_mutex> lk(txMutex);
    if (txHead == txBuf.size()) { txBuf.clear(); txHead = 0; } // fully drained: reclaim
    size_t backlog = txBuf.size() - txHead;
    if (backlog + len > TX_BUF_CAP) len = backlog < TX_BUF_CAP ? TX_BUF_CAP - backlog : 0; // drop overflow
    txBuf.append(buf, len);
    return (int) len;
}

// Push queued output to the client, paced by NimBLE's buffer availability. Pump from the network loop.
static void bleTxDrain(unsigned long nowMs) {
    if (!deviceConnected || !txChar || !bleServer) return;
    std::lock_guard<std::recursive_mutex> lk(txMutex);
    if (txArmSettle) { txConnectMs = nowMs; txArmSettle = false; }
    if (nowMs - txConnectMs < TX_SETTLE_MS) return; // let pairing settle before touching the link
    if (connParamsPending) {
        // Deferred off the host connect callback — see the txArmSettle note for why.
        // 6..12 = 7.5..15ms interval, latency 0, supervision timeout 400 = 4s.
        connParamsPending = false;
        bleServer->requestConnParams(bleServer->getConnId(), 6, 12, 0, 400);
    }

    // Chunk at the negotiated ATT MTU minus the 3-byte notify header (falls back to the 20-byte
    // default before MTU exchange). A larger MTU means a multi-line reply is a few notifications
    // instead of dozens of 20-byte ones — the dominant console-latency win alongside a fast interval.
    uint16_t mtu = bleServer->getPeerMTU(bleServer->getConnId());
    size_t chunk = mtu > 23 ? (size_t) (mtu - 3) : 20;
    while (txHead < txBuf.size()) {
        // Peek NimBLE's free mbuf count first. Right after connect the connection interval is still
        // slow and the pool drains faster than it refills; calling notify() into an empty pool returns
        // BLE_HS_ENOMEM (rc=6) and logs an [E] from the BLE wrapper. So skip the call entirely when the
        // pool is low and retry next tick — no failed call, no error spam, no dropped bytes. (4 mirrors
        // the NimBLE btshell throughput guard: one notify needs a couple of mbufs across ATT/L2CAP.)
        if (os_msys_num_free() < 4) break;
        size_t n = std::min(chunk, txBuf.size() - txHead);
        txChar->setValue((uint8_t *) (txBuf.data() + txHead), n);
        lastNotifyOk = true;     // TxCallbacks::onStatus flips this to false on ENOMEM/error (backstop)
        txChar->notify();
        if (!lastNotifyOk) break; // pool exhausted anyway — retry this chunk on the next tick
        txHead += n;
    }
    if (txHead == txBuf.size()) { txBuf.clear(); txHead = 0; }
}

// Mirror logs to the connected client (registered as a log sink on connect, like telnet). Just queues;
// the error log a failed notify() emits re-enters here and is appended (bounded by TX_BUF_CAP), never
// re-notified, so there is no feedback storm — NimBLE's own INFO notify log is silenced to WARN below.
static void bleLogWrite(const char *str, uint16_t len) {
    bleWrite(str, len);
}

// --- BLE callbacks ---

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        String v = c->getValue();
        for (size_t i = 0; i < v.length(); ++i) rxQueue.enqueue(v[i]);
    }
};

// Firmware-data sink for OTA push. Runs on the NimBLE host task: only copies bytes into the OTA ring
// (otaBleStageBytes never touches flash); the network-loop tick drains them to the partition.
class FwRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        String v = c->getValue();
        otaBleStageBytes((const uint8_t *) v.c_str(), v.length());
    }
};

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *s) override {
        deviceConnected = true;
        addLogCallback(bleLogWrite);
        // Defer the fast-interval connection-param request to the network-loop drain (see TX_SETTLE_MS
        // note): requesting it here, in the host connect callback, trips controller assert lld_con.c:3275.
        connParamsPending = true;
        txArmSettle = true; // hold the TX drain until the link speeds up (see TX_SETTLE_MS)
        ESP_LOGI(TAG, "client connected");
    }

    void onDisconnect(BLEServer *s) override {
        deviceConnected = false;
        if (otaBleActive()) otaBleRequestAbort(); // net-loop tick aborts; never free OTA state on the host task
        removeLogCallback(bleLogWrite);
        { std::lock_guard<std::recursive_mutex> lk(txMutex); txBuf.clear(); txHead = 0; }
        ESP_LOGI(TAG, "client disconnected, re-advertising");
        s->startAdvertising();
    }
};

// Backpressure signal for bleTxDrain(): notify() invokes this synchronously on the same task with
// SUCCESS_NOTIFY on success or ERROR_GATT (rc=6 BLE_HS_ENOMEM) when the mbuf pool is exhausted.
class TxCallbacks : public BLECharacteristicCallbacks {
    void onStatus(BLECharacteristic *, Status s, uint32_t) override {
        if (s != SUCCESS_NOTIFY) lastNotifyOk = false;
    }
};

static RxCallbacks rxCallbacks;
static FwRxCallbacks fwRxCallbacks;
static ServerCallbacks serverCallbacks;
static TxCallbacks txCallbacks;

void bleConsoleBegin(const std::string &deviceName, const std::string &security, uint32_t passkey) {
    if (bleStarted) return; // already advertising

    if (bleInited) {
        // Stack is already up from a previous start; just resume advertising (see bleInited note).
        BLEDevice::startAdvertising();
        bleStarted = true;
        ESP_LOGI(TAG, "re-advertising as '%s' (NUS console)", deviceName.c_str());
        return;
    }

    BLEDevice::init(deviceName.c_str()); // const char* -> Arduino String
    BLEDevice::setMTU(247); // prefer a large ATT MTU so the client negotiates up from the 23-byte default

    // NimBLE logs "GAP procedure initiated: notify;" at INFO on *every* notification. Because we
    // mirror logs to the BLE link, that feeds back into more notifications — a self-sustaining storm
    // that corrupts the console. Keep the NimBLE tag at WARN.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Per-characteristic encryption requirement enforces pairing before commands are accepted
    // (NimBLE backend; under Bluedroid the *_ENC/_AUTHEN flags are 0 and degrade to open).
    uint32_t writeProps = BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR;
    auto *sec = new BLESecurity();
    if (security == "passkey") {
        sec->setPassKey(true, passkey);
        sec->setCapability(ESP_IO_CAP_OUT);            // device displays the passkey
        sec->setAuthenticationMode(true, true, true);  // bond + MITM + secure-connections
        writeProps |= BLECharacteristic::PROPERTY_WRITE_AUTHEN;
        ESP_LOGI(TAG, "security: passkey (bond+MITM)");
    } else if (security == "justworks") {
        sec->setCapability(ESP_IO_CAP_NONE);
        sec->setAuthenticationMode(true, false, true); // bond + no MITM + secure-connections
        writeProps |= BLECharacteristic::PROPERTY_WRITE_ENC;
        ESP_LOGI(TAG, "security: justworks (encrypted, no passkey)");
    } else {
        ESP_LOGW(TAG, "security: none (open console)");
    }

    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(&serverCallbacks);

    BLEService *svc = bleServer->createService(NUS_SERVICE_UUID);

    txChar = svc->createCharacteristic(NUS_TX_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    txChar->setCallbacks(&txCallbacks); // onStatus drives bleTxDrain() backpressure
#if defined(CONFIG_BLUEDROID_ENABLED)
    txChar->addDescriptor(new BLE2902()); // NimBLE adds the CCCD automatically from PROPERTY_NOTIFY
#endif

    BLECharacteristic *rxChar = svc->createCharacteristic(NUS_RX_CHAR_UUID, writeProps);
    rxChar->setCallbacks(&rxCallbacks);

    // OTA firmware push: write-no-response only (high-throughput), same pairing requirement as RX.
    uint32_t fwProps = (writeProps & ~(uint32_t) BLECharacteristic::PROPERTY_WRITE)
                       | BLECharacteristic::PROPERTY_WRITE_NR;
    BLECharacteristic *fwChar = svc->createCharacteristic(NUS_FW_CHAR_UUID, fwProps);
    fwChar->setCallbacks(&fwRxCallbacks);

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();

    bleInited = true;
    bleStarted = true;
    ESP_LOGI(TAG, "advertising as '%s' (NUS console)", deviceName.c_str());
}

void bleConsoleEnd() {
    if (!bleStarted) return;
    // Stop advertising only — do NOT BLEDevice::deinit(): the Arduino BLE wrapper can't be
    // re-initialized cleanly (see bleInited note), so the stack stays up across stop/start.
    removeLogCallback(bleLogWrite);
    BLEDevice::stopAdvertising();
    if (deviceConnected && bleServer) bleServer->disconnect(bleServer->getConnId());
    deviceConnected = false;
    bleStarted = false;
    char c;
    while (rxQueue.try_dequeue(c)) {} // drop stale input
}

void bleConsoleLoop(unsigned long nowMs) {
    if (!bleStarted) return;
    loopConsole(bleRead, bleWrite, nowMs);
    bleTxDrain(nowMs); // flush queued console/log output, paced by NimBLE buffer availability
    otaBleTick(nowMs); // drain any staged OTA firmware bytes to flash (no-op when not updating)
}

bool bleConsoleConnected() { return deviceConnected; }

#else // !WITH_BLE — no-op stubs so callers (and the service wrapper) link without the BLE stack

void bleConsoleBegin(const std::string &, const std::string &, uint32_t) {}

void bleConsoleEnd() {}

void bleConsoleLoop(unsigned long) {}

bool bleConsoleConnected() { return false; }

#endif
