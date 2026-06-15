---
name: vsnprintf-return-oob-landmine
description: vsnprintf returns the untruncated length; using it to advance a write pointer into a fixed buffer causes heap-adjacent OOB writes
metadata: 
  node_type: memory
  type: project
  originSessionId: 6db3e11f-6a47-4930-8b80-fff08e435077
---

When `vsnprintf(buf, n, fmt, args)` truncates, it returns the *untruncated* length (what it *would* have written), not what fit. Adding that return value to a write pointer in a fixed-size `new char[...]` buffer walks past the end and smashes the next heap block. The TLSF allocator catches it later in `block_locate_free: block_size >= *size` with a backtrace pointing at an innocent `malloc` (e.g. `rtcount`'s `unordered_map::operator[]`).

**Why:** Fixed twice in `src/logging.cpp` now — first in `vprintf_mux` (commit `0b8f4fdf` "fix logging of large messages"), then in `enqueue_log` after a sweep-plot TLSF panic on 2026-05-24. Same shape of bug, two strikes. Linked: [[vprintf_mux-va-list-reuse]], [[vprintf_mux-static-locbuf-race]].

**How to apply:** Audit every site that does `r = vsnprintf(p, cap, ...); p += r;` or `buf[r] = '\n';`. Clamp `r = min(r, cap-1)` before using it as an index/offset. Same applies to `snprintf`. HEAP_POISONING_LIGHT in `sdkconfig` flags the next overrun directly instead of via a delayed TLSF assert — leave it on while shaking these out.
