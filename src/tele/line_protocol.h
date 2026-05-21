#pragma once
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <sys/time.h>

// Minimal InfluxDB line-protocol builder. Builds one line in place:
//   measurement[,tag=val...] field=val[,field=val...] [ts_ms]
// Tags must precede fields. Replaces the vendored Point class.
class LineProtocol {
    std::string buf_;
    bool fields_ = false, tags_ = false, time_ = false;
    void fieldSep() { buf_ += fields_ ? ',' : ' '; fields_ = true; }
public:
    explicit LineProtocol(const char *measurement = "") { buf_.reserve(256); buf_ = measurement; }
    void addTag(const char *k, const char *v) { buf_ += ','; buf_ += k; buf_ += '='; buf_ += v; tags_ = true; }
    void addField(const char *k, float v, int dp = 2) {
        if (std::isnan(v)) return;                       // lib skips NaN fields
        char b[24]; snprintf(b, sizeof b, "%.*f", dp, v);
        fieldSep(); buf_ += k; buf_ += '='; buf_ += b;
    }
    void addField(const char *k, int v) {                // influx int => trailing 'i'
        char b[16]; snprintf(b, sizeof b, "%di", v);
        fieldSep(); buf_ += k; buf_ += '='; buf_ += b;
    }
    void addField(const char *k, bool v) { fieldSep(); buf_ += k; buf_ += '='; buf_ += (v ? "true" : "false"); }
    void setTimeMs() {                                   // now() as ms epoch, no %llu (newlib-nano)
        timeval tv{}; gettimeofday(&tv, nullptr);
        uint64_t ms = (uint64_t) tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
        char b[21], *s = b + sizeof b - 1; *s = '\0';
        do { *--s = char('0' + ms % 10); ms /= 10; } while (ms);
        buf_ += ' '; buf_ += s; time_ = true;
    }
    bool hasTags() const { return tags_; }
    bool hasTime() const { return time_; }
    bool hasFields() const { return fields_; }
    const std::string &line() const { return buf_; }
    std::string &&takeLine() { return std::move(buf_); }
    std::string takeWire() { return std::move(buf_); }   // uniform with BinaryLineProtocol
};


/**
 *

Create an optimized version of the line protocol:
* measurement names and tag names are encoded as VARINT with symbol table.
* for symbol tables up to ~253, this would be only 1 byte
* symbol code word 0 doesn't exist, 0 is used as a delimiter in the binary wire protocol (see below)
* when a new symbol is seen, before it lands on the wire it is added to the table, and its actual string value and symbol table index is transmitted
* subsequent messages use the value from the symbol table
* the full symbol table is transmitted every 2 minutes or every 200 points
*   accept packet loss decode hole when symbol table got lost
* The wire format is similar to influxdb v1 line protocol (https://docs.influxdata.com/influxdb/v1/write_protocols/line_protocol_tutorial/)
    * the text protocol looks like `<measurement>,<tag0>=<tagVal0>,... <field0>=<fieldVal0> <timestamp>
* the new binary frame would be:
*   <FrameT><SID(measurement)><SID(tag0)><SID(tag1)>...\0<SID(field0)><fieldVal0><SID(field1)><fieldVal1>\0<timestampMs>
* 0 is the delimiter between tags,fields and timestamp. it can be seen as a list termination signal.
* tags and fields are read until we hit the 0
* SID is the varint-encoded symbol lookup value
* DTByte is the data type byte:
* - bool (1)
* - int8 (2)
* - uint8 (3)
* - int16 (4)
* - uint16 (5)
* - i32 (6)
* - u32 (7)
* - f16  (8)
* - f32  (9)
* - f64  (10)
* the lookup table frame: <FrameT><SID0><sym0><DTByte0><SID1><sym1><DTByte1><SID2><sym2><DTByte2>...\0
*   symN being the 0-terminated string name of the symbol with the code word N.
*   DTByte is the datatype for the symbol
* <FrameT> is the frame type byte:
*   for data frames: 1
*   for symbol tables: 2
* when a new symbol appears, the table is resent to cope with UDP loss: first on the next few CONSECUTIVE
*   points (immediate redundancy, ~3x), then a couple more times at a 20s interval. consecutive resends
*   keep the decode hole to 0-2 points on a single packet loss; the 20s cadence heals bursty loss.
* lost-symbol- table gaps are acceptable. we don't expect new fields coming in after some time. drop-out during warm-up phase is ok
* to join multiple wire frames in a UDP datagram, concat frames each length-prefixed (varint):
*   <len0(varint)><frame0><len1(varint)><frame1>...  (length-prefix supersedes the table's trailing \0,
*   which is now only a redundant in-frame guard)
 *
 */