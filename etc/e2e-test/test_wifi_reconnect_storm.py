#!/usr/bin/env python3
"""E2E regression test for the wifi-task stack overflow on reconnect (logging.cpp fix, 2026-05-29).

Once `enable_esp_log_to_telnet()` is active, the ESP-IDF `wifi` task's connect/reconnect logging
burst used to fan out through `vprintf_mux` (300 B `loc_buf` + telnet/MQTT/BLE mirror frames),
overflowing its 3072 B stack -> `***ERROR*** stack overflow in task wifi` -> reboot loop. The fix
routes the wifi task to the light default vprintf (UART only). This test exercises the exact path:
it forces repeated disconnect/reconnect cycles (`wifi off <min>` then `wifi on`) and asserts the
device **never crashes or reboots** through the reconnect logging bursts.

Serial only: `wifi off` takes the link down, so any telnet/BLE control path that routes through the
AP goes dark, and crash detection (`has_crashed`) needs the serial panic log. Runs on any device
that's currently associated — no router rig needed. `wifi off <min>` keeps the stored SSID and
auto-re-enables, so the device's saved network survives the run.

Usage
-----
    python etc/e2e-test/test_wifi_reconnect_storm.py --serial /dev/cu.usbmodem1201
    python etc/e2e-test/test_wifi_reconnect_storm.py --serial /dev/cu.usbmodem1201 --cycles 10
"""
import argparse
import os
import sys
import time

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc (holds the fugu pkg)
sys.path.insert(0, ETC_DIR)
from fugu.transport import SerialTransport
from fugu.fugu import FuguDevice
from _harness import Results, wait_for


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", metavar="DEV", required=True,
                    help="control console over serial (required: wifi off kills network transports,"
                         " and crash detection needs the serial panic log)")
    ap.add_argument("--cycles", type=int, default=5, help="disconnect/reconnect cycles (default 5)")
    ap.add_argument("--off-min", type=int, default=1, help="`wifi off <min>` value (kept-SSID path; default 1)")
    ap.add_argument("--drop-timeout", type=float, default=15.0, help="wait for the link to drop (s)")
    ap.add_argument("--settle", type=float, default=8.0, help="watch this long after each `wifi on` for a crash (s)")
    args = ap.parse_args()

    dev = FuguDevice(SerialTransport(args.serial), block=False)
    print(f"control=serial {args.serial}  cycles={args.cycles}", flush=True)
    res = Results()
    try:
        if not wait_for(lambda: dev.pwm_state.ccm is not None, 30):
            print("device not responding on the control console", flush=True)
            return 1
        associated = wait_for(lambda: dev.wifi_rssi != 0, 8)
        # association isn't required (a reconnect *attempt* logs on the wifi task either way), but note it
        res.check("baseline: device associated", bool(associated),
                  f"rssi={dev.wifi_rssi}" if associated else "rssi=0 (reconnect attempts still exercise the path)")
        dev.console.command("dc 0", timeout=2.0)  # idle the converter so a mock can actually reassociate
        dev.has_crashed(reset=True)
        dev.has_rebooted(reset=True)

        for i in range(1, args.cycles + 1):
            dev.console.command(f"wifi off {args.off_min}", timeout=4.0)
            wait_for(lambda: dev.wifi_rssi == 0, args.drop_timeout)  # link drops
            dev.console.command("wifi on", timeout=4.0)              # forces a connect burst on the wifi task
            # watch for a panic (passive serial marker) through the reconnect logging burst
            deadline = time.monotonic() + args.settle
            while time.monotonic() < deadline:
                if dev.has_crashed():
                    break
                time.sleep(0.3)
            bad = dev.has_crashed() or dev.has_rebooted()
            if not res.check(f"cycle {i}/{args.cycles}: no wifi-task overflow / reboot", not bad,
                             "crash/reboot detected" if bad else f"rssi={dev.wifi_rssi}"):
                break

        # the storm must leave the console responsive and the device un-rebooted
        res.check("device responsive after storm", dev.console.command("uptime", timeout=4.0).ok)
        res.check("no reboot across the whole storm", not dev.has_rebooted())
    finally:
        dev.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
