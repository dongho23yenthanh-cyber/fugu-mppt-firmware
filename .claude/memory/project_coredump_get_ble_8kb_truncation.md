---
name: project-coredump-get-ble-8kb-truncation
description: "`coredump get` truncates at ~5952 B over BLE (8KB TX FIFO drop); pull via MQTT/telnet, or pace the loop"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2942d6e5-8a72-4ad2-9c4d-d09d05dfd848
---

`coredump get` (src/cli.cpp cmdCoredump) streams the dump as ~666 base64 lines (48 raw B/line) via
UART_LOG. Over the **BLE NUS console it deterministically truncates at 5952 decoded B**: the get loop
runs synchronously inside `bleConsoleLoop`'s tick — the *same* task that runs `bleTxDrain()` — so the
drain can't run until the loop finishes. The loop blasts all lines, the `TX_BUF_CAP = 8192` FIFO
(console_ble.cpp) overflows and **silently drops the tail** (`backlog + len > TX_BUF_CAP` branch).
No host-side retry fixes it; esp-coredump then can't parse the partial.

**Workarounds, easiest first:**
- Pull over **MQTT or telnet** instead — message-framed / large TCP buffer, no 8 KB cap, comes whole.
  See [[project-flat-console-via-havan-mqtt-broker]].
- Firmware fix (written on `main`, uncommitted as of 2026-05-29, never flashed because MQTT worked):
  added `bleConsoleAwaitTxDrain(lowWater,timeoutMs)` to console_ble.* (pumps `bleTxDrain`+`vTaskDelay`
  until backlog < lowWater) and call it per-line in the get loop. No-op on UART/telnet/MQTT.

Decoding a dev `-dirty` build: the saved ELF won't SHA-match flat's (different relink → esp-coredump
"coredump SHA256 != app SHA256"). Same commit = same code addresses, so bypass the gate: temporarily
neutralize the `if core_sha_trimmed != app_sha_trimmed:` raise in esp_coredump corefile/loader.py
(`_extract_elf_corefile`), run `info_corefile`, then restore. Symbols/lines resolve correctly.
