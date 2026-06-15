---
name: project-esptool-diff-with
description: esptool --diff-with for incremental flashing needs esptool>=5.2; ESP-IDF 5.5 ships v4.11 and Espressif warns against upgrading inside the venv
metadata: 
  node_type: memory
  type: project
  originSessionId: d206c26c-b802-4e03-996c-3c9c1ae2965e
---

`esptool write_flash --diff-with <old.bin>` only writes changed 4 KB sectors and was added in **esptool v5.2.0** (released 2026-02-18). ESP-IDF 5.5's bundled python env pins esptool to **v4.11.x**, which doesn't have the flag — passing it makes esptool consume the snapshot path as the address argument and fail with `Address "..." must be a number`.

**Why:** Espressif explicitly warns against upgrading esptool inside an IDF venv; v5 is planned to ship with ESP-IDF v6. So the only way to use `--diff-with` on this project today is a standalone install (`pip install --user 'esptool>=5.2'`) and invoking that binary directly, not via `idf.py flash`.

**How to apply:** The repo's `flash-diff.sh` already version-gates the flag (parses `esptool version`, requires major≥5 and minor≥2). If someone reports `flash-diff` doing full flashes despite a snapshot existing, check that `$ESPTOOL` resolves to a v5 binary (try `esptool --version`, not `esptool.py --version`). The v5 entry point is `esptool` (no .py); v4 is `esptool.py`.
