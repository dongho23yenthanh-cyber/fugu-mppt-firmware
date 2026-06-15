---
name: project_cli_extracted_to_cli_cpp
description: "Console command table lives in src/cli.cpp/cli.h, not main.cpp; built only in the non-test CMake branch"
metadata: 
  node_type: memory
  type: project
  originSessionId: 419b0f0b-3a3f-437b-9023-7cd14f70ccf4
---

The SimpleCLI command table, all `cmd*` handlers, `setupCli()`, and `handleCommand()` live in `src/cli.cpp` (interface in `src/cli.h`), moved out of `main.cpp`. `cli.cpp` `extern`s main.cpp's globals (`manualPwm`, `converter`, `mppt`, `adcSampler`, `sensors`, `led`, `nvs`, etc.) and calls `loopLF`/`stopAndBackoff`/`systemRestart` (the first two were made non-static in main.cpp for this).

**Why:** main.cpp was ~1060 lines; the CLI block (~330 lines) was self-contained and worth isolating.

**How to apply:** `cli.cpp` is registered in `main/CMakeLists.txt` **only in the default `else()` branch** alongside `../src/main.cpp` — NOT for `RUN_TESTS`/`MAIN_SRC` builds, because those swap out main.cpp and cli.cpp's externs would be unresolved. ESP_LOG tag stays `"main"`. See [[project_perf_h_noninline_odr]] for the include gotcha hit during the split.
