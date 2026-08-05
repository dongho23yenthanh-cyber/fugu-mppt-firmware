---
name: ble-telemetry-stream
description: "Telemetry over BLE implemented (CONFIG_FUGU_WITH_BLE_TELE) — NUS 0005 notify char, binary+tamp records; NOT yet bench-tested on hardware"
metadata: 
  node_type: memory
  type: project
  originSessionId: ee629fbc-82ce-4953-a3c7-f58db5aa12e1
  modified: 2026-08-05T15:21:46.291Z
---

Implemented 2026-08-05 (uncommitted at time of writing). Binary telemetry (sym_line_protocol +
own TampCompress) over NUS char `6E400005-...`, record framing `0x7E + varint len + cid + payload`
(payload = one UDP-datagram-equivalent). Kconfig `CONFIG_FUGU_WITH_BLE_TELE` (depends BLE, default
n) → `WITH_BLE_TELE`.

Key structure:
- `src/tele/tele_core.cpp` — WiFi-free telemetry core split out of telemetry.cpp (g_symtab,
  pointsQ, telemetryAddPoint fan-out, getHostname, timeSynced); compiled when NETW OR BLE_TELE.
- `src/tele/tele_ble.cpp` — sink; all state on the network loop; buffers lazy-alloc on
  `tele-ble 1`, freed on stop/disconnect AND in `BleConsoleService::onStop` (a stopped service
  never ticks → the deferred stopRequested flag alone would leak, reviewer-caught).
- Enable sequence: `set-time <epoch_ms>` then `tele-ble 1`. `set-time` sets `timeSynced` but SNTP
  is now gated on a separate `sntpSynced` (BLE-set clock must not suppress NTP).
- `tele-ble 1` refused while UDP telemetry runs the TEXT wire (would ship binary frames down the
  text branch → garbage to InfluxDB). tele.conf `ble` (default 1) is the kill-switch; `enabled`
  gates UDP only.
- Host: `etc/fugu/teledec.py` (TeleStream: skip-record on semantic error, byte-resync only on
  framing error), `influx_binary_proxy.py --ble` (InfluxDB POST off the bleak thread via queue),
  `fugu_console.py --ble --tele`. fugu-py repo (`etc/fugu`) got transport.py tele_cb + teledec.py
  — remember to commit there too. See [[project-fugu-py-shared-console]].

**Bench-validated 8-05 on fbuck** (serial /dev/cu.usbmodem1201; fboost=1301): ~36 pts/s decoded
via `influx_binary_proxy.py --ble fugu-fbuck`, correct epoch-ms, console coexists, clean
disconnect stop. Crash saga on fboost (27 KB free heap vs fbuck's 74 KB) exposed THREE OOM
landmines, all fixed: (1) console txBuf grew unbounded in *capacity* (consumed prefix never
compacted) → 16 KB doubling realloc bad_alloc'd; fix = one-time reserve(TX_BUF_CAP)+compaction —
try/catch alone did NOT prevent the abort; (2) `TaskQueue::add` assert(q.enqueue) = panic on
malloc failure from the RT loop; now drop+count; (3) tele buffers now all pre-reserved at
`tele-ble 1` (incl. reused packedBuf), streaming path allocation-free. Also: BLE flakiness after
many macOS reconnect cycles = NimBLE mbuf starvation (ATT err 17 "Insufficient Resources" on
CCCD/RX writes) — reboot device; and macOS stale GATT cache after adding char 0005 → toggle BT.

**rpi sink DEPLOYED + verified 8-05:** single `fugu-ble-bridge.service` on rpi.local
(/opt/fugu-ble-bridge, deploy via `etc/deploy_ble_bridge_rpi.sh`) runs
`influx_binary_proxy.py --ble-all --forward-udp 127.0.0.1:8086` — continuous scan for
fugu-*/NUS advertisers, one worker thread per device (connect serialized — BlueZ races),
decoded lines fed to the existing `influxdb-udp-relay` (openpe@influx.fabi.me, db open_pe).
Both bench units land in `mppt` there. Lessons: bluetoothd KEEPS the BLE connection when a
bridge process dies → device stops advertising, looks vanished (`bluetoothctl disconnect <mac>`);
stuck pending connects → "Operation already in progress" (restart bluetooth.service); headless
justworks pairing doesn't complete → bench units run `ble_security=none`; device log storms
starved console replies over BLE → firmware now caps the log mirror at half of txBuf
(console_ble.cpp bleLogWrite). Console cmds from bridge need `wait_ready()` + timeout=10 + retry.

**bluek migration (8-05 pm):** bridge now uses fl4p/bluek (kernel-direct, `import bluek.shadow`
before any bleak import; Linux only) — kills the bluetoothd stale-connection + D-Bus race
classes. bluek fixes pushed (5ea69e2): bleak classmethod scanner API, connect retries with
6 s-capped windows + kernel-advert re-prime (a 0x3E-failed window otherwise HANGS the whole
budget), deadline-bounded prescan. ESP32-S3 peers under WiFi coex miss ~50% of CONNECT_IND
windows (HCI 0x3E) — RF, not a stack bug; the daemon's retry rides it.

**Crash forensics postscript:** the fboost "restart loop" after the fixes = ANOTHER AGENT had
reflashed it (~14:54, wsync work committed as 6833fb5) from a tree without the OOM fixes —
old-code coredumps. After reflashing: one REAL new crash: IDF's vendored NimBLE hard-asserts
`ble_att_cmd.c:91 assert(rc==0)` when ble_l2cap_tx runs out of mbufs mid-fragmentation → panic,
not drop (upstream bug). Mitigated by raising drain mbuf floors: console 8, tele 12 (MTU-sized
notify ≈ 4 blocks). After that: 7+ min soak clean, `coredump: none`, fbuck ~32 pts/s into
InfluxDB via the bridge. **IDF PATCHED (user-approved 8-05):** assert → error return in
`ble_att_tx_with_conn` (unwinds client_att_busy on failed requests; ble_l2cap_tx consumes the
mbuf either way). Patch lives in the IDF tree (now `-dirty`) AND as
`etc/patches/nimble-att-tx-no-panic.patch` — reapply with `git -C
$IDF_PATH/components/bt/host/nimble/nimble apply <patch>` after any IDF update. Details in
doc/dev-notes/ble-telemetry.md.

**Open:** fboost `0sps` sampler (likely tied to its wsync-bench wiring; see commit 6833fb5) →
near-zero points. No-WiFi boot still mis-archives today-as-yesterday in DailyEnergyMeter until
set-time. Heap optimization tracked as session todo.
