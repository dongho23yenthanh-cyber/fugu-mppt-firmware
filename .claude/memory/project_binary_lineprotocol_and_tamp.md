---
name: project_binary_lineprotocol_and_tamp
description: binary symbol-table wire protocol (sym_line_protocol.h) + tamp compression; measured 2.6-13x vs text; runtime-selected via tele.conf::binary (NOT a build flag)
metadata: 
  node_type: memory
  type: project
  originSessionId: 7e1c4c73-968e-42dd-bcff-0648ab45af8e
---

A binary variant of the influx line protocol lives in `src/tele/sym_line_protocol.h`
(`BinaryLineProtocol` + `SymbolTable`), alongside the text `LineProtocol` in
`line_protocol.h` (see [[project_influxdb_lineprotocol_inhouse]]). Design: measurement/tag-keys/
tag-values/field-keys interned to 1-based LEB128 SIDs (SID 0 = list terminator); field datatype
is a per-symbol property (WireDT, 0=string) declared in a table frame, NOT repeated per point;
field values are raw little-endian bytes; ts = absolute varint ms. Two frame types (FrameT
1=data, 2=table), each length-prefixed (varint) so frames concat in one UDP datagram. New symbol
-> resend full table on next 3 CONSECUTIVE points then 2x at 20s (consecutive burst keeps a
single-packet loss hole to 0-2 pts; the spec's earlier 20s-only left a ~20-pt hole). f16 via
inline `f32ToF16` (don't use the RobTillaart `float16` class — double math).

Measured on 5000 real `mppt` points (open_pe DB, ~19 fields/pt), B/pt: OLD text 275 | NEW f32
(drop-in) 106 (2.6x) | NEW f16+narrowed ints 69 (4x). With tamp on 40-pt batches: f16 -> 21 B/pt
(~13x vs raw text). Per-DATAGRAM tamp is a net loss (symbol table already removed cross-frame
redundancy); only batched/streamed tamp pays. NOTE: those ratios assumed whole-stream/identical
data; REAL per-batch tamp on *changing f32* telemetry only gets ~50% (raw 3200 -> ~1500 B), so a
3500-raw batch overran MSS (1440). Fixed: Compressor::maxBatchRaw(mtu) sizes the batch per algo
(none: mtu-1; tamp: 1.5*mtu ~= 2160 raw -> ~1.1KB compressed). Verified on-device: '>MSS' warnings
gone. Producers still emit f32 (addField(float)); switching to f16/narrowed would shrink raw AND
improve the ratio.

**Tamp** (BrianPugh/tamp) vendored at `components/tamp/` (embedded match path on xtensa; no
TAMP_ESP32). Swappable compressor abstraction: `src/tele/compress.h` (`Compressor` iface,
`NoCompress`, `TampCompress`, `compressorByName`) + `src/tele/tamp_compress.cpp`. Named `compress.h`/
`TampCompress` to dodge tamp's flat `compressor.h` / `TampCompressor` C struct. Verified host:
C wrapper output decompresses byte-identical via Python `tamp.decompress` (real receiver path).
Firmware C wrapper (window=10, no lazy) ~12% larger than Python tamp; close the gap with
`-DTAMP_LAZY_MATCHING=1` or a bigger window.

**Why:** wire/airtime savings for UDP telemetry + BLE/MQTT.
**How to apply:** WIRED into `telemetry.cpp`, selected at RUNTIME via `tele.conf::binary` (0=text default
so InfluxDB ingestion is unchanged, 1=binary) + `tele.conf::compressor` (none|tamp). `teleLoadWireConf()`
caches both at service start (telemetry.cpp ~244); the old compile-time `WITH_BINARY_TELE` env flag is GONE
(removed from CMakeLists; docs/Configuration.md + conf-editor.html document the keys). `telemetry.h` exposes
`TelePoint`+`makeTelePoint()`; producers (mppt.cpp, dcdcDataChanged) are format-agnostic via the
shared addField surface. Binary path batches length-prefixed frames (TELE_BIN_BATCH=3500 raw ->
~1.1KB after tamp), prepends a 1-byte compressor id, sends one UDP datagram. Flush is time-capped
(`telemetryFlushPointsQ`: queue>=40 OR 1000ms). Compressor from `tele.conf compressor=` (default
tamp). Shared `g_symtab` safe: both producers on RT core (= SPSC queue producer). BOTH builds
verified exit 0 (text + WITH_BINARY_TELE; binary image 1.58MB, 16% free, non-BLE).
**Receiver:** `etc/influx_binary_proxy.py` — UDP listener, strips id byte, tamp.decompress, decodes
length-prefixed frames (per-source symbol table) -> influx line protocol, optional HTTP forward.
Validated against real data (`--test blob.bin`). Plain InfluxDB no longer ingests binary directly.

**On-device benchmark (esp32s3, benchTele, BENCH_TELE=1 build):** text LineProtocol encode
**1645 us/pt** (newlib-nano %f snprintf is brutal) vs BinaryLineProtocol **264 us/pt = 6.2x faster**
— and this is on the RT core (core1, in mppt.update), so binary cuts ~1.4ms RT stall per point.
tamp on a 40-pt batch: 4617->920 B (5x) in **22.6 ms (565 us/pt) but on core0** (flush path), off RT.

**Gotchas learned:** (1) `addField(int32_t)` is AMBIGUOUS on xtensa (int32_t=long, so int->long/bool/
float all equal-rank) — must be `addField(int)` to match LineProtocol; plain `int`/`uint16_t` callers
otherwise fail to compile in binary mode. (2) Env-flag builds (e.g. BENCH_TELE via
component_compile_definitions $ENV) DON'T take effect on a plain rebuild — ninja auto-reconfigures
with STALE env. Must `idf.py reconfigure` (or fullclean) with the env set. (binary mode itself is now
runtime conf, so this no longer applies to it — only remaining $ENV compile flags.) (3) `idf.py flash`
rewrites littlefs (board config); use `app-flash` to preserve it.
