#!/usr/bin/env python3
"""E2E test for ``wifi off <minutes>`` — the temporary Wi-Fi disable.

Covers the behaviour added on `scope-client`:

  * ``wifi off <minutes>``  disables Wi-Fi but **keeps** the stored SSID and **auto
    re-enables** after the timeout (it must NOT stay off and NOT crash/reboot).
  * ``wifi on``             cancels a pending timed re-enable.
  * bare ``wifi off`` is left untouched (disables for good, wipes the sticky SSID) — this
    test never issues it, so the device's saved network survives the run.

How it works
------------
Drives a ``FuguDevice`` (etc/fugu) over serial. FuguDevice taps every console line and
maintains the bits we need:

  * ``has_crashed()`` — True if a panic marker (Guru Meditation / Backtrace / LoadProhibited
    / …) appeared in the serial log since the per-phase baseline (``has_crashed(reset=True)``).
    Serial only; the panic prints before the reset, so the reconnect crash is caught here.
  * ``wifi_rssi`` — link state: 0 when the station drops, non-zero when associated.

On top of that we tap ``on_message`` for the wifi-lifecycle log lines: ``Connected to WiFi
<ssid> …``, ``WiFi off for <n> min`` (command accepted) and ``WiFi re-enabled after timeout``
(the timer fired — the deterministic proof of the feature).

**Serial only.** ``wifi off`` takes the link down on purpose, so any telnet/BLE control path
that routes through the device's Wi-Fi goes dark for the whole window; and ``has_crashed``
needs the serial panic log. The UART/USB console streams regardless of Wi-Fi.

Note on reassociation: re-enabling only flips ``disableWifi`` back off — the firmware then
reconnects on its own schedule, and ``wifiLoop()`` only *attempts* a connect while the
converter is idle/below ~10 W (Wi-Fi RF perturbs the control loop). So on a device actively
producing power the radio comes back but won't reassociate until the load drops; this is the
same gate that governs ``wifi on``. The test therefore asserts the **timer**, and treats
reassociation as a soft check: PASS if it reconnects, SKIP if it's re-enabled but the
converter is loaded, FAIL only on a crash/reboot.

The test:

  1. baseline — device up and associated (rssi != 0);
  2. ``wifi off <minutes>`` is accepted and logs ``WiFi off for <minutes> min``;
  3. the link actually drops (rssi -> 0) — the disable took effect;
  4. it stays down for most of the window (the timeout is honoured, not an instant bounce);
  5. it logs ``WiFi re-enabled after timeout`` within ``minutes*60 + slack``;
  6. it does **not crash or reboot** through the off -> re-enable -> reconnect sequence — the
     crash this test guards against fires *during* the reconnect, a dozen seconds after the
     timer line, so we keep watching for ``--settle`` seconds past it (via FuguDevice.has_crashed());
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
import time

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc (holds the fugu pkg)
sys.path.insert(0, ETC_DIR)
from fugu.transport import SerialTransport
from fugu.fugu import FuguDevice
from _harness import Results, wait_for, EventLog, RE_CONNECT

_POWER = re.compile(r"(-?\d+(?:\.\d+)?)W\s")              # power field of the status line


class Markers(EventLog):
    """Records the wifi-lifecycle log lines via FuguDevice.on_message (already ANSI-stripped and
    with pwm-status/ina-timeout noise filtered out). Crash/reboot and rssi come from FuguDevice."""

    def feed(self, line):
        super().feed(line)
        if m := RE_CONNECT.search(line):
            self.add("connect", m.group(1))
        if "WiFi re-enabled after timeout" in line:
            self.add("reenable")
        if m := re.search(r"WiFi off for (\d+) min", line):
            self.add("off", int(m.group(1)))
        if m := _POWER.search(line):
            self.add("power", float(m.group(1)))

    def peak_power(self, since=0.0):
        ps = self.all("power", since)
        return max(ps) if ps else None




def baseline(dev: FuguDevice, res: Results, need_assoc=True):
    """True once the device is responsive (and, if need_assoc, associated)."""
    if not wait_for(lambda: dev.pwm_state.ccm is not None, 10):
        return res.check("baseline status line seen", False, "no status line on the console")
    if need_assoc and not wait_for(lambda: dev.wifi_rssi != 0, 8):
        return res.check("device associated at baseline", False, "rssi=0 — not connected; bring Wi-Fi up first")
    print(f"  baseline rssi={dev.wifi_rssi}", flush=True)
    return True


def test_timeout(dev: FuguDevice, mk: Markers, args, res: Results):
    print(f"\n--- timeout: wifi off {args.minutes} ---", flush=True)
    if not baseline(dev, res):
        return
    window = args.minutes * 60

    t_fire = time.monotonic()
    dev.has_crashed(reset=True)  # phase-local baseline; has_crashed() flags any panic since
    reply = dev.console.command(f"wifi off {args.minutes}", timeout=4.0)
    res.check("`wifi off <min>` accepted", reply.ok and "WiFi off for" in reply.text,
              reply.text.strip() or ("rejected" if reply.rejected else "no OK"))

    # the disable took effect: the link drops to rssi=0
    dropped = wait_for(lambda: dev.wifi_rssi == 0, args.drop_timeout)
    res.check("link dropped after `wifi off`", bool(dropped),
              "rssi->0" if dropped else f"rssi still {dev.wifi_rssi}")

    # the timeout is honoured: the re-enable line must not fire in the first half of the window.
    early_deadline = t_fire + window * 0.5
    while time.monotonic() < early_deadline:
        if dev.has_crashed() or mk.first_t("reenable", t_fire):
            break
        time.sleep(0.5)
    early = mk.first_t("reenable", t_fire)
    res.check("stayed off for most of the window (timeout honoured)",
              early is None and not dev.has_crashed(),
              "crashed" if dev.has_crashed() else
              (f"re-enabled after only {early - t_fire:.0f}s" if early else
               f"still off at {time.monotonic() - t_fire:.0f}s"))

    # the timer fires: "WiFi re-enabled after timeout" within window + slack.
    deadline = t_fire + window + args.slack
    while time.monotonic() < deadline:
        if dev.has_crashed() or mk.first_t("reenable", t_fire):
            break
        time.sleep(0.5)
    reenabled = mk.first_t("reenable", t_fire)
    res.check(f"re-enable timer fired within {window + args.slack:.0f}s",
              reenabled is not None and not dev.has_crashed(),
              f"after {reenabled - t_fire:.0f}s" if reenabled else "no 'WiFi re-enabled after timeout'")

    # after re-enable the device reconnects; the crash this test exists for happens THERE, a dozen
    # seconds after the timer line. Keep watching for a crash and for reassociation.
    landed = None
    settle_end = (reenabled or time.monotonic()) + args.settle
    while time.monotonic() < settle_end:
        if dev.has_crashed():
            break
        if landed is None and (mk.last("connect", t_fire) is not None or dev.wifi_rssi != 0):
            landed = mk.last("connect", t_fire) or ""  # keep watching: crash may follow reconnect
        time.sleep(0.5)

    # headline: the whole off -> re-enable -> reconnect sequence must not crash or reboot.
    crash = dev.has_crashed()
    res.check("did NOT crash/reboot through off + re-enable + reconnect", not crash,
              "panic detected" if crash else "no panic marker in the serial log")

    # reassociation: soft + load-aware (see module docstring).
    connected = landed is not None or (reenabled and dev.wifi_rssi != 0)
    peak_w = mk.peak_power(t_fire)
    if crash:
        res.skip("reassociated after re-enable", "crashed — see above")
    elif connected:
        if args.ssid and landed:
            res.check("reconnected to the same AP (SSID kept)", landed == args.ssid,
                      f"landed on {landed!r}, expected {args.ssid!r}")
        else:
            res.check("reassociated after re-enable", True, f"rssi={dev.wifi_rssi}")
    elif peak_w is not None and peak_w >= 10:
        res.skip("reassociated after re-enable",
                 f"converter loaded (~{peak_w:.0f} W) — firmware defers reconnect by design")
    else:
        res.check("reassociated after re-enable", False, f"idle (~{peak_w} W) yet never reconnected")


def test_cancel(dev: FuguDevice, mk: Markers, args, res: Results):
    print(f"\n--- cancel: wifi off {args.minutes} then wifi on ---", flush=True)
    # association not required: this asserts the timer is cancelled, which is independent of the
    # link state (a loaded converter may already be off after the timeout test).
    if not baseline(dev, res, need_assoc=False):
        return
    window = args.minutes * 60

    t_fire = time.monotonic()
    dev.has_crashed(reset=True)
    off = dev.console.command(f"wifi off {args.minutes}", timeout=4.0)
    res.check("`wifi off <min>` accepted (cancel setup)", off.ok and "WiFi off for" in off.text,
              off.text.strip())
    wait_for(lambda: dev.wifi_rssi == 0, args.drop_timeout)

    on = dev.console.command("wifi on", timeout=4.0)
    res.check("`wifi on` accepted", on.ok, on.text.strip() or "no OK")

    # cancel assertion (deterministic, independent of converter load): the pending timer must not
    # fire. Watch past when it would have (the full window + a margin) for the re-enable line.
    watch = window + 15
    print(f"  watching {watch:.0f}s for a stray re-enable line ...", flush=True)
    fired = wait_for(lambda: mk.first_t("reenable", t_fire), watch)
    res.check("`wifi on` cancelled the pending re-enable", fired is None,
              "saw 'WiFi re-enabled after timeout' — timer not cancelled" if fired else
              "no timer line after the window elapsed")
    res.check("no crash/reboot during cancel", not dev.has_crashed(),
              "panic detected" if dev.has_crashed() else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", metavar="DEV", required=True,
                    help="control console over serial (required; `wifi off` kills network transports,"
                         " and has_crashed needs the serial panic log)")
    ap.add_argument("--minutes", type=int, default=1,
                    help="timeout passed to `wifi off` (default 1; the test waits this long)")
    ap.add_argument("--ssid", help="expected SSID after re-enable; asserts the SSID was kept")
    ap.add_argument("--drop-timeout", type=float, default=20.0, help="how long to wait for the link to drop (s)")
    ap.add_argument("--slack", type=float, default=45.0, help="grace beyond minutes*60 for re-enable (s)")
    ap.add_argument("--settle", type=float, default=30.0,
                    help="after the re-enable line, keep watching this long for the reconnect crash (s)")
    ap.add_argument("--skip-cancel", action="store_true", help="run only the timeout test")
    args = ap.parse_args()

    mk = Markers()
    dev = FuguDevice(SerialTransport(args.serial), block=False)
    dev.on_message = mk.feed
    print(f"control=serial {args.serial}  minutes={args.minutes}"
          + (f"  expect ssid={args.ssid}" if args.ssid else "  (no --ssid: SSID-retention not asserted)"))
    res = Results()
    try:
        if not wait_for(lambda: dev.pwm_state.ccm is not None, 30):
            print("device not responding on the control console", flush=True)
            return 1
        test_timeout(dev, mk, args, res)
        if not args.skip_cancel:
            test_cancel(dev, mk, args, res)
    finally:
        dev.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
