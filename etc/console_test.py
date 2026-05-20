#!/usr/bin/env python3
"""Serial-console exerciser for the Fugu MPPT firmware.

Drives the device's string command protocol (the same one served on UART/USB-CDC/telnet/MQTT,
see `doc/Console.md`) over a serial port and walks every console command in a meaningful order:
read-only diagnostics first, then config round-trips and service ops, then the converter/PWM
commands that actually move power.

The PWM/charger group is gated behind `--mock`. A mock build (fake ADC, no real switching —
`config/lab/*_mock`, sensor.conf using ADC_Fake) can run the full set safely; on real hardware
those commands can destroy the switches or disrupt an active charge, so they are skipped by
default and reported as SKIPPED. NVS-mutating and reboot/flash commands (wifi, hostname, ota,
restart) always require an explicit opt-in flag.

Requires `pyserial`:  pip install pyserial

Examples:
    python etc/console_test.py --mock                 # full sweep against a mock build
    python etc/console_test.py                          # safe subset against real hardware
    python etc/console_test.py -p /dev/cu.usbmodem1101 --mock
    python etc/console_test.py -c "service list"        # run one command, print the reply
    python etc/console_test.py -i                       # interactive REPL
"""

import argparse
import glob
import os
import queue
import re
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
_PORT_GLOBS = [
    "/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*", "/dev/cu.SLAB_USBtoUART*",
    "/dev/ttyUSB*", "/dev/ttyACM*",
]
REJECT = "unknown or unexpected command"


def autodetect_port() -> str:
    if os.environ.get("ESPPORT"):
        return os.environ["ESPPORT"]
    for pat in _PORT_GLOBS:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    sys.exit("no serial port found; pass --port or set $ESPPORT")


class SerialConsole:
    """Line-oriented wrapper around the serial console.

    A background thread reads the port continuously (the firmware streams status lines between
    commands), strips ANSI, and feeds whole lines into a queue. `command()` sends one command and
    collects reply lines until it sees the firmware's `OK: <cmd>` confirmation, a rejection, or a
    timeout.
    """

    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baudrate=baud, timeout=0.2)
        self._buf = ""
        self._lines: "queue.Queue[str]" = queue.Queue()
        self._stop = threading.Event()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(256)
            except (serial.SerialException, OSError):
                break
            if not chunk:
                continue
            self._buf += _ANSI.sub("", chunk.decode("utf-8", "replace"))
            while "\n" in self._buf:
                line, self._buf = self._buf.split("\n", 1)
                self._lines.put(line.rstrip("\r"))

    def _drain(self):
        while True:
            try:
                self._lines.get_nowait()
            except queue.Empty:
                return

    def command(self, cmd: str, timeout: float = 4.0) -> list[str]:
        """Send `cmd`, return reply lines up to and including the OK/reject marker (or timeout)."""
        self._drain()
        self.ser.write((cmd + "\r\n").encode())
        self.ser.flush()
        ok_marker = "OK: " + cmd
        out: list[str] = []
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                line = self._lines.get(timeout=remaining)
            except queue.Empty:
                break
            out.append(line)
            if ok_marker in line or REJECT in line:
                break
        return out

    def wait_ready(self, timeout: float = 30.0) -> bool:
        """Poll `mem` until the firmware answers — covers the boot/ADC-calibration window."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any("OK: mem" in ln for ln in self.command("mem", timeout=2.0)):
                return True
        return False

    def close(self):
        self._stop.set()
        try:
            self.ser.close()
        except Exception:
            pass


# Test plan. Each step: (command, expect_substr | None, group, tolerate_reject)
#   group "always"   : safe read-only / non-destructive, run everywhere
#   group "mock"     : drives PWM or the live charger; only with --mock
#   group "net"      : mutates NVS / Wi-Fi / reboots; only with --include-network / --restart
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
    ("service list", "NAME", GROUP_ALWAYS, False),
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
    ("service log scope info", None, GROUP_ALWAYS, True),
    ("service restart scope", None, GROUP_ALWAYS, True),
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


def run_plan(con: SerialConsole, mock: bool, include_net: bool):
    results = []  # (cmd, status)  status in {PASS, FAIL, SKIP}
    for cmd, expect, group, tolerate in PLAN:
        if group == GROUP_MOCK and not mock:
            results.append((cmd, "SKIP", "drives PWM/charger — needs --mock"))
            continue
        if group == GROUP_NET and not include_net:
            results.append((cmd, "SKIP", "mutates NVS/Wi-Fi — needs --include-network"))
            continue

        reply = con.command(cmd, timeout=TIMEOUT_OVERRIDE.get(cmd, 4.0))
        joined = "\n".join(reply)
        got_ok = ("OK: " + cmd) in joined
        got_reject = REJECT in joined

        if got_ok:
            # confirmed; verify expected content if any
            if expect is not None and expect not in joined:
                status, note = "FAIL", f"expected {expect!r} in reply"
            else:
                status, note = "PASS", ""
        elif not reply:
            status, note = "FAIL", "no response (timeout)"
        elif tolerate:
            # explicit REJECT, or an early `return false` (warning, no OK) — both acceptable here
            status, note = "SKIP", "declined by firmware (not applicable on this setup)"
        elif got_reject:
            status, note = "FAIL", "rejected"
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


def interactive(con: SerialConsole):
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


def main():
    ap = argparse.ArgumentParser(description="Serial console exerciser for Fugu MPPT firmware")
    ap.add_argument("-p", "--port", default=None, help="serial port (default: $ESPPORT or autodetect)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--mock", action="store_true",
                    help="device runs a mock setup (fake ADC, no real PWM) — enables the PWM/charger commands")
    ap.add_argument("--include-network", action="store_true",
                    help="also run network/NVS-mutating commands (wifi on, hostname)")
    ap.add_argument("-c", "--command", help="send a single command, print the reply, exit")
    ap.add_argument("-i", "--interactive", action="store_true", help="interactive REPL")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    print(f"opening {port} @ {args.baud} ({'MOCK' if args.mock else 'REAL-HARDWARE'} mode)")
    con = SerialConsole(port, args.baud)
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
            print("device did not respond to 'mem' — wrong port, baud, or still booting?")
            return 1
        print("device ready.\n")
        return 1 if run_plan(con, args.mock, args.include_network) else 0
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
