---
name: project_teleflush_bad_alloc_panic
description: flat panicked Jun-4 2026 — unguarded std::bad_alloc in TelemetryService::flushQueue (teleflush task) terminates the device
metadata: 
  node_type: memory
  type: project
  originSessionId: 009a198a-86f6-4502-9d59-dbbd1ff98f2a
---

flat (fry-brk1-76-ga59e69c1-dirty, HEAP_POISONING=COMPREHENSIVE) PANICKED ~12:48 Jun-4-2026 after ~56h uptime; one reboot, stable since. `bootinfo` reset reason = PANIC.

Coredump backtrace (decoded, check=ok): `teleflush` task → `TelemetryService::flushTask` → `flushQueue` (`src/tele/telemetry.cpp:321`, the `batch += frame` in the binary path) → `std::string` growth → `operator new(4033)` → **`std::bad_alloc` thrown, unhandled in the task → `std::terminate` → `abort()`**. Heap at boot: free=41628 but no ~4KB-contiguous block (poisoning overhead + fragmentation).

**Bug:** the telemetry flush task has no try/catch (unlike the RT loop, which CLAUDE.md requires to wrap+stopAndBackoff). A transient OOM in the telemetry path crashes the whole converter instead of dropping a batch. Heap pressure correlates with WiFi disruption (LWIP/UDP buffer churn) — flat had survived the prior day's WiFi dropout (ran 56h through it), so the dropout alone isn't the trigger; the unguarded allocation is.

**Fix (not yet applied — confirm before flashing live converters fry/flat):** wrap `flushQueue` body in try/catch(std::bad_alloc) to drop/clear the static `batch`/`msg` on failure; optionally cap and `shrink_to_fit`. Related: [[project_ina226_timeout_loop_latency_shutdown]], [[project_sysevt_task_stack_overflow]].

Decode recipe: `etc/idf-devtools/elf_archive.py find --device flat -o /tmp/flat.elf`; pull dump via `fugu_console.py --mqtt $MQTT_HOST ... --name flat --coredump get --elf /tmp/flat.elf` (writes coredump.bin); local gdb missing → symbolicate with `. ./idf-export.sh` + xtensa-esp-elf-gdb on PATH, symlink `xtensa-esp32s3-elf-gdb`→`xtensa-esp-elf-gdb-3.13`, then `esp-coredump info_corefile --core-format raw -c coredump.bin /tmp/flat.elf`.
