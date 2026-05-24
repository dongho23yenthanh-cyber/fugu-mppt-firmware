#!/usr/bin/env python3
"""E2E test for the *stick-to-wifi* behaviour: when the AP the device is on reboots,
the device must **ride through the outage without rebooting** and **reconnect to the same
AP** (instead of crashing and roaming onto a weaker network).

This is the regression test for the bug fixed on `scope-client`: losing the AP overflowed
mqtt_task and panicked the device, which then booted onto whatever AP was strongest while
its own was still down and never roamed back.

How it works
------------
A control console (serial preferred) taps the device's streaming status line and logs:

  * ``N=<nSamples>`` — the per-boot ADC sample counter. It only ever climbs; if it drops
    back to ~0 (or a ``setup() done.`` banner appears) the device **rebooted** → FAIL.
  * ``rssi=`` — link state. Goes to 0 when the station drops, recovers when reassociated.
  * ``Connected to WiFi <ssid>, RSSI .. IP ..`` — names the AP it (re)joined.

The test then hits a **configurable webhook** that restarts the AP/router, and watches:

  1. an outage actually happened (sanity — the webhook took the link down);
  2. ``N`` stayed continuous and no boot banner appeared (no reboot);
  3. the device reconnected within the timeout;
  4. it reconnected to ``--ssid`` (the AP that was restarted) — the *stick* assertion.

**Use serial** (``--serial``): the status line streams over UART regardless of WiFi, so the
reboot/outage signals stay visible the whole time. Telnet/BLE work too, but if the control
path runs *through* the restarted AP it goes dark during the outage and reboot detection has
a blind spot — only use a network control channel that does not depend on the AP under test.

Usage
-----
    python etc/e2e-test/test_stick_to_wifi.py --serial /dev/cu.usbmodem1201 \
        --restart-url http://192.168.1.173/scan --ssid pwr-statione
    python etc/e2e-test/test_stick_to_wifi.py --telnet 192.168.1.50:23 \
        --restart-url http://192.168.1.173/scan --ssid pwr-statione --rounds 3
"""
import argparse
import os
import re
import sys
import threading
import time
import urllib.request

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc (holds the fugu pkg)
sys.path.insert(0, ETC_DIR)
from fugu.transport import SocketTransport, SerialTransport
from fugu.console import Console

# status line:  ... N=<samples> rssi=<dBm>
_STATUS = re.compile(r"\bN=(\d+)\b.*?\brssi=(-?\d+)")
# telemetry.cpp: "Connected to WiFi <ssid>, RSSI <r> IP <ip>"
_CONNECT = re.compile(r"Connected to WiFi (.+?), RSSI (-?\d+) IP (\S+)")
_BOOT = "setup() done"
REBOOT_MARGIN = 100  # N may jitter by a sample or two between reads; a real reboot drops it to ~0


class Tap:
    """Records and parses the device's streamed console lines (via Console.on_line)."""

    def __init__(self):
        self._lock = threading.Lock()
        self.events = []  # (t, kind, payload): ("N", n), ("rssi", r), ("connect", ssid), ("boot", None)
        self.lines = []   # (t, raw) for diagnostics

    def feed(self, line: str):
        t = time.monotonic()
        with self._lock:
            self.lines.append((t, line))
            m = _STATUS.search(line)
            if m:
                self.events.append((t, "N", int(m.group(1))))
                self.events.append((t, "rssi", int(m.group(2))))
            m = _CONNECT.search(line)
            if m:
                self.events.append((t, "connect", m.group(1)))
            if _BOOT in line:
                self.events.append((t, "boot", None))

    def _of(self, kind, since=0.0):
        with self._lock:
            return [(t, p) for (t, k, p) in self.events if k == kind and t >= since]

    def last(self, kind, since=0.0):
        e = self._of(kind, since)
        return e[-1][1] if e else None

    def all(self, kind, since=0.0):
        return [p for _, p in self._of(kind, since)]

    def saw_boot(self, since):
        return bool(self._of("boot", since))


def wait_for(predicate, timeout, poll=0.2):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        v = predicate()
        if v:
            return v
        time.sleep(poll)
    return None


def fire_webhook(url, method, timeout):
    req = urllib.request.Request(url, method=method.upper())
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read(200).decode("utf-8", "replace")


class Results:
    def __init__(self):
        self.items = []

    def check(self, name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        self.items.append((name, ok))
        print(f"  [{tag}] {name}" + (f"  ({detail})" if detail else ""), flush=True)
        return ok

    def skip(self, name, detail=""):
        print(f"  [SKIP] {name}" + (f"  ({detail})" if detail else ""), flush=True)

    def ok(self):
        return all(ok for _, ok in self.items)


def run_round(con: Tap, fire, args, res: Results, rnd: int):
    print(f"\n--- round {rnd} ---", flush=True)

    # baseline: device must be up and associated before we knock it off
    n0 = wait_for(lambda: con.last("N"), 8)
    rssi0 = con.last("rssi")
    if n0 is None or rssi0 is None:
        return res.check("baseline status line seen", False, "no N=/rssi= on the console")
    print(f"  baseline N={n0} rssi={rssi0}"
          + (f" expect ssid={args.ssid}" if args.ssid else "  (no --ssid: stick check by rssi only)"))
    if rssi0 == 0:
        return res.check("device associated at baseline", False, "rssi=0 — already disconnected")

    # fire the webhook that restarts the AP
    t_fire = time.monotonic()
    try:
        status, body = fire()
        print(f"  webhook {args.restart_method} {args.restart_url} -> {status}", flush=True)
    except Exception as e:
        return res.check("webhook fired", False, str(e))

    # 1. sanity: the outage actually reached the device (rssi drops to 0, or it logs a retry)
    def outage_signal():
        if con.last("rssi", t_fire) == 0:
            return "rssi->0"
        if any("connection timeout" in l or "Connecting WiFi" in l for _, l in con.lines if _ >= t_fire):
            return "reconnect attempt logged"
        return None
    outage = wait_for(outage_signal, args.outage_timeout)
    res.check("AP outage reached the device", bool(outage),
              outage or "rssi never dropped — webhook may not restart this device's AP")

    # 2. no reboot: watch N for a reset and the boot banner until reconnect or timeout.
    #    (evaluated continuously below; a reboot can happen any time in the window)
    def rebooted():
        if con.saw_boot(t_fire):
            return "setup() banner"
        ns = con.all("N", t_fire)
        peak = n0
        for n in ns:
            if n < peak - REBOOT_MARGIN:
                return f"N reset {peak}->{n}"
            peak = max(peak, n)
        return None

    # 3. reconnect: a fresh "Connected to WiFi" line, or rssi back to a healthy level
    def reconnected():
        s = con.last("connect", t_fire)
        if s is not None:
            return s
        r = con.last("rssi", t_fire)
        if r is not None and r != 0 and r > -90:
            return ""  # link healthy again but no connect line captured (e.g. never fully dropped)
        return None

    landed = None
    reboot_reason = None
    deadline = time.monotonic() + args.reconnect_timeout
    while time.monotonic() < deadline:
        reboot_reason = rebooted()
        if reboot_reason:
            break
        landed = reconnected()
        if landed is not None:
            break
        time.sleep(0.2)

    res.check("device did NOT reboot through the outage", reboot_reason is None,
              reboot_reason or f"N continuous (now {con.last('N')})")

    reconnected_ok = landed is not None and not reboot_reason
    res.check(f"reconnected within {args.reconnect_timeout:.0f}s", reconnected_ok,
              f"ssid={landed!r} rssi={con.last('rssi')}" if reconnected_ok else "no reassociation")

    # 4. the stick assertion
    if not reconnected_ok:
        res.skip("reconnected to the same AP (stick)", "did not reconnect")
    elif args.ssid:
        if landed:
            res.check("reconnected to the same AP (stick)", landed == args.ssid,
                      f"landed on {landed!r}, expected {args.ssid!r}")
        else:
            # link recovered without a connect line — fall back to rssi proximity to baseline
            r = con.last("rssi", t_fire)
            res.check("reconnected to the same AP (stick, by rssi)",
                      r is not None and abs(r - rssi0) <= args.rssi_tol,
                      f"rssi {r} vs baseline {rssi0} (tol {args.rssi_tol})")
    else:
        r = con.last("rssi", t_fire)
        close = r is not None and abs(r - rssi0) <= args.rssi_tol
        res.check("reconnected to a comparable AP (no --ssid: rssi proximity)", close,
                  f"ssid={landed!r} rssi {r} vs baseline {rssi0} (tol {args.rssi_tol})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--serial", metavar="DEV", help="control console over serial (recommended)")
    g.add_argument("--telnet", metavar="IP:PORT", help="control console over telnet (must NOT route via the AP under test)")
    ap.add_argument("--restart-url", required=True, help="webhook that restarts the AP/router (e.g. http://192.168.1.173/scan)")
    ap.add_argument("--restart-method", default="GET", help="HTTP method for the webhook (default GET)")
    ap.add_argument("--restart-timeout", type=float, default=10.0, help="webhook request timeout (s)")
    ap.add_argument("--ssid", help="the SSID being restarted; reconnect must land here (the stick assertion)")
    ap.add_argument("--outage-timeout", type=float, default=30.0, help="how long to wait for the link to drop (s)")
    ap.add_argument("--reconnect-timeout", type=float, default=120.0, help="how long to allow for reconnect (s)")
    ap.add_argument("--rssi-tol", type=int, default=15, help="dB tolerance when comparing reconnect rssi to baseline")
    ap.add_argument("--rounds", type=int, default=1, help="repeat the restart N times")
    ap.add_argument("--settle", type=float, default=8.0, help="seconds to let the link settle between rounds")
    args = ap.parse_args()

    if args.serial:
        transport = SerialTransport(args.serial)
        ctrl = "serial " + args.serial
    else:
        ip, _, p = args.telnet.partition(":")
        transport = SocketTransport(ip, port=int(p or 23), timeout=8)
        ctrl = "telnet " + args.telnet

    tap = Tap()
    con = Console(transport, on_line=tap.feed)
    print(f"control={ctrl}  webhook={args.restart_method} {args.restart_url}  rounds={args.rounds}")
    res = Results()
    fire = lambda: fire_webhook(args.restart_url, args.restart_method, args.restart_timeout)
    try:
        if not con.wait_ready(timeout=30):
            print("device not responding on the control console", flush=True)
            return 1
        for rnd in range(1, args.rounds + 1):
            run_round(tap, fire, args, res, rnd)
            if rnd < args.rounds:
                time.sleep(args.settle)
    finally:
        con.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
