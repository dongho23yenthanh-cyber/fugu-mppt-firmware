"""Shared helpers for the host-side e2e tests in this directory.

Consolidates the PASS/FAIL bookkeeping, the poll-until-true loop, the AP-restart webhook, the
console-line event log (status/connect/boot parsing), and the panic recorder that were previously
copy-pasted across test_*.py / fuzz_*.py. No behaviour change — each helper is the superset of the
local copies it replaces.
"""
import re
import threading
import time
import urllib.request

# --- log-line patterns shared by the wifi tests -----------------------------------------------
# telemetry.cpp: "Connected to WiFi <ssid>, RSSI <r> IP <ip>"
RE_CONNECT = re.compile(r"Connected to WiFi (.+?), RSSI (-?\d+) IP (\S+)")
# streaming status line: "... N=<samples> ... rssi=<dBm>"
RE_STATUS_N_RSSI = re.compile(r"\bN=(\d+)\b.*?\brssi=(-?\d+)")
BOOT_MARKER = "setup() done"

# Panic / reboot markers seen on the serial log (used by the fuzzers and reboot detection).
PANIC_MARKERS = ("assert failed", "Backtrace:", "Guru Meditation",
                 "abort()", "Rebooting...", "rst:0x")


class Results:
    """PASS/FAIL/SKIP bookkeeping with one-line console output. `ok()` is True iff no check failed."""

    def __init__(self):
        self.items = []  # (name, ok)

    def check(self, name, ok, detail=""):
        ok = bool(ok)
        self.items.append((name, ok))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""), flush=True)
        return ok

    def skip(self, name, detail=""):
        print(f"  [SKIP] {name}" + (f"  ({detail})" if detail else ""), flush=True)

    def ok(self):
        return bool(self.items) and all(ok for _, ok in self.items)


def wait_for(predicate, timeout, poll=0.2):
    """Poll `predicate` until it returns a truthy value (returned) or `timeout` elapses (None)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        v = predicate()
        if v:
            return v
        time.sleep(poll)
    return None


wait_until = wait_for  # alias (svc_recovery used this name)


def fire_webhook(url, method="GET", timeout=10.0):
    """Fire an HTTP request (the AP/router restart webhook). Returns (status, first 200 bytes)."""
    req = urllib.request.Request(url, method=method.upper())
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read(200).decode("utf-8", "replace")


class EventLog:
    """Thread-safe, timestamped log of parsed console events plus a ring of raw lines.

    Subclass and override `feed(line)` (call `super().feed(line)` first, then `self.add(kind,
    payload)` for each parsed event). Queries filter by kind and a `since` monotonic timestamp.
    """

    def __init__(self, raw_max=400):
        self._lock = threading.Lock()
        self.ev = []   # (t, kind, payload)
        self.raw = []  # (t, line)
        self._raw_max = raw_max

    def feed(self, line):
        t = time.monotonic()
        with self._lock:
            self.raw.append((t, line))
            if len(self.raw) > self._raw_max:
                self.raw.pop(0)

    def add(self, kind, payload=None):
        with self._lock:
            self.ev.append((time.monotonic(), kind, payload))

    def _sel(self, kind, since):
        with self._lock:
            return [(t, p) for (t, k, p) in self.ev if k == kind and t >= since]

    def last(self, kind, since=0.0):
        e = self._sel(kind, since)
        return e[-1][1] if e else None

    def all(self, kind, since=0.0):
        return [p for _, p in self._sel(kind, since)]

    def events(self, kind, since=0.0):
        return self._sel(kind, since)

    def first_t(self, kind, since=0.0):
        e = self._sel(kind, since)
        return e[0][0] if e else None

    def saw(self, kind, since=0.0):
        return bool(self._sel(kind, since))

    def raw_since(self, since):
        with self._lock:
            return [l for (t, l) in self.raw if t >= since]


class Recorder:
    """Tap for Console.on_line that keeps the last `depth` lines and trips `panic` on a marker."""

    def __init__(self, depth=80, markers=PANIC_MARKERS):
        self.lines = []
        self.depth = depth
        self.markers = markers
        self.panic = False

    def __call__(self, line):
        self.lines.append(line)
        if len(self.lines) > self.depth:
            self.lines.pop(0)
        if any(m in line for m in self.markers):
            self.panic = True

    def tail(self, n=40):
        return self.lines[-n:]
