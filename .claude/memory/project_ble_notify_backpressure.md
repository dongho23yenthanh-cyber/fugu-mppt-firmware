---
name: project_ble_notify_backpressure
description: BLE NUS console must pace notify() with backpressure; fire-and-forget drops packets (rc=6)
metadata: 
  node_type: memory
  type: project
  originSessionId: 08fcaf20-b055-4c2e-994a-4abd49286006
---

`bleWrite` in `src/console_ble.cpp` must NOT call `txChar->notify()` per-chunk in a tight loop. NimBLE emits ~one notification per connection interval from a small mbuf pool, so a multi-line response (e.g. `get-config sensor.conf`, ~15 lines) exhausts it and `ble_gatts_notify_custom` returns `BLE_HS_ENOMEM` (rc=6), **silently dropping packets** and truncating the client's output (only the `[E] ...notify(): rc=6` log spam is visible).

The fix (verified on-target): `bleWrite` only appends to a mutex-guarded `txBuf` FIFO; `bleTxDrain()` — pumped every tick from `bleConsoleLoop` via `tickAll()` on core 0 — sends 20-byte chunks with true backpressure. A `TxCallbacks::onStatus` hook catches the `ERROR_GATT` status that `notify()` reports synchronously and clears `lastNotifyOk`; the drain stops the instant a notify fails and retries that chunk next tick, when the pool has refilled. No `vTaskDelay`/guessed pacing. FIFO capped at 8 KB; cleared on disconnect/stop.

Three more TX-side fixes landed in the same pass (all verified on-target, 0 rc=6 over a full connect+commands cycle):
- **Connect-time rc=6 burst:** right after connect the central is still on its slow default interval and the param update hasn't applied, so draining the queued log backlog overflows the controller's ACL buffers. `bleTxDrain` holds for a `TX_SETTLE_MS=500` settle window (re-armed via `txArmSettle` in `onConnect`) before draining.
- **Command→response latency was all on the BLE TX side** (device emits the reply instantly on serial; the lag is pushing it back). Two compounding causes & fixes: (1) `onConnect` calls `requestConnParams(connId, 6, 12, 0, 400)` to ask for a 7.5–15 ms interval instead of the central's ~30 ms+ default — every notification is gated by the interval. (2) `BLEDevice::setMTU(247)` + `bleTxDrain` chunks at the *negotiated* MTU (`getPeerMTU()-3`, fallback 20) instead of a hardcoded 20, so a ~1.1 KB reply is ~5 notifications not ~58. Result: first byte ~25 ms, full multi-line dump ~150–200 ms.
- The `os_msys_num_free() < 4` pre-check (via `#include <os/os_mbuf.h>`) is a secondary guard; it did NOT catch the connect burst because that bottleneck is the controller ACL pool, not host msys.
- **Interactive REPL felt slow for a different reason:** `handleCommand` emitted no completion marker, so `etc/ble_console.py`'s `command()` waited out its full 4 s timeout every command (the periodic `V=…` status line kept its line-queue non-empty so the idle path never fired either). Fix: `loopConsole` (covers UART/telnet/BLE) now writes `OK: <cmd>` / `ERR: <cmd>` via the write callback after `handleCommand` returns (output already flushed synchronously on core 0); the client breaks on that marker. Commands dropped from ~4080 ms to ~90–260 ms. MQTT goes through `handleCommand` directly (not `loopConsole`) so it doesn't get the marker.

Testing tip: macOS keeps a stale BLE bond after a reflash (CBError 14 "Peer removed pairing information"); `blueutil --unpair` does NOT work for BLE on recent macOS (Classic only). Either forget the device in System Settings, or `set-config ble.conf ble_security none` + reboot to bypass bonding entirely for bench tests.

**Why:** fire-and-forget over BLE NUS loses console output, not just logs — a functional bug; and tiny chunks over a slow interval make the console feel laggy.
**How to apply:** keep the queue+drain+onStatus pattern; never revert to looped `notify()`. The old `bleLogWrite` recursion guard is unnecessary now (bleWrite no longer notifies) — relies on NimBLE tag staying at WARN so successful notifies don't log. Related: [[project_ble_nus_console]].
