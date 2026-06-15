---
name: project_e2e_test_layout
description: "e2e tests live at etc/e2e-test/ (NOT repo root) and are a custom subprocess runner, not pytest"
metadata: 
  node_type: memory
  type: project
  originSessionId: f3394207-cfe3-4a7f-b250-bd1c33e8378f
---

The host-side end-to-end tests are in `etc/e2e-test/` (under `etc/`, **not** a top-level `e2e-test/`). It is **not** a pytest suite — there is no `conftest.py` / `test_*` fixtures.

- `run_e2e.py` — cluster runner; a `SPECS` table maps each test to a cluster (console / mock / destructive / power / wifi), builds its argv, and runs it as a **subprocess**. `--list`, `--dry-run`, `--cluster`, `--serial/--telnet`.
- `_harness.py` — shared helpers: `Results` (PASS/FAIL/SKIP + `.ok()`), `wait_for`, `EventLog`, `Recorder`, log-line regexes. No `make_console` — each `test_*.py` builds its own `Console` from `fugu.transport` (see `test_nettools.py` for the idiom: insert `repo/etc` on sys.path, `from fugu.transport import ...`, `from _harness import Results`).
- Each `test_*.py` is a standalone script with `main()` taking `--serial`/`--telnet`, returning 0/1.

The console command exerciser (PASS/FAIL/SKIP PLAN) is `etc/e2e-test/test_console_plan.py` (3 tiers: safe always, `--mock`, `--include-network`). It used to live in `etc/fugu_console.py` as `--test`/`--mock`; moved out so the client is just a client. See [[project_fugu_py_shared_console]].
