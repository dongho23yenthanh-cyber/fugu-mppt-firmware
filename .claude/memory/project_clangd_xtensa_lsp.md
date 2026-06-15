---
name: project_clangd_xtensa_lsp
description: clangd LSP setup for the xtensa-esp32s3-elf toolchain (.clangd file + query-driver / CLion toolchain)
metadata: 
  node_type: memory
  type: project
  originSessionId: c0e576b0-e1f7-4ac9-9234-cc46df1728d3
---

LSP code-insight uses **clangd**, but the build compiler is `xtensa-esp32s3-elf-gcc/g++`. Two problems clangd's bundled Apple/clang frontend has:

1. Three xtensa-only driver flags it can't parse: `-mlongcalls`, `-fno-tree-switch-conversion`, `-fstrict-volatile-bitfields`. Fixed by a committed project-root `.clangd` with `CompileFlags.Remove:` (also strips `-fno-shrink-wrap`, `-mtext-section-literals`, `--param=*`) and `CompilationDatabase: build`.
2. Newlib system headers (`_ansi.h`, etc.) not found because clangd uses macOS system headers. Fixed by making clangd run the xtensa gcc to extract its include dirs.

**Verified:** `clangd --query-driver='/Users/fab/.espressif/tools/**/xtensa-esp32s3-elf-*' --check=src/buck.h` resolves headers (the `_ansi.h not found` error disappears). The leftover `--check` "errors" are `DefineOutline ==> FAIL` refactoring-probe noise, not editor diagnostics. `invalid target "xtensa-esp-elf", ignoring` is benign — includes still import.

`.clangd` works in every clangd editor (VS Code/Cursor/opencode), but **query-driver is a clangd launch flag, not a `.clangd` field** — set it per-editor. **VS Code is now wired**: `.vscode/settings.json` carries `"clangd.arguments": ["--query-driver=/Users/fab/.espressif/tools/**/xtensa-esp32s3-elf-*"]`. The glob matches both installed toolchains (esp-14.2.0 + esp-15.2.0); clangd runs whichever the build's compile_commands.json names (14.2.0). Residual benign messages even when working: `invalid target "xtensa-esp-elf", ignoring` and one `__block attribute not allowed` (Apple-clang Blocks keyword collision in a system header).

**CLion** (`.idea/`) doesn't take query-driver; it probes the compiler set in its toolchain. The repo's CMake profiles use toolchain `esp-idf-v5.5` (`.idea/cmake.xml`). That toolchain (Settings → Build, Execution, Deployment → Toolchains, a global IDE setting, not in-repo) must have C=`xtensa-esp32s3-elf-gcc`, C++=`...-g++` so CLion harvests the newlib headers/macros. CLion does honor the project-root `.clangd`.

compile_commands.json is generated in `build/` by idf.py.
