*this document is an LLM generated placeholder*

# Plan: dedicated beacon node for bsync (ESP32-S3)

## Status (2026-08-04, bench)

- **Phase 0 verdict: the S3 MAC hardware DOES rewrite the timestamp field of raw-injected
  beacon frames.** Evidence: XIAO node (bssid `10:20:ba:05:4c:8d`, ch 13) transmits softAP
  beacons (10/s) + injected sentinel-timestamp frames (50/s); fbuck's bsync accepted ~60/s
  with `rej=0` and locked (e ±2 µs) — a verbatim sentinel would be rejected every time with
  a ~10¹⁹ µs residual, so acceptance at 6× the softAP rate proves hw stamping.
- **Phase 1+2 de facto running**: `etc/bsync-beacon/` (~160 LOC) is the node, flashed on a
  Seeed XIAO ESP32-S3; hw-stamped injected frames at 20 ms already give the high-rate
  timebase with zero receiver changes. LED heartbeat 0.5 Hz = AP+injector alive.
- **Phase 3 partly done**: receiver `bw` knob exists (fbuck runs bw=0.20 → kp=1e-06
  ki=1e-08 A=0.060); wander measurement in progress.
- Bring-up traps hit: XIAO silently re-enters ROM download mode after esptool's RTS reset
  when it was replugged with BOOT held (needs a clean replug); receiver must not be
  associated to an AP or the sniffer stays on that AP's channel (`wifi off` first).
- Open: fboost still on the FRITZ timebase; SSID/PSK per-chip polish; RSSI weighting.

## Goal

A cheap, always-on, off-board beacon source that replaces the household AP as the bsync
timebase. Purpose: raise the accepted-beacon rate and SNR at the converters, which directly
shrinks the seconds-scale phase breathing (currently ±1–2 µs per device, fboost accepted only
0.2–3/s of the FRITZ's 10/s on congested ch1). The converters stay strictly RX-only; the node
is the only transmitter and sits away from the analog front-ends.

Targets: ≥10 accepted beacons/s at every converter, quiet dedicated channel, wander toward
±0.3–0.5 µs (with the `bw` servo-detuning knob), stretch goal ~100–300 ns with high-rate
frames (Phase 2/3).

## Why not just "beacon faster"

The property that makes beacons usable is **hardware TSF insertion**: the MAC writes its TSF
into the timestamp field at the moment the frame leaves, µs-accurate, no software in the loop.
Only the MAC's beacon engine does this, and `wifi_ap_config_t::beacon_interval` is validated
to ≥100 TU (102.4 ms) in ESP-IDF. Software-built frames stamped at *enqueue* time carry
10 µs–ms of TX-queue/CSMA jitter. That jitter is common-mode for a frame received by BOTH
converters (cancels in relative phase), but any frame received by only one device leaks its
jitter into the differential — so software stamps are only acceptable if reception is
near-perfect on a quiet channel, and hardware stamps remain strictly better.

## Phase 0 — decisive experiment (½ h, do first)

Determine whether the S3 MAC hardware rewrites the timestamp field of **raw-injected**
beacon-subtype frames (`esp_wifi_80211_tx`, AP or STA mode):

1. Node injects beacon frames with timestamp field = 0xDEADBEEF... at 20 ms period.
2. A bench converter (existing bsync sniffer + `d`/debug fields, or a 5-line dump patch)
   reports the received timestamp field.
3. Timestamp ≠ the injected constant and monotonic → **hardware stamps injected beacons** →
   Phase 2 gives hw-grade stamps at arbitrary rate, and the receivers need zero changes
   (same BSSID beacon-subtype match). Timestamp echoed verbatim → Phase 2 falls back to
   seq-number ticks (receiver change required), or we stop at Phase 1.

## Phase 1 — minimal softAP beacon node (the workhorse; ~1 evening)

New standalone minimal IDF project `etc/bsync-beacon/` (no Arduino, no fugu deps):

- **Radio**: `WIFI_MODE_AP`, hidden SSID (`bsync-<chipid>`), WPA2 random PSK (nobody joins),
  `beacon_interval=100` TU, conf channel (default a locally quiet one, e.g. 13),
  `esp_wifi_set_max_tx_power(84)` (20 dBm), power save off (AP mode never modem-sleeps).
- **No services**: stop the DHCP server after AP start, no netif traffic — the node emits
  beacons and nothing else.
- **Config**: NVS-backed channel/interval via a 30-line USB-CDC serial prompt (or menuconfig
  defaults only, first cut).
- **Status**: WS2812 heartbeat (devkit LED); boot banner prints the AP MAC (= the `bssid`
  to configure on the converters) and channel.
- **Estimated size**: ~150 LOC main + sdkconfig.

Converter-side: `set-config bsync.conf bssid <node mac>` / `channel <n>`, no code changes.
Expected result: full ~10/s accept rate at both converters, high SNR, no household-AP
dependency (AP reboots, channel hops, DFS moves all disappear).

## Phase 2 — high-rate frames (gated on Phase 0)

**If hw stamps injected beacons**: `esp_timer` (or GPTimer ISR → task notify) drives
`esp_wifi_80211_tx` of minimal beacon frames (24 B header + 12 B fixed body, no IEs beyond
SSID-len-0) at 20–50 ms period alongside the real softAP beacons. Receivers: unchanged.
Measurement rate 20–50/s → alpha-beta noise floor drops ~√5, lock-in proportionally faster
at the same `bw`.

**If not hw-stamped**: inject tick frames carrying a sequence number + the node's µs clock
sampled in the TX task; converters treat ticks as low-weight measurements and the (rare,
hw-stamped) real beacons as anchors. Only worth doing if Phase 1 + `bw` detuning hasn't
already met the target — the common-mode leak analysis above caps the benefit.

## Phase 3 — receiver tuning to exploit it (fugu side, small)

- `bsync.conf bw` knob (default 1.0) scaling {A, B, kp, ki} together — trade lock-in time
  vs wander; with a strong beacon stream, bw≈0.2–0.3 should land ±0.3–0.5 µs.
- Optional: weight measurements by RSSI / reject below threshold (weak frames have worse
  latch jitter).
- Re-check the `dt > 1000 µs` duplicate gate against the shorter frame period (20 ms ticks
  pass; it only rejects <1 ms).

## Validation

- Accept-rate: `svc` beacons counter ≥10/s on both converters (≥40/s Phase 2).
- Wander: log `e` on both devices at 1 Hz for 10 min (`fugu_console --stdin` loop), compute
  std and worst-case |e1−e2|; compare against the FRITZ baseline (±1–2 µs / 0.3–1.7 µs rel).
- Scope: switch-node band width over 1 min, before/after.
- Interference: confirm the node's TX at the bench does not raise the converters' analog
  noise floor (it's off-board, but verify — that's the whole point of the exercise).

## Risks / notes

- IDF may not allow a bare AP with DHCP stopped on some versions — fallback: leave DHCP, the
  hidden/WPA2 AP gets no clients anyway.
- Channel choice: must be 1–13 and legal for beaconing at 20 dBm locally; scan first.
- The node's own crystal (±20 ppm) is irrelevant — it IS the timebase and cancels
  common-mode, exactly like the FRITZ today.
- Don't reuse a NAT-router-flashed unit without `esptool chip_id` + banner check
  (indistinguishable on USB).
