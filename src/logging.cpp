#include "logging.h"

#include <cstring>
#include <ESPTelnet.h>
#include <freertos/FreeRTOS.h>

#include "etc/readerwriterqueue.h"


//struct ConcurrentQueueMinMemTraits : public moodycamel::ConcurrentQueueDefaultTraits {
//    static const size_t BLOCK_SIZE = 2;
//   static const size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 0;
//};

static int vprintf_(const char *fmt, va_list argptr);

static int vprintf_mux(const char *fmt, va_list argptr);

struct AsyncLogEntry {
    char *str;
    uint16_t len;
    bool telnetOnly = false;
};

//static std::deque<AsyncLogEntry> uart_async_log_queue;

//static moodycamel::ConcurrentQueue<AsyncLogEntry, ConcurrentQueueMinMemTraits> uart_async_log_queue{1};
static moodycamel::ReaderWriterQueue<AsyncLogEntry> uart_async_log_queue{};

ESPTelnet *log_telnet = nullptr;

// Up to kMaxLogCallbacks sinks. Registered from the MQTT/BLE tasks, iterated from any core-0
// logging context (vprintf_mux) + the network loop (flush). logCbMux guards the array; readers
// snapshot it under the lock and invoke the callbacks outside (a callback may log -> re-enter).
static constexpr int kMaxLogCallbacks = 4;
static LogCallback logCallbacks[kMaxLogCallbacks] = {};
static int logCallbackCount = 0;
static portMUX_TYPE logCbMux = portMUX_INITIALIZER_UNLOCKED;

// Boot-log backlog: captures early log lines (before any remote sink exists) so the first sink to
// attach — typically MQTT, which can't connect until WiFi is up, well after setup() logs — gets the
// boot sequence replayed. Closes on first attach (or when full); guarded by logCbMux. mqtt-tagged
// lines are skipped so the one-shot replay isn't dropped by mqttLogCallback's ") mqtt:" filter.
static char s_bootLog[8192];
static size_t s_bootLogLen = 0;
static bool s_bootLogOpen = true;

static void bootLogCapture(const char *str, int len) {
    if (!s_bootLogOpen || len <= 0) return;
    if (strnstr(str, ") mqtt:", len)) return;
    portENTER_CRITICAL(&logCbMux);
    if (s_bootLogOpen) {
        size_t room = sizeof(s_bootLog) - s_bootLogLen;
        if ((size_t) len > room) { len = (int) room; s_bootLogOpen = false; } // fill remainder, then close
        memcpy(s_bootLog + s_bootLogLen, str, len);
        s_bootLogLen += len;
    }
    portEXIT_CRITICAL(&logCbMux);
}

// Copy the sink table into `out` (must hold kMaxLogCallbacks) under the lock; returns the count.
static int snapshotLogCallbacks(LogCallback *out) {
    portENTER_CRITICAL(&logCbMux);
    int n = logCallbackCount;
    for (int i = 0; i < n; ++i) out[i] = logCallbacks[i];
    portEXIT_CRITICAL(&logCbMux);
    return n;
}

vprintf_like_t old_vprintf = &vprintf;

bool deferLogs = false;

void loggingEnableDefer() {
    deferLogs = true;
}

void addLogCallback(LogCallback callback) {
    if (!callback) return;
    bool full = false;
    size_t replayLen = 0;
    portENTER_CRITICAL(&logCbMux);
    bool dup = false;
    for (int i = 0; i < logCallbackCount; ++i)
        if (logCallbacks[i] == callback) { dup = true; break; }
    if (!dup) {
        if (logCallbackCount >= kMaxLogCallbacks) full = true;
        else logCallbacks[logCallbackCount++] = callback;
    }
    if (!dup && !full) { replayLen = s_bootLogLen; s_bootLogOpen = false; } // freeze backlog, replay below
    portEXIT_CRITICAL(&logCbMux);
    if (full) ESP_LOGW("log", "log callback table full, dropping"); // log outside the lock
    // Replay the captured boot backlog to the freshly-attached sink (outside the lock; capture is
    // frozen so s_bootLog is stable). One call = one message for MQTT.
    if (replayLen) callback(s_bootLog, (uint16_t) replayLen);
}

void removeLogCallback(LogCallback callback) {
    portENTER_CRITICAL(&logCbMux);
    for (int i = 0; i < logCallbackCount; ++i) {
        if (logCallbacks[i] == callback) {
            logCallbacks[i] = logCallbacks[--logCallbackCount]; // compact (order irrelevant)
            logCallbacks[logCallbackCount] = nullptr;
            break;
        }
    }
    portEXIT_CRITICAL(&logCbMux);
}


static int enqueue_log(const char *fmt, size_t l, const va_list &args, bool appendBreak = false, bool timestamp = false) {
    assert((xPortGetCoreID() == 1)); // ensure RT core

    if (uart_async_log_queue.size_approx() > 200) return -1;
    auto buf = new char[l + 1];
    int len = 0;
    if (timestamp) {
        len = snprintf(buf, l + 1, "(%lu): ", micros());
        if (len <= 0) return len;
    }
    size_t cap = l + 1 - len - size_t(appendBreak);
    auto r2 = vsnprintf(buf + len, cap, fmt, args);
    if (r2 <= 0) return len; // error or empty
    // vsnprintf returns the untruncated length; clamp so buf[len]='\n' stays in bounds.
    if ((size_t) r2 >= cap) r2 = (int) cap - 1;
    len += r2;
    if (appendBreak) {
        buf[len] = '\n';
        buf[len + 1] = 0;
        len += 1;
    }
    uart_async_log_queue.enqueue(AsyncLogEntry{buf, (uint16_t) len, false});
    return len;
}

/*
void enqueue_telnet_log(const char *s, int len) {
    assert((xPortGetCoreID() == 1));

    if (uart_async_log_queue.size_approx() > 200) return;
    auto buf = new char[len + 1];
    strncpy(buf, s, len + 1);
    uart_async_log_queue.enqueue(AsyncLogEntry{buf, (uint16_t) len, true});
}
*/
/*
void enqueue_telnet_log(const char *fmt, size_t l, const va_list &args) {
    assert((xPortGetCoreID() == 1));

    if (uart_async_log_queue.size_approx() > 200) return;
    auto buf = new char[l + 1];
    auto len = vsnprintf(buf, l + 1, fmt, args);
    uart_async_log_queue.enqueue(AsyncLogEntry{buf, (uint16_t) len, true});
}*/


void UART_LOG(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (xPortGetCoreID() == 1 && deferLogs) {
        // RT core1: defer all log to core0
        enqueue_log(fmt, 200, args, true, true);
    } else {
        vprintf_mux(fmt, args);
        vprintf_mux("\n", va_list{});
    }
    va_end(args);
}

void printf_mux(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (xPortGetCoreID() == 1 && deferLogs) {
        // RT core1: defer all log to core0
        enqueue_log(fmt, 200, args, false);
    } else {
        vprintf_mux(fmt, args);
    }
    va_end(args);
}


/*
void UART_LOG_ASYNC(const char *fmt, ...) {

    assert (xPortGetCoreID() == 1);
    //UART_LOG(fmt, ...);

    if (uart_async_log_queue.size_approx() > 200)
        return;



    auto buf = new char[384];

    va_list args;
    va_start(args, fmt);
    uint16_t l = vsnprintf(buf, 380, fmt, args);
    va_end(args);
    buf[l++] = '\r';
    buf[l++] = '\n';
    buf[l] = '\0';

    uart_async_log_queue.enqueue(AsyncLogEntry{buf, l, false});

    / * if (uart_async_log_queue.size() > 200) {
         delete[] uart_async_log_queue.front().str;
         uart_async_log_queue.pop_front(); // TODO fix race, use lock-free queu
         // e.g. https://github.com/cameron314/concurrentqueue/blob/master/concurrentqueue.h
     }* /
}
*/

static int printf_old(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int l = old_vprintf(fmt, args);
    va_end(args);
    return l;
}

void flush_async_uart_log() {
    // Cap entries drained per call so the network loop can pet the TWDT.
    // printf_old() blocks in uart_tx_char waiting on FIFO room: at 115200 baud a 60-byte
    // line costs ~5 ms, and if the RT core keeps enqueueing faster than UART drains (e.g.
    // a vconv saturation that spams Vin-OV warnings every sample), the while-loop never
    // sees an empty queue and core-0 sits inside flush past TASK_WDT_TIMEOUT_S → reset.
    // 32 entries ≈ 160 ms worst-case; the remainder drains on the next loop tick after
    // vTaskDelay(1). RT-side cap at queue>200 already drops overflow.
    constexpr int kMaxPerCall = 32;
    AsyncLogEntry entry;
    for (int i = 0; i < kMaxPerCall && uart_async_log_queue.try_dequeue(entry); ++i) {
        if (!entry.telnetOnly) {
            printf_old(entry.str);
        }

        if (log_telnet)
            log_telnet->write((uint8_t *) entry.str, entry.len);

        LogCallback cbs[kMaxLogCallbacks];
        int n = snapshotLogCallbacks(cbs);
        for (int i = 0; i < n; ++i) cbs[i](entry.str, entry.len);

        bootLogCapture(entry.str, entry.len);

        delete[] entry.str;
    }
}

/**
 * Synchronous printf to UART, JTAG USB and telnet
 * @param fmt
 * @param argptr
 * @return
 */
static int vprintf_mux(const char *fmt, va_list argptr) {
    char loc_buf[300]; // stack-local: vprintf_mux re-enters across core-0 tasks (net loop + mqtt), a static races

    int r = old_vprintf(fmt, argptr);

    if (log_telnet or logCallbackCount or s_bootLogOpen) {
        va_list ap2;            // argptr is spent by old_vprintf; the mirror needs its own copy
        va_copy(ap2, argptr);
        int l = vsnprintf(loc_buf, sizeof(loc_buf), fmt, ap2);
        va_end(ap2);
        if (l > 0) {
            char *buf = loc_buf;
            // vsnprintf returns the untruncated length; writing l from loc_buf over-reads the stack
            if (l >= (int) sizeof(loc_buf)) {
                buf = (char *) malloc(l + 1);
                if (buf) {
                    va_copy(ap2, argptr);
                    vsnprintf(buf, l + 1, fmt, ap2);
                    va_end(ap2);
                } else {
                    buf = loc_buf;
                    l = sizeof(loc_buf) - 1;
                }
            }
            if (log_telnet) log_telnet->write((uint8_t *) buf, l);
            LogCallback cbs[kMaxLogCallbacks];
            int n = snapshotLogCallbacks(cbs);
            for (int i = 0; i < n; ++i) cbs[i](buf, l);
            bootLogCapture(buf, l);
            if (buf != loc_buf) free(buf);
        }
    }

    return r;
}


static int vprintf_(const char *fmt, va_list argptr) {
    // Defer to the core0 flush only FROM core1 (the RT core, or when it can't yield — critical
    // section/ISR). enqueue_log() asserts core1, so core0 must always take the synchronous
    // vprintf_mux path; this also makes it safe to route esp_log here from the very start of setup()
    // (so boot-log capture sees the setup() body). core1 behaviour is unchanged.
    if (xPortGetCoreID() == 1 && (deferLogs || !xPortCanYield())) {
        return enqueue_log(fmt, 200, argptr);
    } else {
        return vprintf_mux(fmt, argptr);
    }
}

void set_logging_telnet(ESPTelnet *telnet) {
    log_telnet = telnet;
    ESP_LOGI("log", "%s telnet logging", telnet ? "Enabled" : "Disabled");
}

void enable_esp_log_to_telnet() {
    // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/log.html
    old_vprintf = esp_log_set_vprintf(&vprintf_);
    if (!old_vprintf)
        old_vprintf = &vprintf;
}


class TaskQueue {
    typedef std::function<void(void)> Fn;
    moodycamel::ReaderWriterQueue<Fn> q{1};

public:
    inline void add(Fn &&fn) {
        assert(q.enqueue(std::move(fn)));
    }

    inline void work() {
        Fn fn;
        while (q.try_dequeue(fn)) {
            fn();
        }
    }
};

static TaskQueue tq;

void enqueue_task(std::function<void(void)> &&fn) {
    tq.add(std::move(fn));
}

void process_queued_tasks() {
    tq.work();
}
