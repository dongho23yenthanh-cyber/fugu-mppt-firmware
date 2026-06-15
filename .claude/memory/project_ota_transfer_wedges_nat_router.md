---
name: project_ota_transfer_wedges_nat_router
description: "OTA image download (~1.6MB) over the NAT router wedges it, cutting off fry+flat"
metadata: 
  node_type: memory
  type: project
  originSessionId: b6a70e89-844c-4796-94b3-a108b61665f5
---

2026-06-11: OTAing fry over the NAT router (192.168.1.231) wedged the router mid-download
(~13%). Symptoms: HTTP server sees BrokenPipe, then BOTH fry and flat go silent on the
broker simultaneously, the havan log collector stops, telnet via NAT times out, discover.py
empty, and the router stops answering ping AND its web UI (full lockup, not just STA-stuck).

**Why:** the sustained ~1.6MB transfer through the NAT overloads it — same class as
[[project_nat_router_lockup_and_fork]] (STA wedge) but here it went fully unreachable, so the
web-UI revival trick didn't apply; needs a physical power-cycle (or the fork's liveness-watchdog).

**Devices are fine** — fry/flat keep converting on the AP (last healthy fry ~15W, flat ~30W),
just isolated from the .1.x LAN. fry stayed on the OLD image (download aborted).

**How to apply:** telnet-over-NAT OTA (ota.py) AND MQTT-triggered OTA both pull the image
through this router, so both can wedge it. Confirm router liveness (`ping 192.168.1.231`)
before declaring a device dead — a dead router masquerades as dead converters. My LAN/broker
stayed fine throughout — isolate the router as the fault, not the converters.

**FIXED 2026-06-11:** router (nimble-ble-proxy @192.168.1.231) now runs a self-recovery
**liveness watchdog** — see [[project_router_liveness_watchdog]]. A future total wedge auto-reboots
within ~3 min (no physical power-cycle). Also brought the May-30 coex/OTA-quiesce fixes onto it
(deployed image was a stale May-29 build). Probe state: `curl http://192.168.1.231/liveness`.
