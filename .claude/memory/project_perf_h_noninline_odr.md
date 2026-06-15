---
name: project_perf_h_noninline_odr
description: etc/perf.h defines print_real_time_stats_1s_task non-inline in the header; only one TU may include it
metadata: 
  node_type: memory
  type: project
  originSessionId: 419b0f0b-3a3f-437b-9023-7cd14f70ccf4
---

`src/etc/perf.h:160` *defines* (not just declares) `print_real_time_stats_1s_task(void*)` as a plain non-`inline` free function in the header. Only one translation unit may include `perf.h`, or linking fails with `multiple definition of print_real_time_stats_1s_task`.

**Why:** historically only `main.cpp` included it. When CLI was split into `src/cli.cpp` (which needs that symbol for the `rt-stats` command), including `perf.h` there triggered the duplicate-definition link error.

**How to apply:** to use the symbol from a second TU, forward-declare `void print_real_time_stats_1s_task(void *);` and let the linker resolve it against main.cpp's copy — don't include `perf.h`. The real fix would be marking the definition `inline`, but that was left alone. Related: [[project_cli_extracted_to_cli_cpp]].
