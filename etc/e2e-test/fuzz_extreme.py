#!/usr/bin/env python3
"""Extreme console fuzzer — spam wide command set with random arguments.

Goal: make the device panic. Commands are picked from a fixed set (the ones the
user enumerated) and arguments are sampled across NaN/Inf, signed ranges far
outside the firmware's documented limits, garbage tokens, and units of letters
the parser doesn't know. Pauses between commands are random and a `burst` mode
fires several commands back-to-back with no pause. Ack-waiting is rare by
default — the point is to flood the input parser, not to verify replies.

Run:
    ESPPORT=/dev/cu.usbmodem101 ./fuzz_extreme.py
    FUZZ_DURATION=300 FUZZ_ACK_PROB=0.02 ./fuzz_extreme.py

Env:
    FUZZ_SEED          random seed (unset → OS entropy)
    FUZZ_DURATION      seconds to fuzz (default 120)
    FUZZ_ACK_PROB      fraction of commands that wait for OK/ERR (default 0.05)
    FUZZ_PAUSE_MIN     min pause between commands  (default 0.005 s)
    FUZZ_PAUSE_MAX     max pause between commands  (default 0.30 s)
    FUZZ_BURST_PROB    chance of firing a no-pause burst  (default 0.10)
    FUZZ_BURST_MAX     max commands in a burst (default 12)
    FUZZ_SWEEP_SPEED   tracker.conf::sweep_speed value to set + reboot for faster
                       sweeps; default 20 (vs firmware default 4); set to 0/skip
                       to leave the on-flash value untouched
    FUZZ_POOL          which command set to sample from:
                         safe    — everything except known wedgers (default)
                         danger  — only known wedgers (adc-reset, adc-restart, ads-restart)
                         all     — both pools combined

Exit codes:
    0  device still alive at end
    1  could not reach device at start
    2  PANIC marker seen in streamed output
    3  device unresponsive to final `mem` probe
"""
import os
import random
import string
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from fugu.transport import SerialTransport
from fugu.console import Console

PORT = os.environ.get("ESPPORT", "/dev/cu.usbmodem101")
SEED_ENV = os.environ.get("FUZZ_SEED", "")
SEED = int(SEED_ENV) if SEED_ENV else None
DURATION = float(os.environ.get("FUZZ_DURATION", "120"))
ACK_PROB = float(os.environ.get("FUZZ_ACK_PROB", "0.05"))
PAUSE_MIN = float(os.environ.get("FUZZ_PAUSE_MIN", "0.005"))
PAUSE_MAX = float(os.environ.get("FUZZ_PAUSE_MAX", "0.30"))
BURST_PROB = float(os.environ.get("FUZZ_BURST_PROB", "0.10"))
BURST_MAX = int(os.environ.get("FUZZ_BURST_MAX", "12"))
SWEEP_SPEED = os.environ.get("FUZZ_SWEEP_SPEED", "20")
POOL = os.environ.get("FUZZ_POOL", "safe").lower()

PANIC_MARKERS = (
    "assert failed", "Backtrace:", "Guru Meditation", "abort()",
    "Rebooting...", "rst:0x", "CORRUPT HEAP", "Stack overflow",
    "LoadProhibited", "StoreProhibited", "IllegalInstruction",
    "Cache disabled", "panic'ed",
)


class PanicWatch:
    """on_line sink: keeps a rolling tail, flips `panicked` on any marker."""

    def __init__(self, depth=200):
        self.lines = []
        self.depth = depth
        self.panicked = False
        self.trigger = None

    def __call__(self, line):
        self.lines.append(line)
        if len(self.lines) > self.depth:
            self.lines.pop(0)
        if not self.panicked:
            for m in PANIC_MARKERS:
                if m in line:
                    self.panicked = True
                    self.trigger = m
                    break

    def dump(self):
        for l in self.lines:
            print(f"   | {l}")


# ---------------- random argument generators -------------------------------

NUM_BUCKETS = [
    lambda: float("nan"),
    lambda: float("inf"),
    lambda: float("-inf"),
    lambda: random.uniform(-1e9, 1e9),
    lambda: random.uniform(-100, 1000),
    lambda: random.choice([0, -0.0, 1, -1, 999, 1000, -1000, 9999, -9999, 2 ** 31 - 1, -2 ** 31]),
    lambda: random.uniform(0, 10),
    lambda: random.uniform(0, 999),
]


def rnd_num():
    v = random.choice(NUM_BUCKETS)()
    s = f"{v:g}" if isinstance(v, float) else str(v)
    if random.random() < 0.05:
        s += random.choice(["e", "e+", "e-1000", "x", "abc", "..."])
    return s


def rnd_word(maxlen=24):
    n = random.randint(0, maxlen)
    return "".join(random.choices(string.ascii_letters + string.digits + "._-,/+*= ", k=n))


KEYWORDS = (
    "on", "off", "forced", "0", "1", "list", "rs", "restart", "log",
    "info", "warn", "error",
    "mqtt", "tele", "ftp", "lcd", "scope", "telnet", "ble",
)


def rnd_token():
    bucket = random.random()
    if bucket < 0.5:
        return rnd_num()
    if bucket < 0.85:
        return random.choice(KEYWORDS)
    return rnd_word()


def gen_dc():
    parts = [rnd_num()]
    if random.random() < 0.3:
        parts.append(rnd_num())
    return "dc " + " ".join(parts)


def gen_step():
    sign = random.choice("+-")
    return sign + rnd_num()


def gen_sync():
    return "sync " + random.choice(["on", "off", "forced", "1", "0", rnd_token()])


def gen_bf():
    return "bf " + random.choice(["0", "1", rnd_token()])


def gen_svc():
    n = random.choice([1, 2, 3, 4])
    return "svc " + " ".join(rnd_token() for _ in range(n))


def gen_wifi():
    sub = random.choice(["on", "off", rnd_token()])
    if sub == "off" and random.random() < 0.5:
        return f"wifi off {random.randint(0, 10000)}"
    return "wifi " + sub


# (name_for_stats, generator)
GEN_SAFE = [
    ("sensor", lambda: random.choice(["sensor", "sensor avg", "sensor " + rnd_token()])),
    ("mem", lambda: "mem"),
    ("uptime", lambda: "uptime"),
    ("rt-stats", lambda: "rt-stats"),
    ("reset-lag", lambda: "reset-lag"),
    ("scan-i2c", lambda: "scan-i2c"),
    ("sweep", lambda: "sweep"),
    ("mppt", lambda: "mppt"),
    ("short-ls", lambda: "short-ls"),
    ("vset", lambda: "vset " + rnd_num()),
    ("iset", lambda: "iset " + rnd_num()),
    ("speed", lambda: "speed " + rnd_num()),
    ("dc", gen_dc),
    ("step", gen_step),
    ("sync", gen_sync),
    ("bf", gen_bf),
    ("svc", gen_svc),
    ("wifi", gen_wifi),
]

# Known wedgers — repeatedly hammering these blocks the RT loop in adcSampler.update()
# (samples never arrive again) and the TWDT eventually reboots the device. Kept in a
# separate pool so a "safe" run can probe everything else without short-circuiting.
GEN_DANGER = [
    ("ads-restart", lambda: random.choice(["ads-restart", "adc-restart"])),
    ("adc-reset", lambda: "adc-reset"),
]

if POOL == "danger":
    GEN = GEN_DANGER
elif POOL == "all":
    GEN = GEN_SAFE + GEN_DANGER
else:
    if POOL != "safe":
        print(f"warn: unknown FUZZ_POOL={POOL!r}, defaulting to 'safe'")
    GEN = GEN_SAFE


def speed_up_sweeps(c, watch):
    """Bump tracker.conf::sweep_speed and reboot so each sweep finishes quicker.

    `sweep_speed` is the per-tick PWM-step cap (mppt.cpp:162) — read once in
    MpptController::begin so a reboot is needed to apply. Higher value → larger
    ramp per ADC tick → sweep reaches a limiting controller sooner. Default 4.
    """
    if SWEEP_SPEED in ("0", "skip", ""):
        print("sweep_speed: leaving on-flash value untouched")
        return True
    val = SWEEP_SPEED
    print(f"sweep_speed: set tracker.conf sweep_speed = {val}, then reboot")
    r = c.command(f"set-config tracker.conf sweep_speed {val}", timeout=4.0)
    if r.timed_out or r.rejected:
        print(f"  !! set-config failed  (timed_out={r.timed_out}, rejected={r.rejected})")
        return False
    try:
        c.write("restart\r\n")
    except Exception as e:
        print(f"  !! restart write failed: {e}")
        return False
    # USB-CDC re-enumerates on reboot; the reader thread's read() raises and exits.
    # Close + reopen the transport so it comes back, then probe.
    time.sleep(2.5)
    try:
        c.reconnect()
    except Exception as e:
        print(f"  !! reconnect failed: {e}")
        return False
    watch.lines.clear()
    watch.panicked = False
    watch.trigger = None
    if not c.wait_ready(probe="mem", timeout=20):
        print("  !! device did not come back after reboot")
        return False
    print("  ready after reboot")
    return True


def main():
    random.seed(SEED)
    seed_repr = SEED if SEED is not None else "os-entropy"
    print(f"connecting to {PORT}  (seed={seed_repr}, duration={DURATION:.0f}s, "
          f"ack_prob={ACK_PROB:.2f}, pause=[{PAUSE_MIN:.3f},{PAUSE_MAX:.2f}]s, "
          f"pool={POOL} [{len(GEN)} cmds])")
    t = SerialTransport(PORT, baud=115200)
    watch = PanicWatch()
    c = Console(t, eol="\r\n", on_line=watch)
    if not c.wait_ready(probe="mem", timeout=15):
        print("device not responding at start")
        c.close()
        return 1

    if not speed_up_sweeps(c, watch):
        print("sweep-speed setup failed; continuing with on-flash value")

    sent, acked = 0, 0
    paniced_after = None
    per_cmd = {}
    last_cmd = None
    start = time.monotonic()

    def fire(cmd):
        nonlocal sent
        c.write(cmd + "\r\n")
        sent += 1

    try:
        while time.monotonic() - start < DURATION:
            if watch.panicked:
                paniced_after = sent
                break

            name, gen = random.choice(GEN)
            try:
                cmd = gen()
            except Exception as e:
                print(f"gen({name}) raised: {e}")
                continue
            per_cmd[name] = per_cmd.get(name, 0) + 1
            last_cmd = cmd

            if random.random() < ACK_PROB:
                r = c.command(cmd, timeout=2.0)
                acked += 1
                sent += 1
                if r.timed_out:
                    print(f"  !! ack timeout after `{cmd}`")
                    time.sleep(0.5)  # let any pending panic dump drain
                    if watch.panicked:
                        paniced_after = sent
                        break
            else:
                try:
                    fire(cmd)
                except Exception as e:
                    print(f"write failed: {e}  (cmd={cmd!r})")
                    time.sleep(0.5)
                    continue

            if random.random() < BURST_PROB:
                burst = random.randint(2, BURST_MAX)
                for _ in range(burst):
                    if watch.panicked or time.monotonic() - start >= DURATION:
                        break
                    name2, gen2 = random.choice(GEN)
                    per_cmd[name2] = per_cmd.get(name2, 0) + 1
                    try:
                        cmd2 = gen2()
                        last_cmd = cmd2
                        fire(cmd2)
                    except Exception:
                        break
                continue

            time.sleep(random.uniform(PAUSE_MIN, PAUSE_MAX))
    except KeyboardInterrupt:
        print("\n^C — stopping")

    elapsed = time.monotonic() - start
    rate = sent / elapsed if elapsed > 0 else 0
    print(f"\nsent {sent} commands ({acked} with ack) in {elapsed:.1f}s — {rate:.1f} cmd/s")
    for k, n in sorted(per_cmd.items(), key=lambda kv: -kv[1])[:20]:
        print(f"  {k:<12} {n}")

    if watch.panicked:
        print(f"\n!! PANIC after ~{paniced_after} commands  "
              f"(marker={watch.trigger!r}, last cmd={last_cmd!r})")
        watch.dump()
        c.close()
        return 2

    print("\nfinal probe…")
    time.sleep(1.0)
    r = c.command("mem", timeout=5.0)
    if r.timed_out or not r.ok:
        # transport may have died mid-fuzz on a `restart` or USB hiccup; reconnect once
        # and retry so we can tell harness-side drop apart from a real device hang.
        print(f"!! first probe missed (timed_out={r.timed_out}, ok={r.ok}); reconnecting…")
        try:
            c.reconnect()
        except Exception as e:
            print(f"  reconnect failed: {e}")
        time.sleep(1.0)
        r = c.command("mem", timeout=5.0)
        if r.timed_out or not r.ok:
            print(f"!! final probe failed after reconnect  (last cmd={last_cmd!r})")
            watch.dump()
            c.close()
            return 3
        print("  recovered after reconnect (transport had dropped)")

    print("device alive after fuzz")
    c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
