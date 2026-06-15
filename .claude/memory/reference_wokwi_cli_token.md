---
name: reference-wokwi-cli-token
description: Wokwi CI token (WOKWI_CLI_TOKEN) for running wokwi-cli against this project
metadata: 
  node_type: memory
  type: reference
  originSessionId: 8f3751a3-c262-460a-bad4-487c1a773f7b
---

`$WOKWI_CLI_TOKEN` — real value in `.claude/memory/secrets.env` (gitignored; see `secrets.env.example`).

Use with `wokwi-cli` (installed at `/Users/fab/bin/wokwi-cli`) to run the simulator headless. `~/.wokwi/user.tok` is the *license* (for VSCode extension etc.), not the CI token — they're different.

Project's `wokwi.toml` points at `build-esp32/` (classic ESP32 board in `diagram.json`); for esp32s3 builds in `build/` it needs editing. See [[project_esp32_iram_overlay]].
