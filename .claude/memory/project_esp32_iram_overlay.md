---
name: project-esp32-iram-overlay
description: classic ESP32 build needs sdkconfig.defaults.esp32 to disable WiFi RX IRAM opt or .iram0 overflows
metadata: 
  node_type: memory
  type: project
  originSessionId: 8f3751a3-c262-460a-bad4-487c1a773f7b
---

Building this firmware for classic ESP32 (vs. the default esp32s3) overflows `iram0_0_seg` by ~4.5 KB because WiFi+BLE+ADC ISR_IRAM_SAFE all want IRAM and classic ESP32 only has 128 KB.

**Fix:** `sdkconfig.defaults.esp32` (committed) sets `# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set`, freeing ~16 KB IRAM. ESP-IDF auto-layers `sdkconfig.defaults.<TARGET>` on top of `sdkconfig.defaults`.

**Why:** classic ESP32 is a secondary target; perf cost of WiFi RX running from flash is negligible (WiFi pinned to core 0, RT loop on core 1).

**How to apply:** for esp32 builds use a separate build dir, e.g. `IDF_TARGET=esp32 idf.py -B build-esp32 build`. Don't reuse `build/` — its CMakeCache locks the target. Resulting IRAM occupancy ~93% (9 KB free); flash app partition is tight at 4% free.
