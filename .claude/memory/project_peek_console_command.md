---
name: peek-console-command
description: "`peek <hex-addr> [len]` console cmd reads RAM/flash; symbol resolution lives host-side"
metadata: 
  node_type: memory
  type: project
  originSessionId: 58c4b649-333d-4643-a9e7-23f69f932cbc
---

`peek <addr> [len]` (registered in `cli.cpp::cmdPeek`, addBoundlessCmd) reads 1–256 bytes from internal RAM, DROM (flash-mapped const), or IRAM/IROM. Address takes `0x…`, decimal, or octal via `strtoul(_, _, 0)`. Default len=4. For `len ∈ {1,2,4,8}` it prints one typed scalar; otherwise a hex+ASCII dump. Executable regions need 4-byte aligned addr+len so it can issue 32-bit instruction-bus loads.

**Why:** the user mentioned this on 2026-05-25 while debugging the post-sweep startCondition livelock — instead of adding ad-hoc `dump-X` commands, peek covers any symbol the host can resolve from the ELF.

**How to apply:** host-side resolve the symbol (objdump/addr2line against the build's .elf), then `peek <addr> [len]`. For a 4-byte scalar (uint32 / float) the default `len=4` prints it directly; for structs pass the full size and decode the hex dump host-side.
