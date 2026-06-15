---
name: ""
metadata: 
  node_type: memory
  originSessionId: 64e9feaf-808f-4e51-b316-f00aa09ead00
---

Symptom: over BLE, the `℃` in the status line turns into `���` only on the first status
line after a high-volume command (`svc` table) — note the huge `sps` (loopLF starved while the
table drained). `ip` (one short line) never triggers it.

Root cause is the **client**, not the firmware: `etc/config-tool/conf-editor.html` (the `» `-prompt
Web-Bluetooth/MQTT config tool — fugu_console.py uses `> `) decoded **each notification packet**
with a throwaway `new TextDecoder().decode(value)` (no `{stream:true}`). `svc` shifts the byte-stream
alignment so the 3-byte `℃` (`e2 84 83`) straddles a notification boundary; the half ending in `e2`
flushes as one `�` and the next packet's `84 83` are two lone continuation bytes → `���` (exactly 3).
Fixed by a persistent streaming decoder per transport (`bleDec`/`mqttDec` + `{stream:true}`, reset
wherever `bleBuf`/`mqttBuf` resets) — mirroring the already-correct serial loop (`dec`+`{stream:true}`).

Ruled out and **byte-correct**, so don't re-investigate them next time:
- Firmware formatting: both `℃` are identical `e2 84 83` in `main.cpp` status format; `vsnprintf`
  can't corrupt a literal after a correctly-printed number; loopLF is core-0 sequential, `loc_buf`
  stack-local.
- Firmware BLE TX (`bleTxDrain`, console_ble.cpp): NimBLE `notify()` calls `onStatus` synchronously,
  `setValue` copies exact length, chunk-retry preserves bytes. It DOES chunk byte-wise (can split a
  glyph across notifications) — harmless to clients that reassemble bytes, fatal to per-packet
  decoders. Optional firmware defense-in-depth (not applied): trim chunk end to a UTF-8 boundary.
- fugu_console.py / `etc/fugu/*` transports: all buffer **raw bytes** and decode whole lines — immune.

Bonus latent bug found nearby (NOT this symptom): `logging.cpp:237` `printf_old(entry.str)` uses an
RT-enqueued line as a printf format string → any `%` misformats. Should be `printf_old("%s", entry.str)`.

Capture caveat: proxy→fry BLE (`--ble-proxy`) won't subscribe to notifications (fry's NUS needs an
encrypted/bonded link; proxy connects plaintext), and direct bleak to fry is mbuf-fragile — a couple
of reconnects already tripped a GATT bring-up reset. See [[project_ble_notify_backpressure]],
[[project_fugu_py_shared_console]].
