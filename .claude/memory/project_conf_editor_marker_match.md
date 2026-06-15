---
name: project_conf_editor_marker_match
description: "conf-editor.html must match console OK/ERR markers with includes(), not endsWith()"
metadata: 
  node_type: memory
  type: project
  originSessionId: 088c013f-0cc7-4fdb-a95b-ba1051fbdcf2
---

In `etc/config-tool/conf-editor.html`, the device console reply marker (`OK: <cmd>` / `ERR: <cmd>` from `src/console.cpp`) arrives with a trailing character (whitespace), so matching it with `endsWith()` silently fails and the command times out even though it executed on the device.

**Why:** `getConfig()` always worked because it used `ln.includes("OK: ...")`; the upload path (`sendCommand`) used `endsWith` and produced false "0/1 failed … (no marker; N lines received)" reports while the `set-config`/`del-config` actually applied.

**How to apply:** match console markers with `includes("OK: "+cmd)` / `includes("ERR: "+cmd)`. The upload panel's live per-line logging (every device line shown as it arrives, plus received-line count on timeout) is what diagnosed this — keep it.

Related: the editor reads/uploads via the same string console protocol over serial/BLE ([[project_ble_nus_console]]); device name comes from the `hostname` command (made a getter when called with no arg).
