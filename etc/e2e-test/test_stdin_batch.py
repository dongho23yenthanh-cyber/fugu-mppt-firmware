#!/usr/bin/env python3
"""E2E test for `fugu_console.py --stdin` (and the auto-batch fallback).

Unlike the other tests here — which drive the `fugu` package directly — this one exercises the CLI
wrapper itself: it spawns `etc/fugu_console.py` as a subprocess, pipes a batch of read-only commands
on stdin, and asserts each reply comes back tagged `=== <cmd> ===`, in order, over a single
connection. Two modes are checked:

  * explicit `--stdin`
  * auto-batch — no mode flag, stdin not a terminal (a pipe) → the CLI batches stdin instead of
    opening the REPL. This is the agent/script-facing path.

All commands are read-only (`hostname`, `mem`, `svc list`) so the test is safe to point at a live
converter as well as a bench unit; nothing touches the half-bridge. A `#` comment and a blank line
are included to verify they're skipped (the tag list must be exactly the three real commands).

Usage
-----
    python etc/e2e-test/test_stdin_batch.py --serial /dev/cu.usbmodem1201
    python etc/e2e-test/test_stdin_batch.py --telnet 192.168.4.2:23
"""
import argparse
import os
import re
import subprocess
import sys

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc
sys.path.insert(0, ETC_DIR)
from _harness import Results

FUGU_CONSOLE = os.path.join(ETC_DIR, "fugu_console.py")

# blank line + a comment in the middle: both must be dropped, leaving exactly three tagged commands.
CMDS = "hostname\nmem\n# comment line — must be skipped\n\nsvc list\n"
EXPECT_LABELS = ["hostname", "mem", "svc list"]
# (start tag, next tag or None, reply substring the firmware always emits for that command)
REPLY_MARKERS = [
    ("=== hostname ===", "=== mem ===",      "Hostname:"),
    ("=== mem ===",      "=== svc list ===", "Free heap"),
    ("=== svc list ===", None,               "NAME"),
]


def _region(out, start, end):
    i = out.find(start)
    if i < 0:
        return ""
    j = out.find(end, i + len(start)) if end else len(out)
    return out[i: j if j >= 0 else len(out)]


def run_and_check(res, transport_argv, use_stdin, timeout):
    mode = "--stdin" if use_stdin else "auto-batch (piped, no flag)"
    argv = [sys.executable, FUGU_CONSOLE] + transport_argv + (["--stdin"] if use_stdin else [])
    try:
        p = subprocess.run(argv, input=CMDS, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        res.check(f"[{mode}] completed", False, f"timed out after {timeout}s")
        return
    out = p.stdout

    res.check(f"[{mode}] exit code 0", p.returncode == 0, f"rc={p.returncode}; stderr={p.stderr.strip()[:120]}")

    labels = re.findall(r"(?m)^=== (.+?) ===\s*$", out)
    # exact list proves: all three ran, in order, and the comment + blank line were skipped
    res.check(f"[{mode}] commands tagged in order, comment skipped",
              labels == EXPECT_LABELS, f"tags={labels}")

    for start, end, marker in REPLY_MARKERS:
        cmd = start.strip("= ").strip()
        res.check(f"[{mode}] {cmd!r} reply present ({marker!r})",
                  marker in _region(out, start, end))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--serial", metavar="DEV", help="console over serial")
    g.add_argument("--telnet", metavar="HOST[:PORT]", help="console over TCP/telnet")
    g.add_argument("--ble", action="store_true", help="console over BLE NUS")
    ap.add_argument("--address", help="BLE address (with --ble)")
    ap.add_argument("--name", help="BLE name substring (with --ble)")
    ap.add_argument("--timeout", type=float, default=60.0, help="per-invocation wall-clock ceiling")
    args = ap.parse_args()

    if args.serial:
        transport_argv = ["-p", args.serial]
        print(f"control=serial {args.serial}")
    elif args.ble:
        transport_argv = ["--ble"] + (["--address", args.address] if args.address else []) \
            + (["--name", args.name] if args.name else [])
        print(f"control=ble {args.address or args.name or '(scan)'}")
    else:
        transport_argv = ["--ip", args.telnet]
        print(f"control=telnet {args.telnet}")

    res = Results()
    run_and_check(res, transport_argv, use_stdin=True, timeout=args.timeout)
    run_and_check(res, transport_argv, use_stdin=False, timeout=args.timeout)

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
