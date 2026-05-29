#!/usr/bin/env python3
"""E2E test for the AP-outage WiFi behaviour — both halves, selected by ``--mode``.

A configurable webhook knocks the AP the device is on down; the device must ride the outage
**without rebooting or crashing**, and then:

  ``--mode stick`` (short outage, < ``switch_delay``): reconnect to the **same** AP (``--ssid``).
      Regression for the scope-client bug where losing the AP overflowed mqtt_task, panicked the
      device, and it booted onto whatever AP was strongest while its own was down.

  ``--mode roam``  (long outage, > ``switch_delay``): stop sticking and **roam** to another
      configured AP (``--other-ssid``) via ``wifiMulti.run()`` (telemetry.cpp wait_for_wifi). The
      webhook must take ``--ssid`` down for at least ``--down-seconds`` (> ``--switch-delay``),
      otherwise the device just sticks and the roam path is never exercised (asserted as a sanity
      check). A plain router-bounce webhook won't do — you need one that disables the SSID.

Both modes drive a ``FuguDevice``: ``wifi_rssi`` (link state) and ``pwm_state`` come from the
streamed status line, connect events from ``on_message``, and reboot/crash from
``has_rebooted()`` (any transport) + ``has_crashed()`` (serial only). **Use ``--serial``** — the
console must survive an outage of the AP under test; a telnet path routed through that AP goes
dark and blinds the reboot/reconnect detection.

Usage
-----
    python etc/e2e-test/test_wifi_outage.py --mode stick --serial /dev/cu.usbmodem1201 \
        --restart-url http://192.168.1.173/scan --ssid pwr-statione
    python etc/e2e-test/test_wifi_outage.py --mode roam --serial /dev/cu.usbmodem1201 \
        --restart-url 'http://192.168.1.173/down?ssid=pwr-statione&seconds=60' \
        --ssid pwr-statione --other-ssid pwr-stationw --down-seconds 60
"""
import argparse
import os
import sys
import time

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc (holds the fugu pkg)
sys.path.insert(0, ETC_DIR)
from fugu.transport import SerialTransport, SocketTransport
from fugu.fugu import FuguDevice
from _harness import Results, wait_for, fire_webhook, EventLog, RE_CONNECT


class Markers(EventLog):
    """Connect events (and raw lines for outage-signal scans) via FuguDevice.on_message."""

    def feed(self, line):
        super().feed(line)
        if m := RE_CONNECT.search(line):
            self.add("connect", m.group(1))

    def connects_since(self, since):
        return self.events("connect", since)  # [(t, ssid)]


def reset_restart_watch(dev, serial):
    dev.has_rebooted(reset=True)
    if serial:
        dev.has_crashed(reset=True)


def bad_restart(dev, serial):
    """Return a reason string if the device rebooted/crashed, else None."""
    if dev.has_rebooted():
        return "rebooted"
    if serial and dev.has_crashed():
        return "crashed"
    return None


def baseline(dev: FuguDevice, res: Results):
    if not wait_for(lambda: dev.pwm_state.ccm is not None, 10):
        return res.check("baseline status line seen", False, "no status line on the console")
    if not wait_for(lambda: dev.wifi_rssi != 0, 8):
        return res.check("device associated at baseline", False, "rssi=0 — bring Wi-Fi up first")
    print(f"  baseline rssi={dev.wifi_rssi}", flush=True)
    return True


def fire(dev, args, res):
    """Reset the restart watch, fire the webhook, return its fire-time (or None on failure)."""
    t_fire = time.monotonic()
    reset_restart_watch(dev, args._serial)
    try:
        status, _ = fire_webhook(args.restart_url, args.restart_method, args.restart_timeout)
        print(f"  webhook {args.restart_method} {args.restart_url} -> {status}", flush=True)
    except Exception as e:
        res.check("webhook fired", False, str(e))
        return None
    return t_fire


def run_stick(dev: FuguDevice, mk: Markers, args, res: Results, rnd: int):
    print(f"\n--- stick round {rnd} ---", flush=True)
    if not baseline(dev, res):
        return
    rssi0 = dev.wifi_rssi
    t_fire = fire(dev, args, res)
    if t_fire is None:
        return

    # 1. the outage reached the device
    def outage_signal():
        if dev.wifi_rssi == 0:
            return "rssi->0"
        if any("connection timeout" in l or "Connecting WiFi" in l for l in mk.raw_since(t_fire)):
            return "reconnect attempt logged"
        return None
    outage = wait_for(outage_signal, args.outage_timeout)
    res.check("AP outage reached the device", bool(outage),
              outage or "rssi never dropped — webhook may not restart this device's AP")

    # 2+3. watch for reboot/crash and for reconnect, until reconnect or timeout
    landed = None
    reason = None
    deadline = time.monotonic() + args.reconnect_timeout
    while time.monotonic() < deadline:
        reason = bad_restart(dev, args._serial)
        if reason:
            break
        c = mk.connects_since(t_fire)
        if c:
            landed = c[-1][1]
            break
        if dev.wifi_rssi not in (0,) and dev.wifi_rssi > -90:
            landed = ""  # link healthy again, no connect line captured
            break
        time.sleep(0.3)

    res.check("device did NOT reboot/crash through the outage", reason is None, reason or "stable")
    ok_reconn = landed is not None and not reason
    res.check(f"reconnected within {args.reconnect_timeout:.0f}s", ok_reconn,
              f"ssid={landed!r} rssi={dev.wifi_rssi}" if ok_reconn else "no reassociation")

    # 4. the stick assertion
    if not ok_reconn:
        res.skip("reconnected to the same AP (stick)", "did not reconnect")
    elif args.ssid and landed:
        res.check("reconnected to the same AP (stick)", landed == args.ssid,
                  f"landed on {landed!r}, expected {args.ssid!r}")
    else:
        close = abs(dev.wifi_rssi - rssi0) <= args.rssi_tol
        res.check("reconnected to a comparable AP (rssi proximity)", close,
                  f"rssi {dev.wifi_rssi} vs baseline {rssi0} (tol {args.rssi_tol})")


def run_roam(dev: FuguDevice, mk: Markers, args, res: Results, rnd: int):
    print(f"\n--- roam round {rnd} ---", flush=True)
    if not baseline(dev, res):
        return
    # must start on --ssid
    c0 = mk.connects_since(0)
    if c0 and c0[-1][1] != args.ssid:
        return res.check(f"baseline on --ssid ({args.ssid})", False,
                         f"on {c0[-1][1]!r}; pre-condition not met")

    t_fire = fire(dev, args, res)
    if t_fire is None:
        return

    # 1. outage reached the device
    dropped = wait_for(lambda: dev.wifi_rssi == 0, args.outage_timeout)
    res.check("AP outage reached the device", bool(dropped),
              "rssi->0" if dropped else f"rssi still {dev.wifi_rssi}")

    # 2. must NOT roam to --other-ssid before switch_delay elapses (that would be a stick failure)
    early_deadline = t_fire + args.switch_delay * 0.8
    while time.monotonic() < early_deadline:
        if any(s == args.other_ssid for _, s in mk.connects_since(t_fire)):
            break
        time.sleep(0.3)
    early = [s for _, s in mk.connects_since(t_fire) if s == args.other_ssid]
    res.check("did NOT roam before switch_delay elapsed", not early,
              f"saw connect to {args.other_ssid} too early" if early else "")

    # 3. after switch_delay the device must roam to --other-ssid
    deadline = t_fire + args.switch_delay + args.roam_timeout
    landed = wait_for(
        lambda: next((s for _, s in mk.connects_since(t_fire) if s == args.other_ssid), None),
        max(0.0, deadline - time.monotonic()))
    res.check(f"roamed to {args.other_ssid} after switch_delay", landed is not None,
              f"connects since outage: {[s for _, s in mk.connects_since(t_fire)]}")

    # 4. no reboot/crash through the window
    reason = bad_restart(dev, args._serial)
    res.check("did NOT reboot/crash through the outage", reason is None, reason or "stable")

    # 5. sanity: the outage really outlasted switch_delay, else the roam path wasn't exercised
    if landed is not None:
        actual = time.monotonic() - t_fire
        res.check("outage long enough to exercise the roam path", actual >= args.switch_delay,
                  f"roamed after only {actual:.0f}s vs switch_delay={args.switch_delay:.0f}s")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("stick", "roam"), required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--serial", metavar="DEV", help="control console over serial (recommended)")
    g.add_argument("--telnet", metavar="IP:PORT", help="control console over telnet (must NOT route via the AP under test)")
    ap.add_argument("--restart-url", required=True, help="webhook that restarts/disables the AP")
    ap.add_argument("--restart-method", default="GET")
    ap.add_argument("--restart-timeout", type=float, default=10.0)
    ap.add_argument("--ssid", help="the AP under test; stick: reconnect must land here; roam: must start here")
    ap.add_argument("--outage-timeout", type=float, default=30.0, help="wait for the link to drop (s)")
    ap.add_argument("--rounds", type=int, default=1)
    # stick-only
    ap.add_argument("--reconnect-timeout", type=float, default=120.0, help="[stick] allow this long to reconnect (s)")
    ap.add_argument("--rssi-tol", type=int, default=15, help="[stick] dB tolerance vs baseline when no connect line")
    # roam-only
    ap.add_argument("--other-ssid", help="[roam] fallback SSID the device must roam to")
    ap.add_argument("--switch-delay", type=float, default=30.0, help="[roam] wifi.conf::switch_delay (s)")
    ap.add_argument("--down-seconds", type=float, default=60.0, help="[roam] how long the webhook keeps the AP down")
    ap.add_argument("--roam-timeout", type=float, default=60.0, help="[roam] allow this long past switch_delay to roam")
    args = ap.parse_args()

    if args.mode == "roam" and not (args.ssid and args.other_ssid):
        ap.error("--mode roam requires --ssid and --other-ssid")

    args._serial = bool(args.serial)
    transport = SerialTransport(args.serial) if args.serial else SocketTransport(*_hostport(args.telnet))
    mk = Markers()
    dev = FuguDevice(transport, block=False)
    dev.on_message = mk.feed
    print(f"mode={args.mode}  control={'serial ' + args.serial if args.serial else 'telnet ' + args.telnet}"
          + (f"  ssid={args.ssid}" if args.ssid else ""))
    res = Results()
    runner = run_stick if args.mode == "stick" else run_roam
    try:
        if not wait_for(lambda: dev.pwm_state.ccm is not None, 30):
            print("device not responding on the control console", flush=True)
            return 1
        for rnd in range(1, args.rounds + 1):
            runner(dev, mk, args, res, rnd)
    finally:
        dev.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


def _hostport(s):
    host, _, port = s.partition(":")
    return (host, int(port)) if port else (host,)


if __name__ == "__main__":
    sys.exit(main())
