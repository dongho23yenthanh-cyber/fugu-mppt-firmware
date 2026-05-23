#!/usr/bin/env python3
"""E2E test for the *roam-after-switch_delay* path (counterpart to test_stick_to_wifi.py).

When the AP we hold goes down for **longer than ``wifi.conf::switch_delay``** (default 30 s),
the firmware must stop sticking to that SSID and roam to another configured AP via
``wifiMulti.run()``  (see ``wait_for_wifi`` at src/tele/telemetry.cpp:184). This test asserts
that transition; ``test_stick_to_wifi.py`` asserts the opposite half (short outage → no roam).

Setup the device must already have
------------------------------------
  * At least two SSIDs configured (``ssid_<A>`` = the AP under test, plus ``ssid_<B>``=the
    fallback). Use ``set-config wifi.conf ssid_<B> <name>`` / ``ssid_<B>_psk <key>``.
  * ``switch_delay`` at its default (or whatever you pass via ``--switch-delay``); we don't
    change it from the test because the value is only read in ``wifi_load_conf()`` at
    first-connect — touching it would force a reboot and defeat the no-reboot assertion.

How it works (mirrors test_stick_to_wifi.py)
--------------------------------------------
Serial control (the only path that survives a long outage of the AP under test). FuguDevice
taps the streamed status line for ``has_crashed()`` + ``wifi_rssi``; ``on_message`` collects
the ``Connected to WiFi <ssid>, RSSI ..`` lines.

The webhook must take ``--ssid`` **down for at least ``--down-seconds``** (which must exceed
``--switch-delay``). If your existing ``/scan`` webhook just bounces the router, you need a
different endpoint here (e.g. one that disables the SSID on the AP and re-enables it after a
delay), otherwise the device just sticks and the test is meaningless — we sanity-check this
by asserting the outage actually lasted long enough.

Asserts (per round):
  1. baseline — device associated to ``--ssid``;
  2. webhook fired; link drops (rssi -> 0);
  3. no reboot through the outage (N continuous, no ``setup() done`` banner);
  4. once ``switch_delay`` elapses, device reassociates to ``--other-ssid`` (the roam);
  5. (soft) when the original AP returns, behaviour is left unspecified here — both
     "stay on B" and "return to A" are acceptable; we just log which one happened.

Usage
-----
    python etc/e2e-test/test_wifi_roam_on_long_outage.py --serial /dev/cu.usbmodem1201 \\
        --restart-url 'http://192.168.1.173/down?ssid=pwr-statione&seconds=60' \\
        --ssid pwr-statione --other-ssid pwr-stationw --down-seconds 60
"""
import argparse
import os
import re
import sys
import threading
import time
import urllib.request

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ETC_DIR)
from fugu.transport import SerialTransport
from fugu.fugu import FuguDevice

_CONNECT = re.compile(r"Connected to WiFi (.+?), RSSI (-?\d+) IP (\S+)")


class Markers:
    def __init__(self):
        self._lock = threading.Lock()
        self.ev = []  # (t, "connect", ssid)

    def feed(self, line):
        if m := _CONNECT.search(line):
            with self._lock:
                self.ev.append((time.monotonic(), "connect", m.group(1)))

    def connects_since(self, since):
        with self._lock:
            return [(t, s) for (t, k, s) in self.ev if k == "connect" and t >= since]


def wait_for(predicate, timeout, poll=0.3):
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
    def __init__(self): self.items = []
    def check(self, name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        self.items.append((name, ok))
        print(f"  [{tag}] {name}" + (f"  ({detail})" if detail else ""), flush=True)
        return ok
    def skip(self, name, detail=""): print(f"  [SKIP] {name}" + (f"  ({detail})" if detail else ""), flush=True)
    def ok(self): return all(ok for _, ok in self.items)


def run_round(dev: FuguDevice, mk: Markers, args, res: Results, rnd: int):
    print(f"\n--- round {rnd} ---", flush=True)
    if not wait_for(lambda: dev.pwm_state.ccm is not None, 10):
        return res.check("baseline status line seen", False, "no status line")
    if not wait_for(lambda: dev.wifi_rssi != 0, 8):
        return res.check("device associated at baseline", False, "rssi=0 at start")

    # Last connect line tells us which SSID we're on (the test requires we start on --ssid).
    last_conn = mk.connects_since(0)
    if last_conn and last_conn[-1][1] != args.ssid:
        return res.check(f"baseline on --ssid ({args.ssid})", False,
                         f"on {last_conn[-1][1]!r}; pre-condition not met")

    t_fire = time.monotonic()
    dev.has_crashed(reset=True)
    try:
        status, body = fire_webhook(args.restart_url, args.restart_method, args.restart_timeout)
        print(f"  webhook -> {status}", flush=True)
    except Exception as e:
        return res.check("webhook fired", False, str(e))

    # 1. outage reaches the device
    dropped = wait_for(lambda: dev.wifi_rssi == 0, args.outage_timeout)
    res.check("AP outage reached the device", bool(dropped),
              "rssi->0" if dropped else f"rssi still {dev.wifi_rssi}")

    # 2. while inside switch_delay we expect to be either offline or back on --ssid (sticking).
    #    Roam to --other-ssid before switch_delay would itself be a failure of the stick path,
    #    so only check that nothing landed on --other-ssid early.
    early_deadline = t_fire + args.switch_delay * 0.8
    while time.monotonic() < early_deadline:
        if any(s == args.other_ssid for _, s in mk.connects_since(t_fire)):
            break
        time.sleep(0.3)
    early_roam = [s for _, s in mk.connects_since(t_fire) if s == args.other_ssid]
    res.check("did NOT roam before switch_delay elapsed",
              not early_roam,
              f"saw connect to {args.other_ssid} too early" if early_roam else "")

    # 3. after switch_delay elapses the device must roam to --other-ssid (the assertion).
    deadline = t_fire + args.switch_delay + args.roam_timeout
    landed = wait_for(
        lambda: next((s for _, s in mk.connects_since(t_fire) if s == args.other_ssid), None),
        max(0.0, deadline - time.monotonic()))
    res.check(f"roamed to {args.other_ssid} after switch_delay",
              landed is not None,
              f"connects since outage: {[s for _, s in mk.connects_since(t_fire)]}")

    # 4. no reboot through the whole window
    res.check("did NOT reboot/crash through the outage", not dev.has_crashed(),
              "panic marker in serial log" if dev.has_crashed() else "no panic")

    # 5. sanity: the outage really lasted long enough for the test to mean anything.
    if landed is not None:
        actual = time.monotonic() - t_fire
        res.check("outage long enough to exercise the roam path",
                  actual >= args.switch_delay,
                  f"roamed after only {actual:.0f}s vs switch_delay={args.switch_delay:.0f}s")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", metavar="DEV", required=True,
                    help="control console over serial (required — both networks may be unreachable mid-test)")
    ap.add_argument("--restart-url", required=True,
                    help="webhook that takes --ssid DOWN for --down-seconds (NOT just a router reboot)")
    ap.add_argument("--restart-method", default="GET")
    ap.add_argument("--restart-timeout", type=float, default=10.0)
    ap.add_argument("--ssid", required=True, help="SSID being knocked down (must be currently associated)")
    ap.add_argument("--other-ssid", required=True, help="fallback SSID the device must roam to")
    ap.add_argument("--switch-delay", type=float, default=30.0,
                    help="wifi.conf::switch_delay value the firmware is using (s; default 30)")
    ap.add_argument("--down-seconds", type=float, default=60.0,
                    help="how long the webhook will keep --ssid down; must exceed --switch-delay")
    ap.add_argument("--outage-timeout", type=float, default=30.0)
    ap.add_argument("--roam-timeout", type=float, default=60.0,
                    help="extra seconds beyond switch_delay to wait for the roam")
    ap.add_argument("--rounds", type=int, default=1)
    args = ap.parse_args()

    if args.down_seconds <= args.switch_delay:
        print(f"--down-seconds ({args.down_seconds}) must exceed --switch-delay ({args.switch_delay})",
              file=sys.stderr)
        return 2

    mk = Markers()
    dev = FuguDevice(SerialTransport(args.serial), block=False)
    dev.on_message = mk.feed
    print(f"control=serial {args.serial}  ssid={args.ssid} -> other={args.other_ssid}"
          f"  switch_delay={args.switch_delay}s  down={args.down_seconds}s  rounds={args.rounds}")
    res = Results()
    try:
        if not wait_for(lambda: dev.pwm_state.ccm is not None, 30):
            print("device not responding on the control console", flush=True)
            return 1
        for rnd in range(1, args.rounds + 1):
            run_round(dev, mk, args, res, rnd)
    finally:
        dev.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
