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
* The wire format is similar to influxdb v1 line protocol (https://docs.influxdata.com/influxdb/v1/write_protocols/line_protocol_tutorial/)
    * `<measurement>,<tag0>=<tagVal0>,... <field0>=<fieldVal0> <timestamp>
* the binary frame would be: <SID(measurement)><SID(tag0)><SID(tag1)>...\0<SID(field0)><SID(fieldVal0)>\0<timestampMs>
* 0 is the delimiter between tags,fields and timestamp. it can be seen as a list termination signal.
* tags and fields are read until we hit the 0
* SID is the varint-encoded symbol lookup value
* field values are encoded as 32-bit float (IEEE)
* the lookup table frame: <sym0><sym1><sym2>...
*   symN being the string name of the symbol with the code word N.
# TODO bools? int8? int16, in32?,
 *
 */