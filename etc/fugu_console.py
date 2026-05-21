#!/usr/bin/env python3
"""Console client + exerciser for the Fugu MPPT firmware.

Talks the device's string command protocol (the same one served on UART/USB-CDC/telnet/MQTT/BLE,
see `doc/Console.md`) over any transport. Three modes: a single command (`-c`), an interactive
REPL (`-i`), or — the default — a test PLAN that walks every console command in a meaningful order
(read-only diagnostics, config round-trips and service ops, then the converter/PWM commands that
move power) and reports PASS/FAIL/SKIP. The transport and the line-console mechanics live in the
`fugu` package (`fugu.transport`, `fugu.console.Console`); this file is the CLI and the plan.
Defaults to serial; `--ble`/`--ip` select BLE or TCP/telnet.

The PWM/charger group is gated behind `--mock`. A mock build (fake ADC, no real switching —
`config/lab/*_mock`, sensor.conf using ADC_Fake) can run the full set safely; on real hardware
those commands can destroy the switches or disrupt an active charge, so they are skipped by
default and reported as SKIPPED. NVS-mutating and reboot/flash commands (wifi, hostname, ota,
restart) always require an explicit opt-in flag.

Requires `pyserial` (serial) and/or `bleak` (BLE):  pip install pyserial bleak

Examples:
    python etc/fugu_console.py --mock                  # full sweep over serial against a mock build
    python etc/fugu_console.py                           # safe subset against real hardware
    python etc/fugu_console.py -p /dev/cu.usbmodem1101 --mock
    python etc/fugu_console.py --ble --mock              # same sweep over BLE
    python etc/fugu_console.py --ip 192.168.4.2          # over TCP/telnet
    python etc/fugu_console.py -c "svc list"             # run one command, print the reply
    python etc/fugu_console.py -i                        # interactive REPL
"""

import argparse
import glob
import os
import sys

try:  # works both as `python etc/fugu_console.py` and `python -m etc.fugu_console`
    from fugu.transport import SerialTransport, SocketTransport, BleTransport
    from fugu.console import Console
except ImportError:
    from etc.fugu.transport import SerialTransport, SocketTransport, BleTransport
    from etc.fugu.console import Console

_PORT_GLOBS = [
    "/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*", "/dev/cu.SLAB_USBtoUART*",
    "/dev/ttyUSB*", "/dev/ttyACM*",
]


def autodetect_port() -> str:
    if os.environ.get("ESPPORT"):
        return os.environ["ESPPORT"]
    for pat in _PORT_GLOBS:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    sys.exit("no serial port found; pass --port or set $ESPPORT")


# Test plan. Each step: (command, expect_substr | None, group, tolerate_reject)
#   group "always"   : safe read-only / non-destructive, run everywhere
#   group "mock"     : drives PWM or the live charger; only with --mock
#   group "net"      : mutates NVS / Wi-Fi / reboots; only with --include-network
#   tolerate_reject  : the firmware declining is an acceptable outcome — either the final-else
#                      REJECT marker, or an early `return false` that prints a warning but no OK
#                      (e.g. no fan / no panel switch / wrong topology for this hardware).
GROUP_ALWAYS, GROUP_MOCK, GROUP_NET = "always", "mock", "net"

# Per-command timeout overrides (seconds). The default 4 s is plenty for most commands; the I2C
# bus scan is slower, more so amid the mock's ADC-timeout chatter.
TIMEOUT_OVERRIDE = {"scan-i2c": 12.0}

PLAN = [
    # --- read-only diagnostics --------------------------------------------------------------
    ("mem", "Free heap", GROUP_ALWAYS, False),
    ("sensor", "Sensor", GROUP_ALWAYS, False),
    ("rt-stats", None, GROUP_ALWAYS, False),
    ("reset-lag", None, GROUP_ALWAYS, False),
    ("ip", "IP Address", GROUP_ALWAYS, False),
    ("scan-i2c", None, GROUP_ALWAYS, True),       # may report no devices on a mock
    ("svc list", "NAME", GROUP_ALWAYS, False),
    # --- config: dump, then a non-destructive round-trip on a scratch file ------------------
    ("get-config board.conf", "board.conf", GROUP_ALWAYS, False),
    ("get-config converter.conf", "converter.conf", GROUP_ALWAYS, False),
    ("set-config selftest.conf probe 4242", None, GROUP_ALWAYS, False),
    ("get-config selftest.conf probe", "4242", GROUP_ALWAYS, False),
    # --- harmless actuators ------------------------------------------------------------------
    ("led 030", None, GROUP_ALWAYS, False),
    ("fan 30", None, GROUP_ALWAYS, True),         # declined if no fan is configured (e.g. mock)
    ("fan 0", None, GROUP_ALWAYS, True),
    ("led 000", None, GROUP_ALWAYS, False),
    # --- service management: log level + restart (reversible; skip if the service isn't built) -
    ("svc log scope info", None, GROUP_ALWAYS, True),
    ("svc restart scope", None, GROUP_ALWAYS, True),
    # --- ADC backend re-init (brief; safe on a mock) ----------------------------------------
    ("adc-restart", None, GROUP_MOCK, True),
    ("adc-reset", None, GROUP_MOCK, True),
    # --- charger limit overrides (live params) ----------------------------------------------
    ("vset 28.5", None, GROUP_MOCK, False),
    ("iset 10", None, GROUP_MOCK, False),
    ("speed 1.0", None, GROUP_MOCK, False),
    # --- PWM / converter: enter manual mode, exercise switches, return to tracking ----------
    ("dc 0", None, GROUP_MOCK, False),            # switches to manual PWM at zero duty
    ("+5", None, GROUP_MOCK, False),
    ("-5", None, GROUP_MOCK, False),
    ("sync on", None, GROUP_MOCK, False),
    ("sync off", None, GROUP_MOCK, False),
    ("sync forced", None, GROUP_MOCK, False),
    ("sync off", None, GROUP_MOCK, False),
    ("bf 1", None, GROUP_MOCK, True),             # rejected if no backflow switch configured
    ("bf 0", None, GROUP_MOCK, True),
    ("short-ls", None, GROUP_MOCK, True),         # only valid in boost with Vin~0
    ("dc 0", None, GROUP_MOCK, False),
    ("mppt", None, GROUP_MOCK, False),            # back to tracking (valid only in manual mode)
    ("sweep", None, GROUP_MOCK, False),
    # --- network / NVS / reboot (opt-in only) -----------------------------------------------
    ("wifi on", None, GROUP_NET, False),
    ("hostname fugu-test", None, GROUP_NET, False),
    # `ota <url>` and `wifi off`/`wifi-add` intentionally omitted: they flash/reboot or wipe NVS.
]


def run_plan(con: Console, mock: bool, include_net: bool):
    results = []  # (cmd, status, note)  status in {PASS, FAIL, SKIP}
    for cmd, expect, group, tolerate in PLAN:
        if group == GROUP_MOCK and not mock:
            results.append((cmd, "SKIP", "drives PWM/charger — needs --mock"))
            continue
        if group == GROUP_NET and not include_net:
            results.append((cmd, "SKIP", "mutates NVS/Wi-Fi — needs --include-network"))
            continue

        reply = con.command(cmd, timeout=TIMEOUT_OVERRIDE.get(cmd, 4.0))

        if reply.ok:
            if expect is not None and expect not in reply.text:
                status, note = "FAIL", f"expected {expect!r} in reply"
            else:
                status, note = "PASS", ""
        elif tolerate:
            # explicit reject, or an early `return false` (warning, no OK) — both acceptable here
            status, note = "SKIP", "declined by firmware (not applicable on this setup)"
        elif reply.rejected:
            status, note = "FAIL", "rejected"
        elif reply.timed_out and not reply:
            status, note = "FAIL", "no response (timeout)"
        else:
            status, note = "FAIL", "no OK confirmation"

        results.append((cmd, status, note))
        flag = {"PASS": "ok  ", "FAIL": "FAIL", "SKIP": "skip"}[status]
        print(f"[{flag}] {cmd:<28} {note}")
        for ln in reply:
            print("        " + ln)

    print("\n" + "=" * 60)
    npass = sum(1 for r in results if r[1] == "PASS")
    nfail = sum(1 for r in results if r[1] == "FAIL")
    nskip = sum(1 for r in results if r[1] == "SKIP")
    print(f"summary: {npass} passed, {nfail} failed, {nskip} skipped")
    if nfail:
        print("failed:", ", ".join(r[0] for r in results if r[1] == "FAIL"))
    return nfail


def interactive(con: Console):
    print("interactive console — type commands, Ctrl-C / EOF to quit")
    while True:
        try:
            cmd = input("fugu> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not cmd:
            continue
        for ln in con.command(cmd):
            print("  " + ln)


def make_transport(args):
    if args.ble:
        print(f"scanning for BLE NUS (name contains {args.name!r}) …")
        return BleTransport(name=args.name, address=args.address)
    if args.ip:
        print(f"connecting to {args.ip}:23 (telnet)")
        return SocketTransport(args.ip)
    port = args.port or autodetect_port()
    print(f"opening {port} @ {args.baud}")
    return SerialTransport(port, baud=args.baud, timeout=0.2)


def main():
    ap = argparse.ArgumentParser(description="Console exerciser for Fugu MPPT firmware")
    ap.add_argument("-p", "--port", default=None, help="serial port (default: $ESPPORT or autodetect)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--ble", action="store_true", help="use the BLE NUS transport instead of serial")
    ap.add_argument("--name", default="fugu", help="BLE advertised-name substring filter (with --ble)")
    ap.add_argument("--address", help="BLE address to connect to (with --ble)")
    ap.add_argument("--ip", help="use TCP/telnet to this address instead of serial")
    ap.add_argument("--mock", action="store_true",
                    help="device runs a mock setup (fake ADC, no real PWM) — enables the PWM/charger commands")
    ap.add_argument("--include-network", action="store_true",
                    help="also run network/NVS-mutating commands (wifi on, hostname)")
    ap.add_argument("-c", "--command", help="send a single command, print the reply, exit")
    ap.add_argument("-i", "--interactive", action="store_true", help="interactive REPL")
    args = ap.parse_args()

    print(f"({'MOCK' if args.mock else 'REAL-HARDWARE'} mode)")
    try:
        con = Console(make_transport(args))
    except Exception as e:
        print(e)
        return 1
    try:
        if args.command:
            for ln in con.command(args.command):
                print(ln)
            return 0
        if args.interactive:
            interactive(con)
            return 0

        print("waiting for device to be ready …")
        if not con.wait_ready():
            print("device did not respond to 'mem' — wrong port/address, baud, or still booting?")
            return 1
        print("device ready.\n")
        return 1 if run_plan(con, args.mock, args.include_network) else 0
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
