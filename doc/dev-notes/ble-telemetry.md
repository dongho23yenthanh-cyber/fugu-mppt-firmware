*this document is an LLM generated placeholder*

# Telemetry over BLE

Stream the binary telemetry wire (sym_line_protocol + tamp) over a NUS notify characteristic —
telemetry without WiFi. Build-gated by `CONFIG_FUGU_WITH_BLE_TELE` (depends on `FUGU_WITH_BLE`,
default off).

## Wire format

Characteristic `6E400005-B5A3-F393-E0A9-E50E24DCCA9E` (notify-only, same NUS service as the
console). Notifications are a byte stream of records:

```
<0x7E magic> <varint len> <cid:1B> <payload>
```

`<cid><payload>` is byte-identical to one UDP telemetry datagram (compressor id 0=raw/1=tamp +
concatenated varint-length-prefixed frames, see `sym_line_protocol.h`). Records may span
notifications. `0x7E` occurs freely inside tamp payloads, so the host decoder
(`etc/fugu/teledec.py::TeleStream`) validates the length bound + cid and rescans for the next
`0x7E` on any parse failure.

## Enable sequence (host → device, over the console RX char)

1. `set-time <epoch_ms>` — sets the wall clock (no NTP without WiFi) so points carry real
   epoch-ms timestamps. Also sets TZ. If WiFi comes up later, SNTP still runs and may step the
   clock.
2. `tele-ble 1` — starts the stream. Refused when the clock isn't set, no client is connected,
   tele.conf `ble=0`, or the UDP telemetry service is running the *text* wire (would corrupt its
   output; set `binary=1` and `svc rs tele`). Forces `g_teleBinary` and bursts the symbol table
   so a mid-stream subscriber can decode immediately.

Streaming stops on disconnect (buffers freed); the next client must re-enable — the notify char
has no encryption gate, but `tele-ble` arrives over the (optionally encrypted) RX char.

## Host tools

- `etc/influx_binary_proxy.py --ble <name>` (`--address` for MAC) — decode + print, or forward
  to InfluxDB with `--influx ... --db ...`. Sends the enable sequence itself.
- `etc/fugu_console.py --ble --tele` — interactive console + decoded points as `tele| ...` lines.
- BleTransport (bleak) only; `--ble-proxy`/EspHomeBleTransport has no telemetry subscription.

## Architecture

- `src/tele/tele_core.cpp` — WiFi-free telemetry core (point queue, symbol table, wire conf,
  device id), shared by the UDP path (`telemetry.cpp`, WITH_NETW) and the BLE sink. Compiled when
  either is on. `telemetryAddPoint` fans out to `teleBleEnqueue`; the UDP queue only fills while
  the flush task runs (`g_teleUdpActive`).
- `src/tele/tele_ble.cpp` — the sink: batches frames to ~1.5× the notify MTU (~366 B at MTU 247;
  the 2 KB `RAW_CAP` is the drop threshold, not the batch size), tamp-compresses with its own
  instance (the `compressorByName` singleton is the UDP path's), frames records into a ~4 KB
  drop-newest FIFO, drains via notify from `bleConsoleLoop` with the console's backpressure
  pattern (settle window, `os_msys_num_free()` floor 12 — higher than the console's 8 —
  `onStatus` retry). All state runs on the network loop; buffers allocate on `tele-ble 1`, free
  on stop/disconnect.
- Producer gate: `mppt.telemetry()` runs when the WiFi/influx path is ready *or* BLE is
  streaming; the 20 ms rate limiter dedups when both are active. `teleBleTick` calls
  `mppt.telemetry()` itself so points are produced when the WiFi TelemetryService isn't ticking.

## Bandwidth

~50 pts/s × ~40-60 B ≈ 2-3 KB/s raw (tamp ~2x) vs a ~15-30 KB/s practical BLE ceiling shared
with the console log mirror. Partial batches flush after 250 ms.

## Advertising broadcast (CONFIG_FUGU_WITH_BLE_ADV)

Connectionless one-to-many complement to the NUS stream: with `MAX_CONNECTIONS=1` the NUS
link is exclusive (whoever connects owns the device), so telemetry-only observers use the
advertisement instead. A 17 B packed record rides in the manufacturer-data AD
(company id 0xFFFF, magic 0xF7): `u8 magic, u8 seq, f16 Ui Uo I P, i8 mcu ntc, u16 duty,
u16 lag_us, u8 mppt_state|cv_lim_idx<<4`. No clock needed — the observer stamps time.
`lag_us` saturates at 65535 (read as "≥65.5 ms"); saturation is real on an idle converter,
where the loop-latency watchdog is off and peak lag runs ~100 ms.
`tele.conf::adv_ms` (default 500, 0 = off) sets the refresh; `svc rs ble` re-reads it.
Decode: `influx_binary_proxy.py --adv [--verbose]` (passive scan, dedupe by seq,
struct `<BB4e2b2HB`).

While a client is connected the tick swaps the (controller-stopped) connectable
advertisement for a **non-connectable** one — legacy NimBLE allows that during a connection
(only connectable modes are blocked via `ble_hs_conn_can_alloc`), so the broadcast never
pauses — though it thins under RF contention: bench-measured (8-05), a live console session
(log-mirror notifies + WiFi coex) degrades observer delivery from every 0.5 s to every
~1.5-4 s; it recovers immediately on disconnect. The `teleAdvTick` reconciler, not the disconnect event, is the authority for
restoring the connectable adv: a missed restore would leave the device unreachable while
telemetry keeps flowing (looks healthy). Scan responses (device name) are only answered in
the connectable phase — and don't reach every kernel at all (the rpi scans effectively
passively next to an active LE connection), so every 8th refresh slot broadcasts
flags + complete name instead of the record; observers cache MAC→name (decoder strips
the `fugu-` prefix so tags match the NUS/UDP hostname series), falling back to a
MAC-suffix tag.

Deliberately NOT `CONFIG_BT_NIMBLE_EXT_ADV`: with it, the legacy `ble_gap_adv_*` API still
links but silently returns `BLE_HS_ENOTSUP` (the NUS console would stop advertising with
nothing to see), and porting the connectable set needs private wrapper internals
(`BLEServer::handleGATTServerEvent`). Limits: broadcast is lossy and unencrypted; this
fixes telemetry fan-out only — a second console still needs the one NUS slot
(MAX_CONNECTIONS>1 is a separate question).

## Vendored NimBLE patch (mbuf exhaustion = panic)

IDF's NimBLE hard-asserts in `ble_att_tx_with_conn` (`ble_att_cmd.c:91`, `assert(rc == 0)`
after `ble_l2cap_tx`) — when the msys mbuf pool runs dry mid-fragmentation of a notify, the
device panics instead of dropping the packet. Two mitigations, both in place:

- the drain floors above keep enough headroom that fragmentation shouldn't run dry
  (an MTU-sized notify consumes ~4 blocks);
- the IDF tree is patched to return the error instead of asserting (the mbuf is consumed
  either way, and `client_att_busy` is unwound for failed requests so the ATT client
  doesn't wedge). Patch file: `etc/patches/nimble-att-tx-no-panic.patch`; reapply after an
  IDF update with
  `git -C $IDF_PATH/components/bt/host/nimble/nimble apply <repo>/etc/patches/nimble-att-tx-no-panic.patch`.
  A failed notify then surfaces as `onStatus != SUCCESS_NOTIFY` → the tick retries the chunk.

## Known limitation

With no WiFi, `DailyEnergyMeter::restore` waits up to 10 s at boot and archives today's energy
as "yesterday" until a valid clock arrives (`set-time` comes long after `mppt.begin`). Fix
later: persist the last-known epoch in NVS, or re-evaluate day-restore when the clock lands.
