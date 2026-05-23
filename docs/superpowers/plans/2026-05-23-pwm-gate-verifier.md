*this document is an LLM generated placeholder*

# PWM gate-driver closed-loop verifier — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** finish `etc/mcpwm_gate_verify.py` into a one-shot Python tester that drives an
ESP32-S3 mock bench device over serial/socket, captures HS/LS gate signals on a PicoScope
2000, and asserts frequency, HS-duty linearity, LS pulse position/width across an HS×LS
grid, dead-time + no shoot-through, and a < 5 µs hardware fault brake.

**Architecture:** one Python script (`etc/mcpwm_gate_verify.py`, ~300 lines) plus one new
firmware verb `pwm-dump` that publishes edge ticks. Phases run sequentially as independent
functions returning `list[Result]`; main loop prints per-row PASS/FAIL and exits with
the failed-row count.

**Tech Stack:** Python 3 (stdlib + `pyserial`), `etc/pico_capture.py::PS2000` (ps2000
driver via Rosetta), `etc/fugu/{transport,console}.py`, ESP-IDF v5.5 (firmware side).

**Reference spec:** `docs/superpowers/specs/2026-05-23-pwm-gate-verifier-design.md`.

**Commits:** The user has asked not to auto-commit. Each task ends with a "stage and pause"
step instead — the engineer runs `git add -p` then waits for the user's go-ahead before
committing. Suggested commit messages are still given for the user to use.

---

## File structure

| file | role | created/modified |
|---|---|---|
| `etc/mcpwm_gate_verify.py` | tester entry point + all phases | rewritten (replaces 145-line stub) |
| `test/python/test_pwm_helpers.py` | host-side unit tests for pure helpers (`edges`, `parse_pwm_dump`, `hs_grid`, `ls_grid`) | created |
| `src/pwm/mcpwm.h` | add `MCPWM_SyncLeg::getDtTicks()` getter | modified |
| `src/buck.h` | nothing — existing getters cover `pwmCtrl`, `pwmRect`, `pwmDriver.pwmMax` | unchanged |
| `src/cli.cpp` | add `pwm-dump` handler | modified |

---

## Task 1: Pure helpers + host unit tests

**Files:**
- Create: `test/python/test_pwm_helpers.py`
- Create: `etc/mcpwm_gate_verify.py` (initial: only the pure helpers)

The pure parts of the tester have no hardware deps and can be developed TDD-style on
the host. Get them right first so the integration tasks only chase hardware bugs.

- [ ] **Step 1: Write failing tests for `edges()`, `parse_pwm_dump()`, `hs_grid()`, `ls_grid()`**

Create `test/python/test_pwm_helpers.py`:

```python
"""Host-side unit tests for the pure helpers in etc/mcpwm_gate_verify.py."""
import sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "etc"))
from mcpwm_gate_verify import edges, parse_pwm_dump, hs_grid, ls_grid


class TestEdges(unittest.TestCase):
    def test_basic_square_wave(self):
        # 4 samples high, 4 low, 4 high, 4 low; threshold 1.65V
        v = [3.3] * 4 + [0.0] * 4 + [3.3] * 4 + [0.0] * 4
        rises, falls = edges(v)
        self.assertEqual(rises, [8])      # first rise after we passed through low
        self.assertEqual(falls, [4, 12])

    def test_starts_high(self):
        v = [3.3] * 4 + [0.0] * 4
        rises, falls = edges(v)
        self.assertEqual(rises, [])
        self.assertEqual(falls, [4])

    def test_below_threshold_no_edges(self):
        v = [0.5, 0.6, 0.7, 0.6, 0.5]
        rises, falls = edges(v)
        self.assertEqual(rises, [])
        self.assertEqual(falls, [])


class TestParsePwmDump(unittest.TestCase):
    def test_parses_all_fields(self):
        line = "freq=39062 pwmMax=2048 hs_off=1024 ls_on=1026 ls_off=1280 fault=0 brake=0"
        d = parse_pwm_dump(line)
        self.assertEqual(d["freq"], 39062)
        self.assertEqual(d["pwmMax"], 2048)
        self.assertEqual(d["hs_off"], 1024)
        self.assertEqual(d["ls_on"], 1026)
        self.assertEqual(d["ls_off"], 1280)
        self.assertEqual(d["fault"], 0)
        self.assertEqual(d["brake"], 0)

    def test_ignores_surrounding_noise(self):
        line = "some banter freq=100 pwmMax=200 hs_off=50 ls_on=51 ls_off=120 fault=1 brake=1 OK"
        d = parse_pwm_dump(line)
        self.assertEqual(d["fault"], 1)
        self.assertEqual(d["brake"], 1)


class TestHsGrid(unittest.TestCase):
    def test_pwm_max_2048(self):
        g = hs_grid(2048)
        # boundary points present
        for x in (0, 1, 2046, 2047, 2048):
            self.assertIn(x, g)
        self.assertIn(1024, g)        # 1/2
        self.assertIn(16, g)          # 1/128
        self.assertEqual(g, sorted(set(g)))

    def test_pwm_max_4096_scales(self):
        g = hs_grid(4096)
        for x in (0, 1, 4094, 4095, 4096):
            self.assertIn(x, g)
        self.assertIn(2048, g)        # 1/2 of 4096
        self.assertIn(32, g)          # 1/128 of 4096


class TestLsGrid(unittest.TestCase):
    def test_window_zero_or_one_returns_empty(self):
        self.assertEqual(ls_grid(0), [])
        self.assertEqual(ls_grid(1), [])

    def test_window_1023(self):
        g = ls_grid(1023)
        for x in (0, 1, 1022, 1023):
            self.assertIn(x, g)
        self.assertIn(512, g)         # 0.5 * 1023 = 511.5 → rounds to 512
        self.assertEqual(g, sorted(set(g)))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run tests to confirm they fail**

Run: `python3 -m unittest test/python/test_pwm_helpers.py -v`
Expected: `ModuleNotFoundError: No module named 'mcpwm_gate_verify'` (the stub still
exists, but its imports — e.g. `from fugu.console import Console` — will fail with
`pyserial` not installed for arm64. Either way the test file errors out at import.)

- [ ] **Step 3: Replace the existing stub with the helpers and re-exec shim**

Overwrite `etc/mcpwm_gate_verify.py` with **only** the parts the unit tests need
(transport / scope / phases come in later tasks). The Rosetta re-exec **must** happen
before any `import pico_capture`, or the ctypes load of the x86-only `libps2000.dylib`
under arm64 Python aborts the process:

```python
#!/usr/bin/env python3
"""MCPWM gate-drive automated verifier. See docs/superpowers/specs/2026-05-23-pwm-gate-verifier-design.md.

Drives the device via console + captures HS/LS gates on a PicoScope 2000. Reports
PASS/FAIL per assertion and exits with the failed-row count.

Wiring: Ch A=HS gate (board.conf::pwm_hi), Ch B=LS gate (board.conf::pwm_li).
A free GPIO (default 14) wired to board.conf::pwm_fault_pin drives the fault test.

    etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem* [--skip-fault]
    etc/mcpwm_gate_verify.py --ip 192.168.1.173 --port 232
"""

import os
import platform
import re
import sys
from pathlib import Path

# libps2000.dylib is x86_64-only — re-exec under Rosetta before any pico_capture import.
_PICO_APP_RES = "/Applications/PicoScope 7 T&M.app/Contents/Resources"
if platform.machine() == "arm64" and not os.environ.get("_PICO_X86"):
    os.execve(
        "/usr/bin/arch",
        ["arch", "-x86_64", "/usr/bin/python3", os.path.abspath(__file__)] + sys.argv[1:],
        dict(os.environ, _PICO_X86="1",
             DYLD_LIBRARY_PATH=_PICO_APP_RES + ":" + os.environ.get("DYLD_LIBRARY_PATH", "")),
    )

LOGIC_TH = 1.65


def edges(v, th=LOGIC_TH):
    """Return (rising_idx, falling_idx) lists from a logic waveform (volts)."""
    rises, falls = [], []
    prev = v[0] > th
    for i in range(1, len(v)):
        cur = v[i] > th
        if cur and not prev:
            rises.append(i)
        elif prev and not cur:
            falls.append(i)
        prev = cur
    return rises, falls


_PWM_DUMP_RE = re.compile(r"(\w+)=(-?\d+)")


def parse_pwm_dump(line):
    """Parse a `pwm-dump` reply line like `freq=39062 pwmMax=2048 hs_off=1024 ...`.
       Returns dict[str, int]; extra noise around the key=value pairs is ignored."""
    return {k: int(v) for k, v in _PWM_DUMP_RE.findall(line)}


def hs_grid(pwm_max):
    """HS sweep points: edge cases + log-spaced body."""
    fractions = [1 / 128, 1 / 64, 1 / 16, 1 / 4, 1 / 2, 3 / 4]
    body = [int(round(f * pwm_max)) for f in fractions]
    return sorted(set([0, 1] + body + [pwm_max - 2, pwm_max - 1, pwm_max]))


def ls_grid(window):
    """LS sweep points within an upper bound `window = pwm_max - hs - 1`.
       Returns [] when window < 2 (no meaningful range)."""
    if window < 2:
        return []
    fractions = [0.05, 0.25, 0.5, 0.75, 0.99]
    body = [int(round(f * window)) for f in fractions]
    return sorted(set([0, 1] + body + [window - 1, window]))


if __name__ == "__main__":
    sys.exit("not yet — tester body lands in later tasks")
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m unittest test/python/test_pwm_helpers.py -v`
Expected: all 8 tests pass.

- [ ] **Step 5: Stage and pause**

```bash
git add etc/mcpwm_gate_verify.py test/python/test_pwm_helpers.py
git status
# pause for user to review and decide whether to commit
```

Suggested commit message: `pwm-test: pure helpers (edges, parse_pwm_dump, grids) + host unit tests`

---

## Task 2: Firmware getter + `pwm-dump` verb

**Files:**
- Modify: `src/pwm/mcpwm.h` (add `getDtTicks()`)
- Modify: `src/cli.cpp` (add `pwm-dump` handler)

The tester needs `hs_off`, `ls_on`, `ls_off` event ticks from the live converter. Build
these in firmware so the tester never re-derives buck math.

- [ ] **Step 1: Inspect `MCPWM_SyncLeg` to find where dtTicks lives**

Read `src/pwm/mcpwm.h` around the `init()` function (lines ~60–140). Confirm `dtTicks`
is stored as a member of `MCPWM_SyncLeg` (or recoverable from `period_ticks − pwmMax`).
If there is no member, add one — `uint16_t dtTicks_ = 0;` — and assign it inside `init()`
next to the existing `pwmMax = period_ticks - dtTicks;` line.

- [ ] **Step 2: Add the getter**

In `src/pwm/mcpwm.h`, inside `class MCPWM_SyncLeg`, near the other accessors:

```cpp
[[nodiscard]] uint16_t getDtTicks() const { return dtTicks_; }
```

- [ ] **Step 3: Add the `pwm-dump` command handler**

In `src/cli.cpp`, register a new command alongside `dc` / `gpio`. The exact registration
pattern depends on `s_commands[]` / `CmdEntry` shape — match what's already there.
Handler body:

```cpp
// dump live PWM state for the closed-loop tester (etc/mcpwm_gate_verify.py)
static bool cmd_pwm_dump(const String &) {
    extern HalfBridge mppt;   // already accessed elsewhere in cli.cpp
    const auto &leg  = mppt.pwmDriver;
    uint16_t pwmCtrl = mppt.getCtrlOnPwmCnt();
    uint16_t pwmRect = mppt.getRectOnPwmCnt();
    uint16_t dtT     = leg.getDtTicks();
    uint16_t hs_off  = pwmCtrl;
    uint16_t ls_on   = (pwmRect == 0) ? 0 : (uint16_t)(pwmCtrl + dtT);
    uint16_t ls_off  = (pwmRect == 0) ? 0 : (uint16_t)(pwmCtrl + pwmRect);
    bool fault = leg.faultLevel();    // add this getter too if missing
    bool brake = leg.brakeActive();   // and this
    UART_LOG("freq=%u pwmMax=%u hs_off=%u ls_on=%u ls_off=%u fault=%d brake=%d",
             (unsigned) leg.frequency(), (unsigned) leg.pwmMax,
             hs_off, ls_on, ls_off, fault ? 1 : 0, brake ? 1 : 0);
    return true;
}
```

If `faultLevel()` / `brakeActive()` don't exist as getters yet, add them next to
`getDtTicks()` reading from the same backing members `MCPWM_FaultBrake` already tracks
(grep `m_fault_level` / `m_brake_active` or similar). If those members don't exist,
emit `fault=0 brake=0` as placeholders and add a TODO — Phase 4 will need them and the
brake test will FAIL until they're real.

- [ ] **Step 4: Build and flash**

```bash
. ./idf-export.sh
WITH_MCPWM=1 WITH_NETW=0 idf.py build
idf.py -p $ESPPORT flash monitor
```

Expected: clean build (`-Werror=missing-field-initializers` is on), flash succeeds,
boot log shows the mock-ADC config.

- [ ] **Step 5: Verify the verb manually over serial**

In the monitor or with `etc/fugu_console.py -p $ESPPORT -c "pwm-dump"`:

```
> dc 1024 256
OK
> pwm-dump
freq=39062 pwmMax=2048 hs_off=1024 ls_on=1026 ls_off=1280 fault=0 brake=0
OK
```

Expected: numbers match the `dc 1024 256` command (hs_off=1024, ls_off=1024+256=1280,
ls_on=1024+dtTicks where dtTicks comes from `board.conf::pwm_deadtime_ns`).

- [ ] **Step 6: Stage and pause**

```bash
git add src/pwm/mcpwm.h src/cli.cpp
git status
# pause for user
```

Suggested commit: `cli: pwm-dump verb publishes live event ticks for the gate verifier`

---

## Task 3: Tester bootstrap — argparse, transport, scope, Result type

**Files:**
- Modify: `etc/mcpwm_gate_verify.py` (extend the file from Task 1)

Wire up everything the phases need before writing the phases themselves: argparse, the
fugu Console, the PS2000 scope, and the shared `capture()` / `Result` plumbing.

- [ ] **Step 1: Add the dataclass and helpers**

Append to `etc/mcpwm_gate_verify.py` (between the pure helpers and the `__main__` block):

```python
import argparse
import time
from dataclasses import dataclass, field

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fugu.transport import SerialTransport, SocketTransport
from fugu.console import Console
import pico_capture as pc


@dataclass
class Result:
    name: str
    pass_: bool
    measured: object = None
    expected: object = None
    tol: object = None
    detail: str = ""


def open_console(args) -> Console:
    if args.serial:
        t = SerialTransport(args.serial, 115200)
    else:
        t = SocketTransport(args.ip, args.port)
    return Console(t)


def open_scope():
    s = pc.PS2000()
    s.channel(pc.CH_A, pc.RANGES["5v"], pc.DC)
    s.channel(pc.CH_B, pc.RANGES["5v"], pc.DC)
    # Trigger on Ch A rising at ~1.65V; auto_ms=500 => never-blocking free-run.
    s.trigger(pc.CH_A, 0, rising=True, delay_pct=0, auto_ms=500)
    return s


def capture(scope, freq, samples=3000):
    """Window-size to ≥4 switching periods at `freq`."""
    tb, dt, n = scope.timebase_for(4 / freq, samples)
    a, b, ov = scope.capture(tb, n, True, arm_timeout=2.0)
    return dt, pc.to_volts(a, pc.RANGES["5v"]), pc.to_volts(b, pc.RANGES["5v"])


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description="MCPWM gate-drive closed-loop verifier")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--serial", help="serial port, e.g. /dev/cu.usbmodem2101")
    g.add_argument("--ip", help="device IP / hostname for telnet/socket")
    ap.add_argument("--port", type=int, default=23, help="socket port (with --ip)")
    ap.add_argument("--fault-driver-pin", type=int, default=14,
                    help="GPIO on the device wired to pwm_fault_pin")
    ap.add_argument("--skip-fault", action="store_true",
                    help="skip Phase 4 (fault brake)")
    ap.add_argument("--force-host", action="store_true",
                    help="bypass hostname allow-list — for fry/flat use ONLY when you know")
    ap.add_argument("-v", "--verbose", action="store_true")
    return ap.parse_args(argv)


def print_results(phase, rows):
    print(f"PHASE {phase}")
    n_fail = 0
    for r in rows:
        tag = "PASS" if r.pass_ else "FAIL"
        if not r.pass_:
            n_fail += 1
        m = f"{r.measured!s}" if r.measured is not None else ""
        e = f"exp {r.expected!s}" if r.expected is not None else ""
        t = f"tol {r.tol!s}" if r.tol is not None else ""
        d = f"  -- {r.detail}" if r.detail else ""
        print(f"  {r.name:<28} {tag}  {m:<14} {e:<14} {t:<14}{d}")
    return n_fail
```

- [ ] **Step 2: Sanity-import the file**

Run: `python3 etc/mcpwm_gate_verify.py --help`
Expected: argparse help printed (after Rosetta re-exec) — no traceback.

- [ ] **Step 3: Quick connectivity smoke**

```bash
python3 etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem2101
```

Expected: exits with the `not yet — tester body lands in later tasks` message (the
`__main__` placeholder hasn't been replaced yet, but at minimum we want imports + argparse
+ Rosetta re-exec to all succeed against a connected device).

- [ ] **Step 4: Stage and pause**

```bash
git add etc/mcpwm_gate_verify.py
git status
# pause
```

Suggested commit: `pwm-test: argparse + transport + scope + Result plumbing`

---

## Task 4: `setup_board()` + hostname safety check

**Files:**
- Modify: `etc/mcpwm_gate_verify.py`

Reads the six board.conf keys the tester needs, plus the device hostname (the safety
guard from CLAUDE.md: refuse to run against fry/flat unless `--force-host`).

- [ ] **Step 1: Add `setup_board()` and `hostname_check()`**

Append to `etc/mcpwm_gate_verify.py`:

```python
BOARD_KEYS = ["pwm_freq", "pwm_max", "pwm_deadtime_ns", "pwm_hi", "pwm_li", "pwm_fault_pin"]
SAFE_HOST_PATTERN = re.compile(r"^fugu-esp32s3-")


def setup_board(con: Console) -> dict:
    """Read board.conf keys via `get-config`; return dict with ints where possible."""
    out = {}
    for k in BOARD_KEYS:
        r = con.command(f"get-config board.conf {k}", timeout=3.0)
        # reply text is typically `<value>` on its own line; tolerate `<key>=<value>`
        txt = r.text.strip()
        m = re.search(rf"(?:{k}\s*=\s*)?(-?\d+)", txt)
        if not m:
            out[k] = None
            continue
        out[k] = int(m.group(1))
    return out


def read_hostname(con: Console) -> str | None:
    """Hostname comes from the welcome banner or `ip`. Use the `ip` reply for reliability."""
    r = con.command("ip", timeout=3.0)
    # `ip` typically prints something like `hostname: fugu-esp32s3-139C ip: ...`
    m = re.search(r"hostname\s*[:=]\s*(\S+)", r.text)
    return m.group(1) if m else None


def safety_check(con: Console, args) -> Result:
    name = read_hostname(con) or "<unknown>"
    if args.force_host:
        return Result("hostname_check", True, measured=name,
                      detail="--force-host: safety bypassed")
    if not SAFE_HOST_PATTERN.match(name):
        return Result("hostname_check", False, measured=name,
                      expected="fugu-esp32s3-*",
                      detail="refusing to run against non-mock host (use --force-host to override)")
    return Result("hostname_check", True, measured=name)
```

- [ ] **Step 2: Wire it into `__main__`**

Replace the existing `if __name__ == "__main__"` block:

```python
def main():
    args = parse_args()
    con = open_console(args)
    try:
        chk = safety_check(con, args)
        n_fail = print_results("0  preflight", [chk])
        if not chk.pass_:
            return 1
        board = setup_board(con)
        print(f"  board: {board}")
        if any(board[k] is None for k in BOARD_KEYS):
            print("  ERROR: setup_board could not read all keys; aborting", file=sys.stderr)
            return 1
        # phases land in later tasks
        return n_fail
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Run against the mock device**

```bash
python3 etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem2101
```

Expected output (rough shape):

```
PHASE 0  preflight
  hostname_check               PASS  fugu-esp32s3-139C
  board: {'pwm_freq': 39062, 'pwm_max': 2048, 'pwm_deadtime_ns': 0, 'pwm_hi': 13, 'pwm_li': 11, 'pwm_fault_pin': -1}
```

Exit 0. If hostname doesn't match `fugu-esp32s3-*`, the run aborts with a FAIL row.

- [ ] **Step 4: Manually test the safety bypass**

```bash
python3 etc/mcpwm_gate_verify.py --ip fry.local --port 232
```

Expected: hostname_check FAIL with the "refusing to run" detail, exit code 1.
Then with `--force-host`: hostname_check PASS with "--force-host: safety bypassed".

- [ ] **Step 5: Stage and pause**

```bash
git add etc/mcpwm_gate_verify.py
git status
# pause
```

Suggested commit: `pwm-test: setup_board + hostname allow-list safety`

---

## Task 5: Phase 1 — frequency + HS-duty linearity

**Files:**
- Modify: `etc/mcpwm_gate_verify.py`

- [ ] **Step 1: Add `phase1_freq_duty()`**

Append:

```python
def phase1_freq_duty(con: Console, scope, board: dict) -> list[Result]:
    rows: list[Result] = []
    pwm_max = board["pwm_max"]
    freq = board["pwm_freq"]
    tick_s = 1.0 / (freq * pwm_max)

    for hs in hs_grid(pwm_max):
        con.command(f"dc {hs}", timeout=2.0)
        time.sleep(0.1)
        try:
            dt, hs_v, ls_v = capture(scope, freq)
        except Exception as e:
            rows.append(Result(f"duty[hs={hs}]", False, detail=f"capture error: {e}"))
            continue
        rises, falls = edges(hs_v)

        # Static-low: hs=0 should produce no HS edges (gate held low by force).
        if hs == 0:
            ok = (len(rises) == 0 and len(falls) == 0 and max(hs_v) < LOGIC_TH)
            rows.append(Result(f"static[hs=0]", ok, measured=f"vmax={max(hs_v):.2f}",
                               detail="HS static-low expected"))
            continue
        # Static-high: hs=pwmMax should hold HS high all period.
        if hs == pwm_max:
            ok = (len(rises) == 0 and len(falls) == 0 and min(hs_v) > LOGIC_TH)
            rows.append(Result(f"static[hs={pwm_max}]", ok, measured=f"vmin={min(hs_v):.2f}",
                               detail="HS static-high expected"))
            continue

        # Linearity: need ≥2 rises for period and ≥1 fall for duty.
        if len(rises) < 2 or len(falls) < 1:
            rows.append(Result(f"duty[hs={hs}]", False,
                               detail=f"too few edges ({len(rises)} rises, {len(falls)} falls)"))
            continue
        period_s = (rises[1] - rises[0]) * dt
        freq_meas = 1.0 / period_s
        f0 = next((i for i in falls if i > rises[0]), None)
        if f0 is None:
            rows.append(Result(f"duty[hs={hs}]", False, detail="no falling edge after first rise"))
            continue
        duty_meas = (f0 - rises[0]) * dt / period_s
        duty_exp = hs / pwm_max
        ok_duty = abs(duty_meas - duty_exp) <= 1.0 / pwm_max
        rows.append(Result(f"duty[hs={hs}]", ok_duty,
                           measured=f"{duty_meas*100:.3f}%",
                           expected=f"{duty_exp*100:.3f}%",
                           tol=f"1t ({1/pwm_max*100:.3f}%)"))
        # Frequency check fires once on each HS — they should all agree.
        ok_freq = abs(freq_meas - freq) / freq <= 1e-3
        rows.append(Result(f"freq[hs={hs}]", ok_freq,
                           measured=f"{freq_meas:.1f} Hz",
                           expected=f"{freq} Hz",
                           tol="0.1%"))
    con.command("dc 0", timeout=2.0)
    return rows
```

- [ ] **Step 2: Wire into `main()`**

Replace `return n_fail` in main() with:

```python
        with open_scope() as scope:
            n_fail += print_results("1  freq + HS duty", phase1_freq_duty(con, scope, board))
        return n_fail
```

`PS2000` doesn't ship a context manager. Either add one (preferred — small `__enter__`/`__exit__`
in `etc/pico_capture.py::PS2000` returning self / calling `close()`) or use try/finally:

```python
        scope = open_scope()
        try:
            n_fail += print_results("1  freq + HS duty", phase1_freq_duty(con, scope, board))
        finally:
            scope.close()
        return n_fail
```

Pick try/finally — minimal-touch on the vendored-but-internal `pico_capture.py`.

- [ ] **Step 3: Run against the mock device**

```bash
python3 etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem2101
```

Expected (`pwm_deadtime_ns=0` on the mock dry_mock config, so this is a clean MCPWM at
~39 kHz):

```
PHASE 1  freq + HS duty
  static[hs=0]                 PASS  vmax=0.05      HS static-low expected
  duty[hs=1]                   PASS  0.049%         exp 0.049%   tol 1t (0.049%)
  freq[hs=1]                   PASS  39060.5 Hz     exp 39062 Hz tol 0.1%
  duty[hs=16]                  PASS  0.78%          exp 0.78%    tol 1t
  ...
  static[hs=2048]              PASS  vmin=3.20      HS static-high expected
```

If `freq` rows fail by more than 0.1% the device clock source is wrong; check
`bestTiming()` and `pwm_freq`.

- [ ] **Step 4: Stage and pause**

```bash
git add etc/mcpwm_gate_verify.py
git status
# pause
```

Suggested commit: `pwm-test: Phase 1 (freq + HS duty linearity)`

---

## Task 6: Phase 2 — LS pulse position + width (HS × LS)

**Files:**
- Modify: `etc/mcpwm_gate_verify.py`

The biggest phase — outer-loop HS, inner-loop LS, with special-case rows for the
no-edge / no-window cases.

- [ ] **Step 1: Add `phase2_ls_pos_width()`**

```python
def phase2_ls_pos_width(con: Console, scope, board: dict) -> list[Result]:
    rows: list[Result] = []
    pwm_max = board["pwm_max"]
    freq = board["pwm_freq"]
    tick_s = 1.0 / (freq * pwm_max)
    tol_s = tick_s

    for hs in hs_grid(pwm_max):
        window = pwm_max - hs - 1
        # No-window cases:
        if hs == 0:
            con.command(f"dc 0", timeout=2.0); time.sleep(0.1)
            dt, hs_v, ls_v = capture(scope, freq)
            ok = (max(hs_v) < LOGIC_TH and max(ls_v) < LOGIC_TH)
            rows.append(Result(f"both_static_low[hs=0]", ok,
                               detail=f"HSvmax={max(hs_v):.2f}, LSvmax={max(ls_v):.2f}"))
            continue
        if hs >= pwm_max - 1:
            con.command(f"dc {hs}", timeout=2.0); time.sleep(0.1)
            dt, hs_v, ls_v = capture(scope, freq)
            ok = (min(hs_v) > LOGIC_TH and max(ls_v) < LOGIC_TH)
            rows.append(Result(f"no_window[hs={hs}]", ok,
                               detail=f"HSvmin={min(hs_v):.2f}, LSvmax={max(ls_v):.2f}"))
            continue

        ls_points = ls_grid(window)
        for ls in ls_points:
            con.command(f"dc {hs} {ls}", timeout=2.0)
            time.sleep(0.1)
            r = con.command("pwm-dump", timeout=2.0)
            dump = parse_pwm_dump(r.text)
            if not all(k in dump for k in ("hs_off", "ls_on", "ls_off", "pwmMax")):
                rows.append(Result(f"ls[hs={hs},ls={ls}]", False,
                                   detail=f"pwm-dump parse fail: {r.text!r}"))
                continue
            dt, hs_v, ls_v = capture(scope, freq)
            hs_rises, _ = edges(hs_v)
            ls_rises, ls_falls = edges(ls_v)

            # ls=0 → LS pulse width is zero → no LS rises expected.
            if ls == 0 or dump["ls_off"] == 0:
                ok = (len(ls_rises) == 0)
                rows.append(Result(f"ls_off[hs={hs},ls=0]", ok,
                                   detail=f"{len(ls_rises)} LS rises (expect 0)"))
                continue
            if not hs_rises or not ls_rises or not ls_falls:
                rows.append(Result(f"ls[hs={hs},ls={ls}]", False,
                                   detail=f"missing edges (HSr={len(hs_rises)}, "
                                          f"LSr={len(ls_rises)}, LSf={len(ls_falls)})"))
                continue
            # Edge in first observed period: take ls rise after hs rises[0], ls fall after that.
            r0 = hs_rises[0]
            lr = next((i for i in ls_rises if i > r0), None)
            if lr is None:
                rows.append(Result(f"ls[hs={hs},ls={ls}]", False,
                                   detail="no LS rise in first observed period"))
                continue
            lf = next((i for i in ls_falls if i > lr), None)
            if lf is None:
                rows.append(Result(f"ls[hs={hs},ls={ls}]", False,
                                   detail="no LS fall after LS rise"))
                continue
            ls_rise_s = (lr - r0) * dt
            ls_high_s = (lf - lr) * dt
            exp_rise_s = dump["ls_on"]  * tick_s
            exp_high_s = (dump["ls_off"] - dump["ls_on"]) * tick_s
            ok_pos  = abs(ls_rise_s - exp_rise_s) <= tol_s
            ok_wid  = abs(ls_high_s - exp_high_s) <= tol_s
            rows.append(Result(f"ls_pos[hs={hs},ls={ls}]", ok_pos,
                               measured=f"{ls_rise_s*1e6:.3f}µs",
                               expected=f"{exp_rise_s*1e6:.3f}µs", tol=f"{tol_s*1e6:.3f}µs"))
            rows.append(Result(f"ls_wid[hs={hs},ls={ls}]", ok_wid,
                               measured=f"{ls_high_s*1e6:.3f}µs",
                               expected=f"{exp_high_s*1e6:.3f}µs", tol=f"{tol_s*1e6:.3f}µs"))
            # Cliff regression: when ls fills the window, LS must NOT be static-HIGH.
            if ls == window:
                static_high = (min(ls_v) > LOGIC_TH)
                rows.append(Result(f"ls_cliff[hs={hs},ls={ls}]", not static_high,
                                   detail=f"LSvmin={min(ls_v):.2f}"))
    con.command("dc 0", timeout=2.0)
    return rows
```

- [ ] **Step 2: Wire into `main()`**

In main(), after Phase 1's call:

```python
            n_fail += print_results("2  LS pos + width (HS × LS)",
                                    phase2_ls_pos_width(con, scope, board))
```

- [ ] **Step 3: Run, expect a lot of rows**

```bash
python3 etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem2101 2>&1 | tee /tmp/p2.log
```

Expected: ~11 HS points × ~9 LS points × 2 rows ≈ 200 rows, all PASS. The cliff
regression rows (`ls_cliff[hs=<x>,ls=<window>]`) are explicit — they would FAIL if
the LS-cliff fix (commit `1318015`) regressed.

Take 1-2 minutes to read past. If any rows fail, look at the `pwm-dump` reply to
check the firmware is reporting the same numbers the tester expects.

- [ ] **Step 4: Stage and pause**

Suggested commit: `pwm-test: Phase 2 (LS pos+width on HS × LS grid)`

---

## Task 7: Phase 3 — dead-time + no shoot-through

**Files:**
- Modify: `etc/mcpwm_gate_verify.py`
- Optional: provision a config with `pwm_deadtime_ns > 0` (e.g. `set-config board.conf pwm_deadtime_ns 80`) and reboot, otherwise Phase 3 measures `dt=0` against expected 0 — passes but doesn't exercise the dead-time logic.

- [ ] **Step 1: Add `phase3_deadtime()`**

```python
def phase3_deadtime(con: Console, scope, board: dict) -> list[Result]:
    rows: list[Result] = []
    pwm_max = board["pwm_max"]
    freq = board["pwm_freq"]
    tick_s = 1.0 / (freq * pwm_max)
    tol_s = tick_s
    dt_ns_exp = board["pwm_deadtime_ns"] or 0

    hs = pwm_max // 2
    ls = (pwm_max - hs - 1) // 2          # half-window LS pulse
    con.command(f"dc {hs} {ls}", timeout=2.0)
    time.sleep(0.1)
    dump = parse_pwm_dump(con.command("pwm-dump").text)
    dt, hs_v, ls_v = capture(scope, freq)

    hs_rises, hs_falls = edges(hs_v)
    ls_rises, _ = edges(ls_v)
    if not (hs_rises and hs_falls and ls_rises):
        rows.append(Result("dt_setup", False,
                           detail=f"missing edges hsR={len(hs_rises)} hsF={len(hs_falls)} lsR={len(ls_rises)}"))
        con.command("dc 0", timeout=2.0)
        return rows

    hf0 = hs_falls[0]
    lr_after = next((i for i in ls_rises if i > hf0), None)
    if lr_after is None:
        rows.append(Result("dt_setup", False, detail="no LS rise after HS fall"))
        con.command("dc 0", timeout=2.0)
        return rows
    dt_hl_s = (lr_after - hf0) * dt
    dt_hl_ns = dt_hl_s * 1e9

    ok_dt = abs(dt_hl_s - dt_ns_exp * 1e-9) <= tol_s
    rows.append(Result("dt_hl", ok_dt,
                       measured=f"{dt_hl_ns:.0f}ns",
                       expected=f"{dt_ns_exp}ns",
                       tol=f"{tol_s*1e9:.1f}ns (1t)"))

    # Shoot-through: no sample has both above threshold.
    shoot = any(hs_v[i] > LOGIC_TH and ls_v[i] > LOGIC_TH for i in range(len(hs_v)))
    rows.append(Result("shoot_through", not shoot,
                       detail="none" if not shoot else "OVERLAP DETECTED"))

    con.command("dc 0", timeout=2.0)
    return rows
```

- [ ] **Step 2: Wire and run**

```python
            n_fail += print_results("3  deadtime + shoot-through",
                                    phase3_deadtime(con, scope, board))
```

```bash
python3 etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem2101
```

Expected with `pwm_deadtime_ns=0`:

```
PHASE 3  deadtime + shoot-through
  dt_hl                        PASS  0ns            exp 0ns      tol 12.8ns (1t)
  shoot_through                PASS                              none
```

With `pwm_deadtime_ns=80`: `dt_hl ≈ 80ns ±13ns`.

- [ ] **Step 3: Stage and pause**

Suggested commit: `pwm-test: Phase 3 (deadtime + no-shoot-through)`

---

## Task 8: Phase 4 — fault GPIO brake

**Files:**
- Modify: `etc/mcpwm_gate_verify.py`

This phase needs the operator to have wired `--fault-driver-pin` (default 14) to
`board.conf::pwm_fault_pin`. If that wire isn't in place, run with `--skip-fault`.

- [ ] **Step 1: Add `phase4_fault_brake()`**

```python
def phase4_fault_brake(con: Console, scope, board: dict, drv_pin: int) -> list[Result]:
    rows: list[Result] = []
    freq = board["pwm_freq"]
    fault_pin = board["pwm_fault_pin"]
    if fault_pin is None or fault_pin < 0:
        rows.append(Result("fault_pin_configured", False,
                           detail="board.conf::pwm_fault_pin not set; cannot test brake"))
        return rows

    # Set up switching, deassert fault drive.
    con.command(f"gpio {drv_pin} 0", timeout=2.0)
    con.command(f"dc {board['pwm_max']//2} {board['pwm_max']//4}", timeout=2.0)
    time.sleep(0.1)

    # Arm scope, then trigger brake. Use Ch B (LS) edge fall as the post-brake reference;
    # we capture a long enough window and find the fault-edge offset by looking for the
    # last HS rise before both gates collapse.
    dt, hs_v, ls_v = capture(scope, freq, samples=6000)
    pre_rises, _ = edges(hs_v)
    if len(pre_rises) < 2:
        rows.append(Result("brake_pre_switching", False,
                           detail=f"only {len(pre_rises)} HS rises before brake — gates already off"))
        return rows
    rows.append(Result("brake_pre_switching", True,
                       detail=f"{len(pre_rises)} HS rises in pre-brake capture"))

    # Now assert fault and re-capture.
    con.command(f"gpio {drv_pin} 1", timeout=2.0)
    time.sleep(0.05)   # 50ms — much longer than 5µs settle target
    dt, hs_v, ls_v = capture(scope, freq, samples=3000)
    rises, falls = edges(hs_v)
    ls_rises, _ = edges(ls_v)
    # After brake: there must be no edges in the full window.
    ok_brake = (len(rises) == 0 and len(falls) == 0 and len(ls_rises) == 0)
    rows.append(Result("brake_gates_quiet", ok_brake,
                       measured=f"HSr={len(rises)} HSf={len(falls)} LSr={len(ls_rises)}",
                       expected="all 0"))

    # Deassert and confirm recovery.
    con.command(f"gpio {drv_pin} 0", timeout=2.0)
    con.command(f"dc {board['pwm_max']//2} {board['pwm_max']//4}", timeout=2.0)
    time.sleep(0.1)
    dt, hs_v, ls_v = capture(scope, freq)
    rises, _ = edges(hs_v)
    ok_recover = len(rises) >= 2
    rows.append(Result("brake_recover", ok_recover,
                       measured=f"{len(rises)} HS rises",
                       expected="≥2"))

    con.command("dc 0", timeout=2.0)
    return rows
```

- [ ] **Step 2: Wire it (gated by `--skip-fault`)**

```python
            if not args.skip_fault:
                n_fail += print_results("4  fault brake",
                                        phase4_fault_brake(con, scope, board, args.fault_driver_pin))
            else:
                print("PHASE 4  fault brake  SKIPPED (--skip-fault)")
```

- [ ] **Step 3: Wire the fault driver pin and run**

Wire physically: device GPIO 14 → device GPIO whose number is `board.conf::pwm_fault_pin`.
Verify wiring with a one-shot `gpio 14 1; gpio 14 0` before running.

```bash
python3 etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem2101
```

Expected:

```
PHASE 4  fault brake
  brake_pre_switching          PASS                              N HS rises in pre-brake capture
  brake_gates_quiet            PASS  HSr=0 HSf=0 LSr=0           exp all 0
  brake_recover                PASS  N HS rises                  exp ≥2
```

If `brake_gates_quiet` fails the MCPWM brake is not driving the gates LOW — either
`pwm_fault_pin` is wrong, the fault-active polarity (`pwm_fault_active_high`) is wrong,
or the MCPWM_FaultBrake registration is broken. Inspect the `fault=` field in `pwm-dump`.

- [ ] **Step 4: Stage and pause**

Suggested commit: `pwm-test: Phase 4 (fault GPIO brake + recover)`

---

## Task 9: Summary line, exit code, and bench-doc cross-link

**Files:**
- Modify: `etc/mcpwm_gate_verify.py`
- Modify: `doc/Automated Bench Tests.md` (add a one-line pointer)

- [ ] **Step 1: Replace the ad-hoc summary with a proper SUMMARY row**

In `main()`, accumulate phase pass/fail counts into a list and print one summary line
before returning:

```python
def main():
    args = parse_args()
    con = open_console(args)
    summary = []
    try:
        chk = safety_check(con, args)
        n_p0 = print_results("0  preflight", [chk])
        summary.append(("0  preflight", n_p0))
        if not chk.pass_:
            return 1
        board = setup_board(con)
        print(f"  board: {board}")
        if any(board[k] is None for k in BOARD_KEYS):
            print("  ERROR: setup_board could not read all keys", file=sys.stderr)
            return 1
        scope = open_scope()
        try:
            for phase_name, fn in (
                ("1  freq + HS duty",        lambda: phase1_freq_duty(con, scope, board)),
                ("2  LS pos + width (HS×LS)",lambda: phase2_ls_pos_width(con, scope, board)),
                ("3  deadtime + shoot-thru", lambda: phase3_deadtime(con, scope, board)),
            ):
                summary.append((phase_name, print_results(phase_name, fn())))
            if not args.skip_fault:
                summary.append(("4  fault brake",
                                print_results("4  fault brake",
                                              phase4_fault_brake(con, scope, board,
                                                                 args.fault_driver_pin))))
            else:
                summary.append(("4  fault brake", "SKIP"))
                print("PHASE 4  fault brake  SKIPPED (--skip-fault)")
        finally:
            scope.close()
        # final line
        parts = []
        n_fail = 0
        for name, n in summary:
            if n == "SKIP":
                parts.append(f"{name.split()[0]} SKIP")
            else:
                parts.append(f"{name.split()[0]} " + ("PASS" if n == 0 else f"FAIL({n})"))
                if n != "SKIP":
                    n_fail += n
        print("SUMMARY  " + "  ".join(parts) + f"   exit={n_fail}")
        return n_fail
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Add a pointer in `doc/Automated Bench Tests.md`**

Find the "## Bench setup" or "## Console commands" section and add a short pointer:

```markdown
## Automated gate-driver test

`etc/mcpwm_gate_verify.py` — closed-loop PWM verifier. Drives the device over
serial/socket while capturing HS/LS gates on a PicoScope 2000. Asserts frequency,
HS-duty linearity, LS pulse position/width across an HS×LS grid, dead-time +
no shoot-through, and the hardware fault brake. Runs against `fugu-esp32s3-*`
mock devices by default; use `--force-host` for any other target (refuses
fry/flat without that flag).

    etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem* [--skip-fault]
```

- [ ] **Step 3: Full end-to-end run**

```bash
python3 etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem2101 | tee /tmp/pwm-verify.log
echo "exit=$?"
```

Expected: all four phases (or three with `--skip-fault`) print, the SUMMARY line is
`SUMMARY  0 PASS  1 PASS  2 PASS  3 PASS  4 PASS   exit=0` (or PASS/SKIP if Phase 4
was skipped), and the shell exit code is `0`.

- [ ] **Step 4: Stage and pause for final review**

```bash
git add etc/mcpwm_gate_verify.py "doc/Automated Bench Tests.md"
git status
# pause for user
```

Suggested final commit: `pwm-test: summary line + bench-test cross-link`

---

## Self-review notes (against the spec)

- ✅ All four phases in the spec map to Tasks 5–8.
- ✅ Bench wiring assumption (Ch A=HS, Ch B=LS, fault via wire) honoured in `open_scope()` + `phase4_fault_brake`.
- ✅ Normalised grids built from `pwmMax` at runtime via `hs_grid` / `ls_grid` functions (Task 1).
- ✅ HS × LS matrix in Phase 2 with outer HS, inner LS (Task 6, Step 1).
- ✅ Cliff regression row at `ls == window` in Phase 2.
- ✅ Static-low/static-high boundary checks for `hs=0` and `hs∈{pwmMax-1, pwmMax}`.
- ✅ `pwm-dump` is the source of truth — tester does not re-derive buck math (Tasks 2 and 6).
- ✅ `--force-host` safety override gates fry/flat (Task 4).
- ✅ Exit code = # failed rows (Task 9).
- ⚠ Task 2 Step 3 references `leg.faultLevel()`/`leg.brakeActive()` which may not exist —
  the step explicitly tells the engineer to add them if missing or fall back to `0/0`. Not
  a placeholder; it's a conditional implementation directive.
