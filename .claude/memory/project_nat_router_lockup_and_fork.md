---
name: nat-router-lockup-and-fork
description: The 192.168.1.173 NAT router (esp32_nat_router_extended) wedges in an STA-stuck-but-AP-alive state; patched fork at fl4p has the fix
metadata: 
  node_type: memory
  type: project
  originSessionId: 6cb80ccd-7030-4a48-af84-e488c081ed1f
---

The ESP32 NAT router at 192.168.1.173 (hostname `esp32-repeater.lan`) is the
upstream gateway for fry/flat (and any other devices on its 192.168.4.x AP).
Runs upstream `dchristl/esp32_nat_router_extended`.

**Lockup mode observed 2026-05-22 → 23:** STA disconnects from upstream main
wifi but the soft-AP stays up (so http://192.168.4.1/ web UI is reachable from
clients on its AP). The STA stays wedged for many hours; hitting the web UI
itself appears to revive it (calling any `esp_wifi_*` API from a non-event-loop
path kicks the driver out of its stuck state). Matches the tight-
`esp_wifi_connect()`-in-STA_DISCONNECTED-handler radio-wedge pattern.

**When fry/flat go silent again,** check this router first — if the router is
pingable from havan but fry/flat can't reach 192.168.1.200, the router likely
needs a kick. Reaching its web UI may suffice; otherwise power-cycle.

**Patched fork:** [[fl4p-nat-router-fork]] at `fl4p/esp32_nat_router_extended`.
Branches: `liveness-watchdog` (TWDT panic + STA backoff + 60-min liveness
reboot), `upload-firmware-button`, `editable-ota-url`, `all-fixes` (combined).

**Why:** The field router has the unfixed firmware and will keep wedging until
the user flashes the patched fork. The fixes target the exact tight-reconnect
pattern that caused this incident.

**How to apply:** When diagnosing fry/flat outages, don't immediately blame the
fugu firmware — check the NAT router state and 192.168.1.173 reachability
first. If the router is reachable but wedged, the patched firmware is ready.
