#!/usr/bin/env python3
"""E2E test for ``wifi off <minutes>`` — the temporary Wi-Fi disable.

Covers the behaviour added on `scope-client`:

  * ``wifi off <minutes>``  disables Wi-Fi but **keeps** the stored SSID and **auto
    re-enables** after the timeout (it must NOT stay off and NOT reboot).
  * ``wifi on``             cancels a pending timed re-enable.
  * bare ``wifi off`` is left untouched (disables for good, wipes the sticky SSID) — this
    test never issues it, so the device's saved network survives the run.

How it works
------------
A control console (serial required) taps the device's streaming status line:

  * ``N=<nSamples>`` — per-boot ADC sample counter; only climbs. A drop to ~0 (or a
    ``setup() done.`` banner) means the device **rebooted** → FAIL.
  * ``rssi=`` — link state: 0 when the station drops, non-zero when associated.
  * ``Connected to WiFi <ssid> ...`` — names the AP it (re)joined.

It also matches the firmware's own log lines: ``WiFi off for <n> min`` (command accepted)
and ``WiFi re-enabled after timeout`` (the timer fired) — the latter is the deterministic
proof of the feature.

**Serial only.** ``wifi off`` takes the link down on purpose, so any telnet/BLE control path
that routes through the device's Wi-Fi goes dark for the whole window and can't see the
re-enable. The UART status line streams regardless of Wi-Fi.

Note on reassociation: re-enabling only flips ``disableWifi`` back off — the firmware then
reconnects on its own schedule, and ``wifiLoop()`` only *attempts* a connect while the
converter is idle/below ~10 W (Wi-Fi RF perturbs the control loop). So on a device actively
producing power the radio comes back but won't reassociate until the load drops; this is the
same gate that governs ``wifi on``. The test therefore asserts the **timer**, and treats
reassociation as a soft check: PASS if it reconnects, SKIP if it's re-enabled but the
converter is loaded, FAIL only on a reboot.

The test:

  1. baseline — device up and associated (rssi != 0);
  2. ``wifi off <minutes>`` is accepted and logs ``WiFi off for <minutes> min``;
  3. the link actually drops (rssi -> 0) — the disable took effect;
  4. it stays down for most of the window (the timeout is honoured, not an instant bounce);
  5. it logs ``WiFi re-enabled after timeout`` within ``minutes*60 + slack``;
  6. it does **not crash or reboot** through the off -> re-enable -> reconnect sequence — the
     crash this test guards against fires *during* the reconnect, a dozen seconds after the
     timer line, so we keep watching for ``--settle`` seconds past it (panic banners are matched
     directly, as is an N-counter reset / boot banner);
  7. reassociation (soft, load-aware) — and with ``--ssid`` the reconnect must land on it.

Then a fast cancel check: ``wifi off <minutes>`` immediately followed by ``wifi on`` must
cancel the pending re-enable — no ``WiFi re-enabled after timeout`` line is allowed to fire.

Usage
-----
    python etc/e2e-test/test_wifi_off_timeout.py --serial /dev/cu.usbmodem1201
    python etc/e2e-test/test_wifi_off_timeout.py --serial /dev/cu.usbmodem1201 \
        --minutes 1 --ssid pwr-statione
"""
import argparse
import os
import re
import sys
import threading
import time

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc (holds the fugu pkg)
sys.path.insert(0, ETC_DIR)
from fugu.transport import SerialTransport
from fugu.console import Console

# status line:  V=../.. I=../..A  <power>W ... N=<samples> rssi=<dBm>
_STATUS = re.compile(r"\bN=(\d+)\b.*?\brssi=(-?\d+)")
_POWER = re.compile(r"(-?\d+(?:\.\d+)?)W\s")
# telemetry.cpp: "Connected to WiFi <ssid>, RSSI <r> IP <ip>"
_CONNECT = re.compile(r"Connected to WiFi (.+?), RSSI (-?\d+) IP (\S+)")
_BOOT = "setup() done"
REBOOT_MARGIN = 100  # N may jitter by a sample or two between reads; a real reboot drops it to ~0
# a panic prints these before the device resets; catch them directly so we don't rely on N alone
_PANIC = re.compile(r"Guru Meditation|panic'?ed|Backtrace:|abort\(\) was called|"
                    r"StoreProhibited|LoadProhibited|assert failed|rst:0x")


class Tap:
    """Records and parses the device's streamed console lines (via Console.on_line)."""

    def __init__(self):
        self._lock = threading.Lock()
        self.events = []  # (t, kind, payload): ("N", n) ("rssi", r) ("connect", ssid)
                          #                     ("off", mins) ("reenable", None) ("boot", None)
        self.lines = []   # (t, raw) for diagnostics

    def feed(self, line: str):
        t = time.monotonic()
        with self._lock:
            self.lines.append((t, line))
            m = _STATUS.search(line)
            if m:
                self.events.append((t, "N", int(m.group(1))))
                self.events.append((t, "rssi", int(m.group(2))))
                p = _POWER.search(line)
                if p:
                    self.events.append((t, "power", float(p.group(1))))
            m = _CONNECT.search(line)
            if m:
                self.events.append((t, "connect", m.group(1)))
            m = re.search(r"WiFi off for (\d+) min", line)
            if m:
                self.events.append((t, "off", int(m.group(1))))
            if "WiFi re-enabled after timeout" in line:
                self.events.append((t, "reenable", None))
            if _BOOT in line:
                self.events.append((t, "boot", None))
            m = _PANIC.search(line)
            if m:
                self.events.append((t, "panic", m.group(0)))

    def _of(self, kind, since=0.0):
        with self._lock:
            return [(t, p) for (t, k, p) in self.events if k == kind and t >= since]

    def last(self, kind, since=0.0):
        e = self._of(kind, since)
        return e[-1][1] if e else None

    def first_t(self, kind, since=0.0):
        e = self._of(kind, since)
        return e[0][0] if e else None

    def saw_boot(self, since):
        return bool(self._of("boot", since))

    def crashed(self, since, baseline_n):
        """A reboot/panic since `since`, described, or None. Catches panics directly (before the
        reset) and an N-counter reset / boot banner (after it)."""
        p = self._of("panic", since)
        if p:
            return f"panic: {p[0][1]!r}"
        if self.saw_boot(since):
            return "setup() banner"
        peak = baseline_n
        for _, n in self._of("N", since):
            if n < peak - REBOOT_MARGIN:
                return f"N reset {peak}->{n}"
            peak = max(peak, n)
        return None


def wait_for(predicate, timeout, poll=0.2):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        v = predicate()
        if v:
            return v
        time.sleep(poll)
    return None


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


def baseline(con: Console, tap: Tap, res: Results, need_assoc=True):
    """Returns (N, rssi) once the device is responsive (and, if need_assoc, associated)."""
    n0 = wait_for(lambda: tap.last("N"), 10)
    rssi0 = tap.last("rssi")
    if n0 is None or rssi0 is None:
        res.check("baseline status line seen", False, "no N=/rssi= on the console")
        return None
    if need_assoc and rssi0 == 0:
        res.check("device associated at baseline", False, "rssi=0 — not connected; bring Wi-Fi up first")
        return None
    print(f"  baseline N={n0} rssi={rssi0}", flush=True)
    return n0, rssi0


def _loaded(tap: Tap, since=0.0):
    """Peak converter power (W) seen since `since`, or None if unknown."""
    ps = [p for _, p in tap._of("power", since)]
    return max(ps) if ps else None


def test_timeout(con: Console, tap: Tap, args, res: Results):
    print(f"\n--- timeout: wifi off {args.minutes} ---", flush=True)
    base = baseline(con, tap, res)
    if base is None:
        return
    n0, rssi0 = base
    window = args.minutes * 60

    t_fire = time.monotonic()
    reply = con.command(f"wifi off {args.minutes}", timeout=4.0)
    res.check("`wifi off <min>` accepted", reply.ok and "WiFi off for" in reply.text,
              reply.text.strip() or ("rejected" if reply.rejected else "no OK"))

    # the disable took effect: the link drops to rssi=0
    dropped = wait_for(lambda: tap.last("rssi", t_fire) == 0, args.drop_timeout)
    res.check("link dropped after `wifi off`", bool(dropped),
              "rssi->0" if dropped else f"rssi still {tap.last('rssi', t_fire)}")

    # the timeout is honoured: the re-enable line must not fire in the first half of the window.
    early_deadline = t_fire + window * 0.5
    while time.monotonic() < early_deadline:
        if tap.crashed(t_fire, n0) or tap.first_t("reenable", t_fire):
            break
        time.sleep(0.5)
    early = tap.first_t("reenable", t_fire)
    res.check("stayed off for most of the window (timeout honoured)",
              early is None and tap.crashed(t_fire, n0) is None,
              tap.crashed(t_fire, n0) or (f"re-enabled after only {early - t_fire:.0f}s" if early else
                                          f"still off at {time.monotonic() - t_fire:.0f}s"))

    # the timer fires: "WiFi re-enabled after timeout" within window + slack.
    deadline = t_fire + window + args.slack
    while time.monotonic() < deadline:
        if tap.crashed(t_fire, n0) or tap.first_t("reenable", t_fire):
            break
        time.sleep(0.5)
    reenabled = tap.first_t("reenable", t_fire)
    res.check(f"re-enable timer fired within {window + args.slack:.0f}s",
              reenabled is not None and tap.crashed(t_fire, n0) is None,
              f"after {reenabled - t_fire:.0f}s" if reenabled else "no 'WiFi re-enabled after timeout'")

    # after re-enable the device reconnects; the crash this test exists for happens THERE, a dozen
    # seconds after the timer line. Keep watching for a crash and for reassociation.
    landed = None
    settle_end = (reenabled or time.monotonic()) + args.settle
    while time.monotonic() < settle_end:
        if tap.crashed(t_fire, n0):
            break
        if landed is None and (tap.last("connect", t_fire) is not None
                               or (tap.last("rssi", t_fire) or 0) != 0):
            landed = tap.last("connect", t_fire) or ""  # keep watching: crash may follow reconnect
        time.sleep(0.5)

    # headline: the whole off -> re-enable -> reconnect sequence must not crash or reboot.
    crash = tap.crashed(t_fire, n0)
    res.check("did NOT crash/reboot through off + re-enable + reconnect", crash is None,
              crash or f"N continuous (now {tap.last('N')})")

    # reassociation: soft + load-aware. The firmware only attempts a connect while the converter
    # is idle/<~10 W, so a loaded device legitimately stays off until the load drops.
    connected = landed is not None or (reenabled and (tap.last("rssi", t_fire) or 0) != 0)
    peak_w = _loaded(tap, t_fire)
    if crash:
        res.skip("reassociated after re-enable", "crashed — see above")
    elif connected:
        if args.ssid and landed:
            res.check("reconnected to the same AP (SSID kept)", landed == args.ssid,
                      f"landed on {landed!r}, expected {args.ssid!r}")
        else:
            res.check("reassociated after re-enable", True, f"rssi={tap.last('rssi', t_fire)}")
    elif peak_w is not None and peak_w >= 10:
        res.skip("reassociated after re-enable",
                 f"converter loaded (~{peak_w:.0f} W) — firmware defers reconnect by design")
    else:
        res.check("reassociated after re-enable", False,
                  f"idle (~{peak_w} W) yet never reconnected")


def test_cancel(con: Console, tap: Tap, args, res: Results):
    print(f"\n--- cancel: wifi off {args.minutes} then wifi on ---", flush=True)
    # association not required: this asserts the timer is cancelled, which is independent of the
    # link state (a loaded converter may already be off after the timeout test).
    base = baseline(con, tap, res, need_assoc=False)
    if base is None:
        return
    n0, _ = base
    window = args.minutes * 60

    t_fire = time.monotonic()
    off = con.command(f"wifi off {args.minutes}", timeout=4.0)
    res.check("`wifi off <min>` accepted (cancel setup)", off.ok and "WiFi off for" in off.text,
              off.text.strip())
    wait_for(lambda: tap.last("rssi", t_fire) == 0, args.drop_timeout)

    on = con.command("wifi on", timeout=4.0)
    res.check("`wifi on` accepted", on.ok, on.text.strip() or "no OK")

    # cancel assertion (deterministic, independent of converter load): the pending timer must not
    # fire. Watch past when it would have (the full window + a margin) for the re-enable line.
    watch = window + 15
    print(f"  watching {watch:.0f}s for a stray re-enable line ...", flush=True)
    fired = wait_for(lambda: tap.first_t("reenable", t_fire), watch)
    res.check("`wifi on` cancelled the pending re-enable", fired is None,
              "saw 'WiFi re-enabled after timeout' — timer not cancelled" if fired else
              "no timer line after the window elapsed")
    res.check("no crash/reboot during cancel", tap.crashed(t_fire, n0) is None,
              tap.crashed(t_fire, n0) or "")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", metavar="DEV", required=True,
                    help="control console over serial (required; `wifi off` kills network transports)")
    ap.add_argument("--minutes", type=int, default=1,
                    help="timeout passed to `wifi off` (default 1; the test waits this long)")
    ap.add_argument("--ssid", help="expected SSID after re-enable; asserts the SSID was kept")
    ap.add_argument("--drop-timeout", type=float, default=20.0, help="how long to wait for the link to drop (s)")
    ap.add_argument("--slack", type=float, default=45.0, help="grace beyond minutes*60 for re-enable (s)")
    ap.add_argument("--settle", type=float, default=30.0,
                    help="after the re-enable line, keep watching this long for the reconnect crash (s)")
    ap.add_argument("--skip-cancel", action="store_true", help="run only the timeout test")
    args = ap.parse_args()

    transport = SerialTransport(args.serial)
    tap = Tap()
    con = Console(transport, on_line=tap.feed)
    print(f"control=serial {args.serial}  minutes={args.minutes}"
          + (f"  expect ssid={args.ssid}" if args.ssid else "  (no --ssid: SSID-retention not asserted)"))
    res = Results()
    try:
        if not con.wait_ready(timeout=30):
            print("device not responding on the control console", flush=True)
            return 1
        test_timeout(con, tap, args, res)
        if not args.skip_cancel:
            test_cancel(con, tap, args, res)
    finally:
        con.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
