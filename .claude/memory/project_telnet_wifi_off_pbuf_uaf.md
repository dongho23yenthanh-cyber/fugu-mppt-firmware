---
name: project-telnet-wifi-off-pbuf-uaf
description: "FIXED — `wifi off N` over telnet crashed the device (reentrant lwip pbuf UAF via the log mirror); WiFi teardown now deferred out of the input callback"
metadata: 
  node_type: memory
  type: project
  originSessionId: a73d2322-985c-45f0-83c1-e7f91dff1391
---

**FIXED 2026-05-30** (build fry-brk1-69, uncommitted as of writing). Fix: `cmdWifi` off-branch no longer calls `disconnect_wifi()` synchronously — it only sets `g_app.disableWifi`. A new **WiFi-down edge in `networkLoopTick`** (main.cpp) runs the teardown outside any input callback: `g_services.stopNetworkServices()` (new service.h method, symmetric to `startEnabledNetworkServices`) → `disconnect_wifi(true)`, latched once per edge. Netif is never deinited from inside `telnet.loop()`; the "WiFi off" log mirrors to a still-valid socket. Verified on the esp32-classic vconv unit: survives `wifi off 1` over telnet + a full `fuzz_extreme` flood, coredump partition stays empty (checked via `parttool read_partition`).

**Original crash, found by `fuzz_extreme` (2026-05-30) on the esp32-classic vconv bench unit.** Was reproducible in any NETW build: send **`wifi off <minutes>` over telnet** → device panicked (`InstFetchProhibitedCause`, PC=`0xfefefefe` = heap free-poison → call through a freed callback).

Chain (loopTask / core 0): `TelnetService::onTick → telnet.loop() → onInputReceived lambda` (telnet_service.cpp:62) `→ handleCommand → cmdWifi` (cli.cpp:274-278). The `off` branch calls `disconnect_wifi(true)` (tears down the Wi-Fi netif/lwip **under the telnet socket the command arrived on**), then `UART_LOG("WiFi off for %ld min")`. `UART_LOG → vprintf_mux` (logging.cpp:288) mirrors to `log_telnet` (the same, now-dead telnet client) → `NetworkClient::write` sees a broken socket → `stop()→lwip_close→free_socket→pbuf_free→esp_pbuf_free` calls an already-freed custom-free pointer → UAF. All reentrant inside `telnet.loop()`.

Same family as [[project_mdns_uaf_on_wifi_deinit]] and the [[project_vprintf_mux_va_list_reuse]] / [[project_vprintf_mux_static_locbuf_race]] mirror bugs: **logging to a telnet client while/after that client's network is being destroyed by the command itself.** COMPREHENSIVE heap poisoning made it a clean 0xfefefefe; a production build would UAF less deterministically.

Fix direction (not yet applied): defer the netif teardown out of the telnet input/tick context, or null/guard `log_telnet` before `disconnect_wifi()` so the confirmation isn't mirrored to the dying socket (and never synchronously `close()` a socket from inside `telnet.loop()`). Affects fry/flat too if driven over telnet.
