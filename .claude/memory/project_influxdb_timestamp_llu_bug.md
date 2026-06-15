---
name: project_influxdb_timestamp_llu_bug
description: "InfluxDB telemetry timestamps emitted as literal \"lu\" — the now-removed vendored Point lib used %llu under newlib-nano"
metadata: 
  node_type: memory
  type: project
  originSessionId: 20585072-91c9-4dc2-bbb5-c624fabb705f
---

OBSOLETE as of 2026-05-21: the vendored InfluxDB-Client-for-Arduino submodule was removed (see [[project_influxdb_lineprotocol_inhouse]]). The bug below no longer exists, but the lesson stands for any 64-bit ms formatting.

The vendored InfluxDB-Client Point library formatted its 64-bit ms timestamp with `snprintf("%llu", ...)` in `components/InfluxDB-Client-for-Arduino/.../src/util/helpers.cpp::timeStampToString`. Under `CONFIG_NEWLIB_NANO_FORMAT=y` nano printf can't parse `%llu`, so every line-protocol point shipped a timestamp of the literal string `lu` — InfluxDB would reject/mis-time all writes. The old fix split into 9-digit halves with `%lu`. The in-house replacement `LineProtocol::setTimeMs()` sidesteps printf entirely with a manual digit loop, so the class is safe regardless of newlib config.

Found by running `etc/influx_test.py` (the `## influxdb` test in `test/vibe tests.md`) against the physical device. See [[project_newlib_nano_format]].
