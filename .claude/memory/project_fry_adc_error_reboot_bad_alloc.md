---
name: project_fry_adc_error_reboot_bad_alloc
description: "fry \"ADC error\" reboot loop (May 29) = uncaught std::bad_alloc from log flood, not stack/heap corruption"
metadata: 
  node_type: memory
  type: project
  originSessionId: 40e65c6f-fd57-4205-891c-257c81fdf265
---

fry's `E (…) main: ADC error` + ~15s reboot loop (2026-05-29) root cause: **uncaught `std::bad_alloc`**, not a stack overflow / heap poisoning / gpio_isr issue. Decoded backtrace: `loopRT` (main.cpp:556) → `stopAndBackoff` → `shutdownDcdc` (mppt.h ESP_LOGW "backoff 5s") → `vprintf_` → `enqueue_log` (logging.cpp:116 `new char[]`) → `operator new[]` throws → `std::terminate` → abort → coredump → reboot.

Chain: internal continuous-ADC DMA stalls on fry (resetPeripherals didn't revive it on the old image); the pre-`c295e93` `loopRT` logs `ADC error` + `stopAndBackoff` **every tick** unconditionally → log flood → repeated throwing `new[]` exhausts heap → bad_alloc propagates out of the RT loop (no try/catch) → abort.

**Why:** flat ran the "same" firmware fine because its DMA self-heals on the first reset, so it never enters the flood loop — same image, different luck on the ADC stall. The stack-overflow / HEAP_POISONING=COMPREHENSIVE / `gpio_install_isr_service already installed` leads were all red herrings for THIS crash (poisoning would print CORRUPT HEAP; gpio msg is benign at main.cpp:284). Distinct from [[project_fry_flat_may29_crash_rootcause]] / [[project_sysevt_task_stack_overflow]] / [[project_adc_nosample_watchdog_deadlock]].

**How to apply:** fixed at HEAD by `c295e93` (gate per-tick `ADC error`+`stopAndBackoff` on `!mppt.inBackoff()`) + `196c7d3` (enqueue_log uses `new(std::nothrow)`+null-guard so RT-path log OOM drops the line instead of aborting). fry serial-flashed HEAD (omit littlefs) 2026-05-29, recovered: no reboot loop, 568sps, sensors live. Lesson: throwing `new`/`new[]` anywhere reachable from loopRT logging is a latent abort landmine (CLAUDE.md: no throw from RT loop). fry MAC 70:04:1d:a6:ab:30. The `CONFIG_HEAP_POISONING_COMPREHENSIVE=y` edit in sdkconfig.defaults was a diagnostic, left uncommitted.
