---
name: project_influxdb_lineprotocol_inhouse
description: Telemetry line protocol now built in-house by LineProtocol; vendored InfluxDB-Client-for-Arduino submodule removed
metadata: 
  node_type: memory
  type: project
  originSessionId: 8b3bfbb2-0455-41ec-b530-3b3afa0bf3db
---

The vendored `InfluxDB-Client-for-Arduino` submodule (under `components/`, registered in `.gitmodules`) was removed 2026-05-21. Only its `Point` class + `WritePrecision` were ever used; the UDP send/batching/queue/NTP were already homegrown in `src/tele/telemetry.cpp`.

Replacement: header-only `src/tele/line_protocol.h` class `LineProtocol` — appends measurement/tags/fields/ts directly into one pre-reserved `std::string` (no shared_ptr, no per-key new[]/delete[], no deferred toLineProtocol). Tags must be added before fields (both producers — `MpptController::telemetry` in mppt.cpp and `dcdcDataChanged` in telemetry.cpp — add the `device` tag first). `pointsQ` is now `ReaderWriterQueue<std::string>` holding finished lines; `telemetryAddPoint` enqueues `p.takeLine()` (move). Int fields get the influx `i` suffix; floats use `%.*f`; timestamp via `setTimeMs()` manual digit loop (no `%llu`, see [[project_influxdb_timestamp_llu_bug]]).

**Why:** the lib was memory/perf-heavy for what amounts to writing line protocol over UDP.
**How to apply:** validate end-to-end with `etc/influx_test.py` (udp/8086). Field bytes are kept compatible with that validator. Submodule removal steps: `git submodule deinit -f <path>`, `git rm -r <wrapper-dir>`, `rm -rf .git/modules/<path>`.
