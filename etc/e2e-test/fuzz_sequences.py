#!/usr/bin/env python3
"""Sequence fuzzer — run named command sequences and watch for panic markers.

Merges the old fuzz_fugu (a corpus of sweep / vconv / dc sequences) and fuzz_replay (hammer one
known-bad sequence many times). Each sequence is a list of (cmd, hold_s); after every command we
check the panic recorder, and after each sequence we probe that the device still answers. The
``heap`` sequence (vconv r_bat -> dc -> dc -> sweep) is the TLSF heap-assert repro — heap-layout
sensitive, so hammer it with ``--repeat``. Exits 2 if a panic/hang is observed, else 0.

Run:
    ESPPORT=/dev/cu.usbmodem101 ./fuzz_sequences.py              # whole corpus once
    ./fuzz_sequences.py --seq heap --repeat 20                   # hammer the heap repro
    ./fuzz_sequences.py --port /dev/cu.usbmodem101 --seed 7      # vary the random sequences
"""
import argparse
import os
import random
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)            # _harness
sys.path.insert(0, os.path.dirname(HERE))  # repo/etc -> fugu pkg
from fugu.transport import SerialTransport
from fugu.console import Console
from _harness import Recorder, PANIC_MARKERS

# the heap-assert repro also trips on the TLSF allocator's own asserts
HEAP_MARKERS = PANIC_MARKERS + ("tlsf", "block_locate_free")


def build_corpus(rng):
    """Return [(name, [(cmd, hold_s), ...])]. Random sequences are drawn from `rng` (seeded)."""
    seqs = []
    # the original panicking sequence (TLSF heap assert) — the prime hammer target
    seqs.append(("heap", [("vconv set r_bat 1", 0.3), ("dc 1", 1.0), ("dc 100", 1.0), ("sweep", 6.0)]))
    # vary duty before a sweep
    for dc in (0, 1, 5, 10, 50, 100, 200, 500):
        seqs.append((f"dc{dc}-sweep", [(f"dc {dc}", 1.0), ("sweep", 6.0)]))
    # vary vconv params then sweep
    for i, pre in enumerate([
        "vconv set r_bat 1", "vconv set r_bat 0.05", "vconv set r_bat 10",
        "vconv bat 13.5", "vconv bat 50", "vconv bat open", "vconv bat short",
        "vconv pv 8 80", "vconv pv 12 100 0.5", "vconv set c_in 100e-6",
        "vconv set c_out 470e-6", "vconv set l 50e-6",
        "vconv set vbat_ac_amp 0.5", "vconv set vbat_ac_freq 100",
    ]):
        seqs.append((f"vconv-{i}", [(pre, 0.3), ("sweep", 6.0)]))
    # back-to-back and interrupted sweeps
    seqs.append(("double-sweep", [("sweep", 6.0), ("sweep", 6.0)]))
    seqs.append(("triple-sweep", [("sweep", 6.0), ("sweep", 6.0), ("sweep", 6.0)]))
    seqs.append(("sweep-then-dc", [("sweep", 1.0), ("dc 50", 1.0), ("sweep", 6.0)]))
    # dc cycling then sweep
    seqs.append(("dc-cycling", [(f"dc {rng.randint(0, 500)}", 0.3) for _ in range(10)] + [("sweep", 6.0)]))
    # random interleaved fuzz
    pool = ["vconv", "vconv set r_bat {}", "vconv bat {}", "vconv pv {} {}", "dc {}", "+10", "-10",
            "sync on", "sync off", "bf 0", "bf 1", "fan {}", "mppt", "sweep", "rt-stats", "sensor", "mem", "hn"]
    for trial in range(20):
        seq = []
        for _ in range(rng.randint(3, 8)):
            tmpl = rng.choice(pool)
            cmd = tmpl.format(*[str(round(rng.uniform(0.01, 100), 3)) for _ in range(tmpl.count("{}"))])
            seq.append((cmd, 0.4))
        seqs.append((f"random-{trial}", seq))
    return seqs


def reset_state(c):
    """Best-effort: stop converter, return to MPPT control."""
    c.command("dc 0", timeout=2.0)
    time.sleep(0.2)
    c.command("mppt", timeout=2.0)
    time.sleep(0.2)


def run_sequence(c, rec, name, cmds, probe="hn"):
    print(f"\n[seq] {name}", flush=True)
    rec.lines.clear()
    rec.panic = False
    for cmd, hold in cmds:
        print(f"  > {cmd}", flush=True)
        r = c.command(cmd, timeout=10.0)
        if r.timed_out:
            print("  !! timeout — possible hang")
        elif r.rejected:
            print("  -- rejected")
        time.sleep(hold)
        if rec.panic:
            print(f"  !! PANIC after `{cmd}`")
            for l in rec.tail(60):
                print(f"     | {l}")
            return False
    p = c.command(probe, timeout=4.0)
    if p.timed_out or not p.ok:
        if rec.panic:
            print(f"  !! POST-PROBE PANIC after seq `{name}`")
            for l in rec.tail(60):
                print(f"     | {l}")
        else:
            print(f"  ?? probe didn't ack (timed_out={p.timed_out}, ok={p.ok})")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("ESPPORT", "/dev/cu.usbmodem101"))
    ap.add_argument("--seq", help="only run sequences whose name starts with this (e.g. 'heap', 'vconv')")
    ap.add_argument("--repeat", type=int, default=1, help="run the selected set this many times (hammer)")
    ap.add_argument("--seed", type=int, default=int(os.environ.get("FUZZ_SEED", "42")))
    args = ap.parse_args()

    corpus = build_corpus(random.Random(args.seed))
    if args.seq:
        corpus = [(n, s) for n, s in corpus if n.startswith(args.seq)]
        if not corpus:
            print(f"no sequence matches --seq {args.seq!r}")
            return 1

    print(f"connecting to {args.port}", flush=True)
    rec = Recorder(depth=200, markers=HEAP_MARKERS)
    c = Console(SerialTransport(args.port, baud=115200), eol="\r\n", on_line=rec)
    if not c.wait_ready(probe="hn", timeout=15):
        print("device not responding")
        return 1
    reset_state(c)

    try:
        for it in range(1, args.repeat + 1):
            if args.repeat > 1:
                print(f"\n===== pass {it}/{args.repeat} =====", flush=True)
            for name, seq in corpus:
                if not run_sequence(c, rec, name, seq):
                    print(f"\nstopping: panic/hang in `{name}`")
                    return 2
                reset_state(c)
    finally:
        c.close()
    print("\nfuzz done — no panic observed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
