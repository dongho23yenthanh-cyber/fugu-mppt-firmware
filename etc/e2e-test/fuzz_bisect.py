#!/usr/bin/env python3
"""Bisect-runner: pair adc-reset/ads-restart with subsets of the safe pool
to find which command interaction wedges the RT loop.

Each round: ~90 s of fuzz with one partner subset; between rounds we check the
device is alive. First round that yields exit 2 (panic marker) or 3 (final
probe failed) or an unrecoverable timeout points at the racing partner.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fugu.transport import SerialTransport
from fugu.console import Console

PORT = os.environ.get("ESPPORT", "/dev/cu.usbmodem101")
HERE = os.path.dirname(os.path.abspath(__file__))
FUZZ = os.path.join(HERE, "fuzz_extreme.py")

ROUNDS = [
    ("pure-danger",  "adc-reset,ads-restart"),
    ("sweep-only",   "adc-reset,ads-restart,sweep"),
    ("mppt-only",    "adc-reset,ads-restart,mppt"),
    ("rt-control",   "adc-reset,ads-restart,sweep,mppt"),
    ("services",     "adc-reset,ads-restart,svc,wifi"),
    ("manual-pwm",   "adc-reset,ads-restart,dc,sync,bf,short-ls,step"),
    ("setpoints",    "adc-reset,ads-restart,vset,iset,speed"),
    ("info-only",    "adc-reset,ads-restart,sensor,mem,uptime,rt-stats,reset-lag,scan-i2c"),
]
DURATION = int(os.environ.get("BISECT_DUR", "90"))
ONLY = os.environ.get("BISECT_ONLY", "").strip()
if ONLY:
    wanted = {s.strip() for s in ONLY.split(",") if s.strip()}
    ROUNDS = [r for r in ROUNDS if r[0] in wanted]
    if not ROUNDS:
        print(f"error: BISECT_ONLY={ONLY!r} matched none of the rounds")
        sys.exit(1)


def check_alive(c):
    r = c.command("mem", timeout=4.0)
    return r.ok and not r.timed_out


def main():
    t = SerialTransport(PORT, baud=115200)
    c = Console(t, eol="\r\n")
    if not c.wait_ready(probe="mem", timeout=10):
        print("device not responding at start"); return 1
    print(f"-- device alive, starting bisect ({len(ROUNDS)} rounds × {DURATION}s) --\n")
    c.close()
    time.sleep(0.5)

    findings = []
    for i, (name, cmds) in enumerate(ROUNDS, 1):
        print(f"\n=== round {i}/{len(ROUNDS)}: {name}  [{cmds}] ===")
        env = os.environ.copy()
        env["ESPPORT"] = PORT
        env["FUZZ_DURATION"] = str(DURATION)
        env["FUZZ_SEED"] = str(1000 + i)
        env["FUZZ_CMDS"] = cmds
        env["FUZZ_SWEEP_SPEED"] = "skip"  # leave on-flash; saves a reboot per round
        log = f"/tmp/fuzz_bisect_{name}.log"
        with open(log, "wb") as f:
            rc = subprocess.call([sys.executable, FUZZ], env=env, stdout=f, stderr=subprocess.STDOUT)
        # parse summary from the log
        with open(log) as f:
            text = f.read()
        sent = "?"
        for ln in text.splitlines():
            if ln.startswith("sent "):
                sent = ln.split(" in ")[0].replace("sent ", "")
                break
        verdict = {0: "OK", 1: "no-start", 2: "PANIC", 3: "wedged"}.get(rc, f"rc={rc}")
        print(f"  → {verdict}  ({sent})  log={log}")
        findings.append((name, rc, verdict, sent, cmds))
        # Between rounds: a `PANIC`/`wedged` exit usually means the TWDT triggered
        # systemRestart() and the device cleanly rebooted. Try to reconnect; if it
        # comes back we continue the bisect. Only stop when the device stays dead.
        time.sleep(2.0)
        t2 = SerialTransport(PORT, baud=115200)
        c2 = Console(t2, eol="\r\n")
        recovered = c2.wait_ready(probe="mem", timeout=15)
        c2.close()
        if not recovered:
            print("  !! device did not come back — stopping bisect")
            break
        if rc != 0:
            print("  device recovered after soft reboot; continuing bisect")

    print("\n\n========= bisect summary =========")
    for name, rc, verdict, sent, cmds in findings:
        print(f"  {name:<12}  {verdict:<18} {sent:<32}  ({cmds})")
    return 0 if all(rc == 0 for _, rc, _, _, _ in findings) else 1


if __name__ == "__main__":
    sys.exit(main())
