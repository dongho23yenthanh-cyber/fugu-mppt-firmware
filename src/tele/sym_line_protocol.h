#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cassert>
#include <sys/time.h>

// Binary, symbol-table variant of the influx line protocol. Measurement, tag
// keys, tag values and field keys are interned into a per-stream table and put
// on the wire as 1-based LEB128 varints (SID). SID 0 is reserved as a list
// terminator; a canonical varint of a value >=1 never emits a 0x00 byte, so 0
// is unambiguous wherever a SID is expected.
//
// The datatype of a field is a property of its symbol, declared once in the
// table (not repeated per point). Data frames carry only <SID><raw value>;
// the decoder gets the width from the symbol's DT, so it CANNOT parse a data
// frame's fields until it has learned the table. String symbols (measurement,
// tag keys, tag values) carry DT=Str(0).
//
// Frames (each led by a 1-byte FrameT):
//   Table: <2> (<SID><name bytes>0<DT>)* 0      -- entries until a 0-SID
//   Data : <1> <SID(meas)> (<SID(tagK)><SID(tagV)>)* 0
//                          (<SID(fieldK)><raw LE value>)* 0  <ts_ms varint>
// A Table frame with one entry is an incremental def; with all entries it is a
// full snapshot. The trailing 0-SID terminates the entry list so a Table frame
// and a Data frame can be concatenated in one datagram unambiguously (the
// def+data pair stays atomic: either both arrive or both are lost).

enum class WireDT : uint8_t {  // 0 = non-numeric (string) symbol
    Str = 0, Bool = 1, I8 = 2, U8 = 3, I16 = 4, U16 = 5, I32 = 6, U32 = 7, F16 = 8, F32 = 9, F64 = 10,
};

enum class FrameT : uint8_t { Data = 1, Table = 2 };

inline uint8_t dtSize(WireDT t) {  // byte width of a field VALUE (Str never appears as a value)
    switch (t) {
        case WireDT::Str:                                    return 0;
        case WireDT::Bool: case WireDT::I8: case WireDT::U8:  return 1;
        case WireDT::I16:  case WireDT::U16: case WireDT::F16: return 2;
        case WireDT::F64:                                     return 8;
        default:                                             return 4;
    }
}

inline void putVarint(std::string &b, uint64_t v) {
    while (v >= 0x80) { b += char((v & 0x7F) | 0x80); v >>= 7; }
    b += char(v);
}

// IEEE-754 binary32 -> binary16, round-to-nearest. Overflow saturates to inf,
// tiny values flush toward 0. Only emitted if addFieldF16 is actually called.
inline uint16_t f32ToF16(float f) {
    uint32_t x; memcpy(&x, &f, sizeof x);
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t rawE = (x >> 23) & 0xffu;
    int32_t exp = int32_t(rawE) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (rawE == 0xff) return uint16_t(sign | 0x7c00u | (mant ? 0x200u : 0u));  // inf/nan
    if (exp >= 0x1f) return uint16_t(sign | 0x7c00u);                          // overflow -> inf
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);                                  // underflow -> 0
        mant |= 0x800000u;
        uint32_t shift = uint32_t(14 - exp);
        uint16_t h = uint16_t(mant >> shift);
        if ((mant >> (shift - 1)) & 1u) h++;                                   // round half up
        return uint16_t(sign | h);
    }
    uint16_t h = uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
    if (mant & 0x1000u) h++;                                                    // round (carry into exp ok)
    return h;
}

// Interns identifiers -> SID (1-based) with a fixed datatype. Node pointers in
// unordered_map are stable across rehash, so names_ may hold &node.first.
class SymbolTable {
    std::unordered_map<std::string, uint32_t> ids_;
    std::vector<const std::string *> names_;   // index = SID-1
    std::vector<uint8_t> dts_;                  // index = SID-1
    uint32_t pts_ = 0;
    int64_t lastFullMs_ = 0;
    int burstLeft_ = 0;                         // consecutive resends still owed (immediate redundancy)
    int spacedLeft_ = 0;                        // spaced resends still owed
    int64_t spacedNextMs_ = 0;                  // when the next spaced resend is due
    static constexpr int kBurstCount = 3;       // consecutive sends -> hole stays 0-2 pts on a single loss
    static constexpr int kSpacedCount = 2;      // follow-ups at kResendMs to survive bursty loss
    static constexpr int64_t kResendMs = 20000;
    static constexpr uint32_t kEveryPts = 200;
    static constexpr int64_t kEveryMs = 120000;
public:
    uint32_t intern(const char *s, WireDT dt, bool &isNew) {
        auto it = ids_.find(s);
        if (it != ids_.end()) {
            isNew = false;
            assert(dts_[it->second - 1] == uint8_t(dt));  // a symbol's type is fixed for the stream
            return it->second;
        }
        uint32_t sid = (uint32_t) names_.size() + 1;
        auto res = ids_.emplace(s, sid);
        names_.push_back(&res.first->first);
        dts_.push_back(uint8_t(dt));
        isNew = true;
        return sid;
    }
    const std::string &name(uint32_t sid) const { return *names_[sid - 1]; }
    WireDT dt(uint32_t sid) const { return WireDT(dts_[sid - 1]); }
    uint32_t count() const { return (uint32_t) names_.size(); }

    void writeEntry(std::string &out, uint32_t sid) const {
        putVarint(out, sid);
        out += name(sid); out += '\0';
        out += char(dts_[sid - 1]);
    }
    void writeFullTable(std::string &out) const {
        out += char(uint8_t(FrameT::Table));
        for (uint32_t i = 1; i <= names_.size(); i++) writeEntry(out, i);
        out += '\0';   // 0-SID terminates the entry list
    }
    // Call once per point. Returns true if a full table must be emitted now: on
    // the periodic heartbeat, or for a new-symbol resend. A new symbol schedules
    // kBurstCount consecutive sends (tight, single-loss cover) then kSpacedCount
    // sends at kResendMs (bursty-loss cover).
    bool tableDue(int64_t now, bool sawNew) {
        if (sawNew) { burstLeft_ = kBurstCount; spacedLeft_ = kSpacedCount; spacedNextMs_ = now + kResendMs; }
        bool full = (++pts_ >= kEveryPts) || (now - lastFullMs_ >= kEveryMs);
        if (burstLeft_ > 0) { full = true; burstLeft_--; }
        else if (spacedLeft_ > 0 && now >= spacedNextMs_) { full = true; spacedLeft_--; spacedNextMs_ = now + kResendMs; }
        if (full) { pts_ = 0; lastFullMs_ = now; }
        return full;
    }
};

// Builds one point. Same call shape as LineProtocol; take() returns the
// datagram: an optional Table frame (full snapshot, or just this point's
// newly-seen symbols) followed by the Data frame. Tags must precede fields.
class BinaryLineProtocol {
    SymbolTable &tab_;
    std::string body_;                // Data body (no FrameT): meas, tags..0, fields..0, ts
    bool fields_ = false;
    bool sawNew_ = false;             // a symbol was first seen while building this point
    int64_t ts_ = -1;

    uint32_t sym(const char *s, WireDT dt) {
        bool isNew; uint32_t sid = tab_.intern(s, dt, isNew);
        sawNew_ |= isNew;
        return sid;
    }
    void putSid(const char *s, WireDT dt) { putVarint(body_, sym(s, dt)); }
    void beginFields() { if (!fields_) { body_ += '\0'; fields_ = true; } }  // close tag list
    void putVal(const char *k, WireDT dt, const void *p) {
        beginFields();
        putSid(k, dt);
        body_.append((const char *) p, dtSize(dt));   // value width is implied by the symbol's DT
    }
public:
    BinaryLineProtocol(SymbolTable &t, const char *measurement) : tab_(t) {
        body_.reserve(64);
        putSid(measurement, WireDT::Str);
    }
    void addTag(const char *k, const char *v) { putSid(k, WireDT::Str); putSid(v, WireDT::Str); }

    void addField(const char *k, bool v)    { uint8_t b = v ? 1 : 0; putVal(k, WireDT::Bool, &b); }
    void addField(const char *k, int v)      { int32_t x = v; putVal(k, WireDT::I32, &x); }  // matches LineProtocol(int); int32_t=long on xtensa would be ambiguous
    void addField(const char *k, float v, int = 0) {
        if (std::isnan(v)) return;            // skip NaN, like the text builder
        putVal(k, WireDT::F32, &v);
    }
    // narrower numeric helpers — caller chooses width to save bytes/precision
    void addFieldI16(const char *k, int16_t v)  { putVal(k, WireDT::I16, &v); }
    void addFieldU16(const char *k, uint16_t v) { putVal(k, WireDT::U16, &v); }
    void addFieldU8(const char *k, uint8_t v)   { putVal(k, WireDT::U8, &v); }
    void addFieldF16(const char *k, float v)    { if (std::isnan(v)) return; uint16_t h = f32ToF16(v); putVal(k, WireDT::F16, &h); }
    void addFieldF64(const char *k, double v)   { if (std::isnan(v)) return; putVal(k, WireDT::F64, &v); }

    void setTimeMs() {
        timeval tv{}; gettimeofday(&tv, nullptr);
        ts_ = (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    bool hasTime() const { return ts_ >= 0; }

    std::string take() {
        if (!fields_) body_ += '\0';   // degenerate: close (empty) tag list
        body_ += '\0';                 // close field list
        putVarint(body_, (uint64_t) (ts_ < 0 ? 0 : ts_));

        std::string out;
        out.reserve(body_.size() + 48);
        if (tab_.tableDue(ts_ < 0 ? 0 : ts_, sawNew_)) { // full table on heartbeat or new-symbol burst
            std::string tbl; tab_.writeFullTable(tbl);
            putVarint(out, tbl.size()); out += tbl;      // length-prefixed frame
        }
        putVarint(out, body_.size() + 1);                // +1 for the FrameT byte
        out += char(uint8_t(FrameT::Data));
        out += body_;
        return out;
    }
    std::string takeWire() { return take(); }   // uniform with LineProtocol (telemetry)
};

#ifdef LP_HOST_DECODE
// Off-device decoder for round-trip tests / debug. Renders a datagram back to
// text influx lines (one per Data frame). Returns "" on a SID hole (lost def).
#include <cstdio>
struct LpSym { std::string name; uint8_t dt; };
inline float f16ToF32(uint16_t h) {
    uint32_t sign = uint32_t(h & 0x8000u) << 16, exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu, out;
    if (exp == 0) {
        if (!mant) out = sign;
        else { exp = 127 - 15 + 1; while (!(mant & 0x400u)) { mant <<= 1; exp--; } mant &= 0x3ffu; out = sign | (exp << 23) | (mant << 13); }
    } else if (exp == 0x1f) out = sign | 0x7f800000u | (mant << 13);
    else out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f; memcpy(&f, &out, 4); return f;
}
inline uint64_t getVarint(const uint8_t *&p, const uint8_t *end) {
    uint64_t v = 0; int sh = 0;
    while (p < end) { uint8_t b = *p++; v |= uint64_t(b & 0x7F) << sh; if (!(b & 0x80)) break; sh += 7; }
    return v;
}
inline std::string lpDecode(const uint8_t *p, size_t n, std::vector<LpSym> &tab) {
    const uint8_t *dgEnd = p + n; std::string out;
    auto sym = [&](uint64_t sid) -> const LpSym * { return (sid >= 1 && sid <= tab.size()) ? &tab[sid - 1] : nullptr; };
    while (p < dgEnd) {
        uint64_t flen = getVarint(p, dgEnd);     // length-prefixed frame
        const uint8_t *end = p + flen;           // this frame's end
        if (end > dgEnd) break;
        FrameT ft = FrameT(*p++);
        if (ft == FrameT::Table) {
            for (;;) {
                uint64_t sid = getVarint(p, end);
                if (sid == 0) break;                              // 0-SID ends the entry list
                std::string name((const char *) p); p += name.size() + 1;
                uint8_t dt = *p++;
                if (sid > tab.size()) tab.resize(sid);
                tab[sid - 1] = {name, dt};
            }
        } else {  // Data
            const LpSym *m = sym(getVarint(p, end));
            if (!m) return "";
            std::string line = m->name;
            while (*p) {
                const LpSym *k = sym(getVarint(p, end)), *v = sym(getVarint(p, end));
                if (!k || !v) return "";
                line += ','; line += k->name; line += '='; line += v->name;
            }
            p++;  // tag terminator
            bool first = true;
            while (*p) {
                const LpSym *k = sym(getVarint(p, end));
                if (!k) return "";                                // unknown field SID: width unknown, frame lost
                WireDT dt = WireDT(k->dt); char b[32];
                switch (dt) {
                    case WireDT::Bool: snprintf(b, sizeof b, "%s", *p ? "true" : "false"); break;
                    case WireDT::I8:  snprintf(b, sizeof b, "%di", (int) (int8_t) *p); break;
                    case WireDT::U8:  snprintf(b, sizeof b, "%ui", (unsigned) *p); break;
                    case WireDT::I16: { int16_t x; memcpy(&x, p, 2); snprintf(b, sizeof b, "%di", x); } break;
                    case WireDT::U16: { uint16_t x; memcpy(&x, p, 2); snprintf(b, sizeof b, "%ui", x); } break;
                    case WireDT::I32: { int32_t x; memcpy(&x, p, 4); snprintf(b, sizeof b, "%di", (int) x); } break;
                    case WireDT::U32: { uint32_t x; memcpy(&x, p, 4); snprintf(b, sizeof b, "%uui", (unsigned) x); } break;
                    case WireDT::F16: { uint16_t x; memcpy(&x, p, 2); snprintf(b, sizeof b, "%g", f16ToF32(x)); } break;
                    case WireDT::F32: { float x; memcpy(&x, p, 4); snprintf(b, sizeof b, "%g", x); } break;
                    case WireDT::F64: { double x; memcpy(&x, p, 8); snprintf(b, sizeof b, "%g", x); } break;
                    default: return "";
                }
                p += dtSize(dt);
                line += first ? ' ' : ','; first = false; line += k->name; line += '='; line += b;
            }
            p++;  // field terminator
            char ts[24]; snprintf(ts, sizeof ts, " %llu", (unsigned long long) getVarint(p, end));
            line += ts; out += line; out += '\n';
        }
        p = end;  // length prefix is authoritative — resync to next frame
    }
    return out;
}
#endif
