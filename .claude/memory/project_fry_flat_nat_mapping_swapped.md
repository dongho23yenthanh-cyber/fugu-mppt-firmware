---
name: project_fry_flat_nat_mapping_swapped
description: "NAT port→host mapping is NOT stable; it flip-flops (232=flat 2026-05-22, then 232=fry 2026-06-07) — never assume, always confirm via hostname/ip"
metadata: 
  node_type: memory
  type: project
  originSessionId: 159ce19a-958c-467f-b95f-5ddde2928a29
---

The NAT port→host mapping (192.168.1.231:232/233) is **not stable — it flip-flops between sessions**:

- 2026-05-22: `:232`→flat (192.168.4.2), `:233`→fry (192.168.4.3)
- 2026-06-07: `:232`→**fry** (192.168.4.2), `:233`→**flat** (192.168.4.3) — swapped back

So neither CLAUDE.md's table nor any prior session's mapping can be trusted. Always confirm the
target with the `hostname` command (or `Welcome to <host>` banner / `ip`) before issuing anything.

**How to apply:** before driving/flashing fry or flat, open the console and read the welcome banner;
don't assume the port→host mapping. Related: [[project_charger_shared_bms_and_flat_vout_cal]].
