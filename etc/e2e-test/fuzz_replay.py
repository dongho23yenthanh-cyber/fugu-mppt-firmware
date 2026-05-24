#!/usr/bin/env python3
"""Hammer the original panicking sequence to see if it's reproducible.

The original sequence (vconv set r_bat 1 → dc 1 → dc 100 → sweep) panicked
once with a TLSF heap assert. These bugs are typically heap-layout sensitive,
so we run it many times and watch for the panic markers.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fugu.transport import SerialTransport
from fugu.console import Console

PORT = os.environ.get("ESPPORT", "/dev/cu.usbmodem101")
PANIC_MARKERS = ("assert failed", "Backtrace:", "Guru Meditation",
                 "abort()", "Rebooting...", "rst:0x", "tlsf", "block_locate_free")


class Recorder:
    def __init__(self, depth=200):
        self.lines = []
        self.depth = depth
        self.panic = False

    def __call__(self, line):
        self.lines.append(line)
        if len(self.lines) > self.depth:
            self.lines.pop(0)
        if any(m in line for m in PANIC_MARKERS):
            self.panic = True
            print(f"  !! PANIC line: {line}")

    def clear(self):
        self.lines.clear()


def main():
    t = SerialTransport(PORT, baud=115200)
    rec = Recorder()
    c = Console(t, eol="\r\n", on_line=rec)
    if not c.wait_ready(probe="hn", timeout=15):
        print("device not responding")
        return 1

    iterations = int(os.environ.get("ITER", "10"))
    print(f"running {iterations} iterations")

    for i in range(iterations):
        print(f"\n=== iter {i+1}/{iterations} ===")
        rec.clear()
        rec.panic = False

        # reset to MPPT
        c.command("mppt", timeout=4.0)
        time.sleep(0.3)

        for cmd, hold in [
            ("vconv set r_bat 1", 0.3),
            ("dc 1", 1.0),
            ("dc 100", 1.0),
            ("sweep", 8.0),
        ]:
            print(f"  > {cmd}")
            r = c.command(cmd, timeout=10.0)
            time.sleep(hold)
            if rec.panic:
                print(f"  !! PANIC TRIGGERED after `{cmd}`")
                print("  --- recent lines ---")
                for l in rec.lines:
                    print(f"  | {l}")
                c.close()
                return 0
            if r.timed_out:
                print(f"  !! timeout — device may have hung")
                # snapshot
                time.sleep(2.0)
                if rec.panic:
                    print("  !! PANIC after timeout")
                    for l in rec.lines:
                        print(f"  | {l}")
                    c.close()
                    return 0

    print("\nno panic observed across all iterations")
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
