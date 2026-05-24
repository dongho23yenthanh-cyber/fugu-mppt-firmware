#!/usr/bin/env python3
"""Fuzz the fugu firmware over serial to try to reproduce the sweep-plot crash.

Each test case is a sequence of commands. After each sequence we send a probe
(`hn`) and verify the device responded — a missing/garbage reply implies a panic.
On panic we capture the recent output, reset state, and continue.
"""
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from fugu.transport import SerialTransport
from fugu.console import Console

PORT = os.environ.get("ESPPORT", "/dev/cu.usbmodem101")
SEED = int(os.environ.get("FUZZ_SEED", "42"))

PANIC_MARKERS = ("assert failed", "Backtrace:", "Guru Meditation",
                 "abort()", "Rebooting...", "rst:0x")


class Recorder:
    def __init__(self, depth=80):
        self.lines = []
        self.depth = depth

    def __call__(self, line):
        self.lines.append(line)
        if len(self.lines) > self.depth:
            self.lines.pop(0)

    def snapshot(self):
        return list(self.lines)

    def has_panic(self):
        return any(any(m in l for m in PANIC_MARKERS) for l in self.lines)


def make_console():
    t = SerialTransport(PORT, baud=115200)
    rec = Recorder()
    c = Console(t, eol="\r\n", on_line=rec)
    return c, rec


def reset_state(c):
    """Best-effort: stop converter, return to MPPT control."""
    c.command("dc 0", timeout=2.0)
    time.sleep(0.2)
    c.command("mppt", timeout=2.0)
    time.sleep(0.2)


def run_sequence(c, rec, name, cmds, *, settle=0.3, probe="hn"):
    print(f"\n[seq] {name}")
    rec.lines.clear()
    for cmd in cmds:
        if isinstance(cmd, tuple):
            cmd, hold = cmd
        else:
            hold = settle
        print(f"  > {cmd}")
        r = c.command(cmd, timeout=8.0)
        if r.timed_out:
            print(f"  !! timeout — possible panic")
        elif r.rejected:
            print(f"  -- rejected")
        time.sleep(hold)
        if rec.has_panic():
            print(f"  !! PANIC detected after `{cmd}`")
            for l in rec.snapshot():
                print(f"     | {l}")
            return False
    # post-probe to make sure device is still alive
    p = c.command(probe, timeout=4.0)
    if p.timed_out or not p.ok:
        if rec.has_panic():
            print(f"  !! POST-PROBE PANIC after seq `{name}`")
            for l in rec.snapshot():
                print(f"     | {l}")
            return False
        print(f"  ?? probe didn't ack (timed_out={p.timed_out}, ok={p.ok})")
        return False
    return True


def main():
    random.seed(SEED)
    print(f"connecting to {PORT}")
    c, rec = make_console()
    if not c.wait_ready(probe="hn", timeout=10):
        print("device not responding")
        return 1
    reset_state(c)

    # ----- exact replay of the panicking sequence ---------------------------
    replay = [
        ("vconv set r_bat 1", 0.3),
        ("dc 1", 1.0),
        ("dc 100", 1.0),
        ("sweep", 6.0),
    ]
    run_sequence(c, rec, "user-replay", replay)
    reset_state(c)

    # ----- vary the duty cycle before sweep ---------------------------------
    for dc in (0, 1, 5, 10, 50, 100, 200, 500):
        run_sequence(c, rec, f"dc{dc}-sweep", [
            (f"dc {dc}", 1.0),
            ("sweep", 6.0),
        ])
        reset_state(c)

    # ----- vary vconv params, then sweep ------------------------------------
    vconv_sets = [
        [("vconv set r_bat 1", 0.3)],
        [("vconv set r_bat 0.05", 0.3)],
        [("vconv set r_bat 10", 0.3)],
        [("vconv bat 13.5", 0.3)],
        [("vconv bat 50", 0.3)],
        [("vconv bat open", 0.3)],
        [("vconv bat short", 0.3)],
        [("vconv pv 8 80", 0.3)],
        [("vconv pv 12 100 0.5", 0.3)],
        [("vconv set c_in 100e-6", 0.3)],
        [("vconv set c_out 470e-6", 0.3)],
        [("vconv set l 50e-6", 0.3)],
        [("vconv set vbat_ac_amp 0.5", 0.3)],
        [("vconv set vbat_ac_freq 100", 0.3)],
    ]
    for i, seq in enumerate(vconv_sets):
        run_sequence(c, rec, f"vconv-{i}", seq + [("sweep", 6.0)])
        reset_state(c)

    # ----- back-to-back sweeps ----------------------------------------------
    run_sequence(c, rec, "double-sweep", [
        ("sweep", 6.0),
        ("sweep", 6.0),
    ])
    reset_state(c)

    run_sequence(c, rec, "triple-sweep", [
        ("sweep", 6.0),
        ("sweep", 6.0),
        ("sweep", 6.0),
    ])
    reset_state(c)

    # ----- sweep interrupted by dc ------------------------------------------
    run_sequence(c, rec, "sweep-then-dc", [
        ("sweep", 1.0),
        ("dc 50", 1.0),
        ("sweep", 6.0),
    ])
    reset_state(c)

    # ----- dc cycling + sweep -----------------------------------------------
    seq = []
    for _ in range(10):
        seq.append((f"dc {random.randint(0, 500)}", 0.3))
    seq.append(("sweep", 6.0))
    run_sequence(c, rec, "dc-cycling", seq)
    reset_state(c)

    # ----- random fuzz: random params interleaved ---------------------------
    cmds_pool = [
        "vconv",
        "vconv set r_bat {}",
        "vconv bat {}",
        "vconv pv {} {}",
        "dc {}",
        "+10",
        "-10",
        "sync on",
        "sync off",
        "bf 0",
        "bf 1",
        "fan {}",
        "mppt",
        "sweep",
        "rt-stats",
        "sensor",
        "mem",
        "hn",
    ]
    for trial in range(20):
        seq = []
        for _ in range(random.randint(3, 8)):
            tmpl = random.choice(cmds_pool)
            if "{}" in tmpl:
                args = [str(round(random.uniform(0.01, 100), 3)) for _ in range(tmpl.count("{}"))]
                cmd = tmpl.format(*args)
            else:
                cmd = tmpl
            seq.append((cmd, 0.4))
        run_sequence(c, rec, f"random-{trial}", seq)
        reset_state(c)

    print("\nfuzz done")
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
