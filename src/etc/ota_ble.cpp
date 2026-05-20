#include "ota_ble.h"

#ifdef WITH_BLE

#include <algorithm>
#include <cstring>
#include <mutex>

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include "util.h"          // wallClockMs
#include "adc/sampling.h"  // ADC_Sampler (halt during flash)

static const char *TAG = "otab";

// Globals owned by main.cpp (see cli.cpp): halt the converter/ADC while flashing, restore on end/abort.
extern ADC_Sampler adcSampler;
void stopAndBackoff(uint32_t secondsDelay);
void systemRestart();

// Staging ring: the FW characteristic onWrite (NimBLE host task) only copies bytes in here; the
// network-loop tick drains to flash. Decoupling keeps the slow esp_ota_write off the host task (a
// stall there trips the BLE supervision timeout). Capacity doubles as the host's credit window.
// Kept small (8 KB): on a no-PSRAM board internal heap is tight/fragmented, and BLE throughput
// (~tens of KB/s) is far below what this window sustains, so the credit round-trip never bottlenecks.
static constexpr size_t RING_CAP = 8 * 1024;
static constexpr size_t FLUSH_SLICE = 2048;       // flash-page-friendly esp_ota_write granularity
static constexpr size_t CRED_STEP = RING_CAP / 2; // re-grant credit in half-window steps (limits notifies)

static uint8_t *ring = nullptr;
static size_t rHead = 0, rTail = 0, rCount = 0;   // byte ring indices + fill level
static std::mutex ringMutex;                      // guards ring + rCount across host task / net loop

static esp_ota_handle_t otaHandle = 0;
static const esp_partition_t *otaPart = nullptr;
static mbedtls_sha256_context shaCtx;
static uint8_t expectedSha[32];

static bool active = false;
static bool failed = false;
static volatile bool abortReq = false; // set from any task; consumed by the net-loop tick
static uint32_t expectedSize = 0;
static uint32_t written = 0;    // flushed to flash (consumer)
static uint32_t lastGranted = 0;
static uint32_t lastProg = 0;

static int parseHex32(const char *hex, uint8_t out[32]) {
    auto nib = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; ++i) {
        int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return 0;
}

static void freeRing() {
    if (ring) { heap_caps_free(ring); ring = nullptr; }
    rHead = rTail = rCount = 0;
}

static void grantCredit() {
    // High-water mark: the host may stream up to (written + RING_CAP) cumulative bytes. Advance it as
    // flash drains; re-announce only in CRED_STEP jumps to avoid notify spam.
    uint32_t g = written + RING_CAP;
    if (g > expectedSize) g = expectedSize;
    if (g >= lastGranted + CRED_STEP || g == expectedSize) {
        lastGranted = g;
        ESP_LOGI(TAG, "OTAB CRED %u", (unsigned) g);
    }
}

bool otaBleActive() { return active; }

bool otaBleBegin(uint32_t size, const char *sha256hex) {
    if (active) { ESP_LOGW(TAG, "OTAB FAIL already-active"); return false; }
    if (!sha256hex || parseHex32(sha256hex, expectedSha) != 0) {
        ESP_LOGW(TAG, "OTAB FAIL bad-sha"); return false;
    }
    otaPart = esp_ota_get_next_update_partition(nullptr);
    if (!otaPart) { ESP_LOGW(TAG, "OTAB FAIL no-partition"); return false; }
    if (size == 0 || size > otaPart->size) {
        ESP_LOGW(TAG, "OTAB FAIL size %u > part %u", (unsigned) size, (unsigned) otaPart->size);
        return false;
    }
    ring = (uint8_t *) heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM);
    if (!ring) ring = (uint8_t *) heap_caps_malloc(RING_CAP, MALLOC_CAP_DEFAULT); // PSRAM-less fallback
    if (!ring) { ESP_LOGW(TAG, "OTAB FAIL no-mem"); return false; }

    stopAndBackoff(10);
    adcSampler.halted = true; // free the CPU/flash for the erase + writes; converter already disabled

    esp_err_t err = esp_ota_begin(otaPart, size, &otaHandle); // erases the whole target partition
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTAB FAIL esp_ota_begin %s", esp_err_to_name(err));
        freeRing();
        adcSampler.halted = false;
        return false;
    }
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0); // 0 = SHA-256
    rHead = rTail = rCount = 0;
    expectedSize = size;
    written = lastGranted = lastProg = 0;
    failed = false;
    active = true;
    ESP_LOGI(TAG, "OTAB READY part=%s size=%u", otaPart->label, (unsigned) size);
    grantCredit();
    return true;
}

void otaBleStageBytes(const uint8_t *data, size_t len) {
    if (!len) return;
    std::lock_guard<std::mutex> lk(ringMutex);
    if (!active || failed || !ring) return; // re-check under lock: net-loop teardown frees ring here too
    if (len > RING_CAP - rCount) { // host overran its credit window — fatal, caught later by sha/len
        failed = true;
        return;
    }
    size_t first = std::min(len, RING_CAP - rHead);
    memcpy(ring + rHead, data, first);
    if (len > first) memcpy(ring, data + first, len - first);
    rHead = (rHead + len) % RING_CAP;
    rCount += len;
}

void otaBleTick(unsigned long nowMs) {
    if (!active) return;
    if (abortReq) { otaBleAbort(); return; } // disconnect/abort requested off the net loop
    static uint8_t slice[FLUSH_SLICE];
    bool drained = false;
    for (;;) {
        size_t n;
        {
            std::lock_guard<std::mutex> lk(ringMutex);
            n = std::min(rCount, (size_t) FLUSH_SLICE);
            if (n == 0) break;
            size_t first = std::min(n, RING_CAP - rTail);
            memcpy(slice, ring + rTail, first);
            if (n > first) memcpy(slice + first, ring, n - first);
            rTail = (rTail + n) % RING_CAP;
            rCount -= n;
        }
        // Flash write happens outside the lock so the host task's onWrite never blocks on flash I/O.
        esp_err_t err = esp_ota_write(otaHandle, slice, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTAB FAIL esp_ota_write %s", esp_err_to_name(err));
            failed = true;
            otaBleAbort();
            return;
        }
        mbedtls_sha256_update(&shaCtx, slice, n);
        written += n;
        drained = true;
    }
    if (drained) {
        if (written >= lastProg + 64 * 1024 || written == expectedSize) {
            lastProg = written;
            ESP_LOGI(TAG, "OTAB PROG %u/%u", (unsigned) written, (unsigned) expectedSize);
        }
        grantCredit();
    }
}

bool otaBleEnd() {
    if (!active) { ESP_LOGW(TAG, "OTAB FAIL not-active"); return false; }
    otaBleTick(wallClockMs()); // drain whatever is still staged
    if (!active) return false; // tick aborted on a write error

    if (failed || written != expectedSize) {
        ESP_LOGW(TAG, "OTAB FAIL incomplete %u/%u", (unsigned) written, (unsigned) expectedSize);
        otaBleAbort();
        return false;
    }
    uint8_t got[32];
    mbedtls_sha256_finish(&shaCtx, got);
    if (memcmp(got, expectedSha, 32) != 0) {
        ESP_LOGW(TAG, "OTAB FAIL sha-mismatch");
        otaBleAbort();
        return false;
    }
    esp_err_t err = esp_ota_end(otaHandle); // image validation (magic, esp_app_desc, signature)
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTAB FAIL esp_ota_end %s", esp_err_to_name(err));
        mbedtls_sha256_free(&shaCtx);
        { std::lock_guard<std::mutex> lk(ringMutex); active = false; freeRing(); }
        otaHandle = 0;
        adcSampler.halted = false;
        return false;
    }
    err = esp_ota_set_boot_partition(otaPart);
    mbedtls_sha256_free(&shaCtx);
    { std::lock_guard<std::mutex> lk(ringMutex); active = false; freeRing(); }
    otaHandle = 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTAB FAIL set-boot %s", esp_err_to_name(err));
        adcSampler.halted = false;
        return false;
    }
    ESP_LOGI(TAG, "OTAB OK rebooting");
    systemRestart(); // does not return
    return true;
}

void otaBleRequestAbort() { abortReq = true; } // consumed by otaBleTick on the net loop

void otaBleAbort() {
    if (!active) return;
    { std::lock_guard<std::mutex> lk(ringMutex); active = false; freeRing(); }
    if (otaHandle) { esp_ota_abort(otaHandle); otaHandle = 0; }
    mbedtls_sha256_free(&shaCtx);
    adcSampler.halted = false;
    abortReq = false;
    ESP_LOGW(TAG, "OTAB FAIL aborted");
}

#else // !WITH_BLE — no-op stubs

bool otaBleBegin(uint32_t, const char *) { return false; }
bool otaBleEnd() { return false; }
void otaBleAbort() {}
void otaBleRequestAbort() {}
bool otaBleActive() { return false; }
void otaBleStageBytes(const uint8_t *, size_t) {}
void otaBleTick(unsigned long) {}

#endif
