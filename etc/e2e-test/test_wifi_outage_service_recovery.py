#!/usr/bin/env python3
"""E2E test — WiFi-outage service recovery (serial-controlled router).

Regression test for the ``startEnabledAtBoot(networkUp)`` fix: when WiFi is down at
boot, network-requiring services (``tele``, ``ftp``, ``telnet``, ``scope``) must stay
``Stopped`` (not ``Failed``); when WiFi comes up later the self-heal in
``loopNetwork_task`` must bring them to ``Running``.

The host drives **both** ESP32s over their USB serial consoles:

  * the DUT (regular fugu firmware) on ``--dut``
  * the WiFi router (``fl4p/esp32_nat_router_extended`` fork) on ``--router``

Outage is simulated by changing the router's AP SSID to a decoy + restarting it,
which the DUT cannot associate to. Service recovery is verified two ways:

  1. ``svc`` on the DUT — every requiresNetwork service must report ``Running``
  2. behavioural — open TCP to the router's WAN IP : 23 (port-forwarded to the
     DUT's telnet service) and expect the ``Welcome to <hostname>`` banner

Two scenarios per round:

  * **A** outage *after* boot (DUT was up, AP goes away, AP returns)
  * **B** outage *during* boot (AP is already down when DUT boots; DUT comes up
    with services Stopped, AP returns, services self-heal)

Usage
-----
    python etc/e2e-test/test_wifi_outage_service_recovery.py \\
        --dut    /dev/cu.usbmodem21401 \\
        --router /dev/cu.usbmodem11401 \\
        --router-wan-ip 192.168.1.173 \\
        --ssid my-net --psk hunter2 \\
        --off-ssid my-net-off
"""

import argparse
import os
import re
import socket
import sys
import threading
import time

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc
sys.path.insert(0, ETC_DIR)
from fugu.transport import SerialTransport
from fugu.console import Console
from _harness import (Results, wait_until, EventLog,
                      RE_CONNECT, RE_STATUS_N_RSSI, BOOT_MARKER)

# `ip` reply: "Local IP Address: 192.168.4.5"
_IP_REPLY = re.compile(r"Local IP Address:\s*(\d+\.\d+\.\d+\.\d+)")

# Services that require network (per src/main.cpp registration order).
NETWORK_SERVICES = ("tele", "ftp", "telnet", "scope")


class Tap(EventLog):
    """Records status-line events from the DUT (rssi, N counter, connect, boot, welcome banner)."""

    def feed(self, line: str):
        super().feed(line)
        if m := RE_STATUS_N_RSSI.search(line):
            self.add("N", int(m.group(1)))
            self.add("rssi", int(m.group(2)))
        if m := RE_CONNECT.search(line):
            self.add("connect", m.group(1))
        if BOOT_MARKER in line:
            self.add("boot")
        if "Welcome to " in line:
            self.add("welcome", line)


# --- DUT ----------------------------------------------------------------------

class Dut:
    """Wraps fugu.Console for the DUT plus a Tap on the streamed status lines."""

    def __init__(self, port: str):
        self.tap = Tap()
        self.con = Console(SerialTransport(port), on_line=self.tap.feed)

    def wait_ready(self, timeout=30.0) -> bool:
        return self.con.wait_ready(timeout=timeout)

    def cmd(self, c: str, timeout=4.0):
        return self.con.command(c, timeout=timeout)

    def restart(self, boot_timeout=30.0) -> bool:
        t0 = time.monotonic()
        self.con.write("restart\r\n")
        # the device drops the serial briefly when it reboots; wait for the boot banner
        return wait_until(lambda: self.tap.saw("boot", since=t0), boot_timeout, poll=0.3) is not None

    def force_idle(self):
        # `dc 0` switches to manual PWM mode at zero duty, keeping the converter idle.
        self.cmd("dc 0", timeout=2.0)

    def ip(self) -> str:
        r = self.cmd("ip", timeout=4.0)
        for line in r:
            m = _IP_REPLY.search(line)
            if m:
                return m.group(1)
        raise RuntimeError(f"could not parse `ip` reply: {r.text!r}")

    def svc_list(self) -> "dict[str,str]":
        """Run `svc` and return {name: state}. State values: Running / Stopped / Failed."""
        r = self.cmd("svc", timeout=4.0)
        out: "dict[str,str]" = {}
        for line in r:
            # header line starts with "NAME"
            tok = line.split()
            if len(tok) < 2 or tok[0] == "NAME":
                continue
            name, state = tok[0], tok[1]
            if state in ("Running", "Stopped", "Failed"):
                out[name] = state
        return out

    def wifi_add(self, ssid: str, psk: str):
        self.cmd(f"wifi-add {ssid}:{psk}", timeout=4.0)

    def wait_associated(self, timeout: float) -> bool:
        t0 = time.monotonic()
        return wait_until(lambda: (self.tap.last("rssi", since=t0) or 0) != 0,
                          timeout) is not None

    def wait_disassociated(self, timeout: float) -> bool:
        t0 = time.monotonic()
        return wait_until(lambda: self.tap.last("rssi", since=t0) == 0,
                          timeout) is not None

    def close(self):
        self.con.close()


# --- Router -------------------------------------------------------------------

class Router:
    """esp32_nat_router_extended over its IDF console — raw send + settle.

    The IDF console doesn't emit fugu's "OK:/ERR:" markers, so we bypass
    Console.command(): write the line, wait for output to settle, capture what
    arrived for diagnostics. ``restart`` waits longer because the chip reboots.
    """

    READY_HINTS = ("Type 'help'", "esp32>", "ESP-ROM", "I (")

    def __init__(self, port: str):
        self._lock = threading.Lock()
        self.recent_lines: "list[str]" = []
        self.con = Console(SerialTransport(port), on_line=self._on_line)

    def _on_line(self, line: str):
        with self._lock:
            self.recent_lines.append(line)
            if len(self.recent_lines) > 500:
                self.recent_lines = self.recent_lines[-300:]

    def _clear(self):
        with self._lock:
            self.recent_lines.clear()

    def _send(self, cmd: str, settle: float = 1.0) -> "list[str]":
        self._clear()
        self.con.write(cmd + "\r\n")
        time.sleep(settle)
        with self._lock:
            return list(self.recent_lines)

    def wait_ready(self, timeout=30.0) -> bool:
        t0 = time.monotonic()
        # nudge the prompt; many ESP-IDF consoles only print after a newline
        self.con.write("\r\n")
        while time.monotonic() - t0 < timeout:
            with self._lock:
                if any(any(h in l for h in self.READY_HINTS) for l in self.recent_lines):
                    return True
            time.sleep(0.3)
            self.con.write("\r\n")
        return False

    def set_ap(self, ssid: str, psk: str):
        # esp32_nat_router_extended: `set_ap <ssid> <password>` — persists to NVS, applies on restart
        self._send(f"set_ap {ssid} {psk}", settle=1.5)

    def add_portmap(self, proto: str, ext_port: int, dut_ip: str, int_port: int):
        self._send(f"portmap add {proto} {ext_port} {dut_ip} {int_port}", settle=1.0)

    def restart(self, boot_timeout: float = 15.0) -> bool:
        self._clear()
        self.con.write("restart\r\n")
        # the chip drops serial during the reboot; wait for a fresh banner / prompt
        t0 = time.monotonic()
        while time.monotonic() - t0 < boot_timeout:
            time.sleep(0.5)
            with self._lock:
                if any(any(h in l for h in self.READY_HINTS) for l in self.recent_lines):
                    # one extra second so its WiFi has actually come back up
                    time.sleep(1.5)
                    return True
        return False

    def close(self):
        self.con.close()


# --- behavioral probe ---------------------------------------------------------

def telnet_probe(host: str, port: int, timeout: float = 5.0) -> str:
    """Open TCP <host>:<port>, read the first ~256 bytes of banner, return it."""
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.settimeout(timeout)
        chunks: "list[bytes]" = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and sum(len(c) for c in chunks) < 256:
            try:
                d = s.recv(256)
            except socket.timeout:
                break
            if not d:
                break
            chunks.append(d)
        return b"".join(chunks).decode("utf-8", "replace")


# --- result aggregator --------------------------------------------------------

# --- scenarios ----------------------------------------------------------------

def assert_services(dut: Dut, expected: "dict[str,str]", res: Results, label: str) -> bool:
    states = dut.svc_list()
    bad = {n: states.get(n, "<missing>") for n, exp in expected.items() if states.get(n) != exp}
    return res.check(f"{label}: {expected}", not bad,
                     "" if not bad else f"got {bad} (full: {states})")


def behavioural_telnet(host: str, port: int, expect: str, res: Results, label: str) -> bool:
    try:
        banner = telnet_probe(host, port, timeout=6.0)
    except Exception as e:
        return res.check(f"{label}: telnet probe {host}:{port}", False, f"{e!r}")
    return res.check(f"{label}: telnet banner contains {expect!r}", expect in banner,
                     f"banner={banner!r}")


def scenario_A_outage_after_boot(dut: Dut, router: Router, args, res: Results, dut_hostname: str):
    print("\n--- scenario A: outage AFTER boot ---", flush=True)
    res.check("A.pre: rssi != 0", dut.tap.last("rssi") != 0)
    assert_services(dut, {n: "Running" for n in NETWORK_SERVICES}, res, "A.pre: svc")

    t_outage = time.monotonic()
    router.set_ap(args.off_ssid, args.off_psk)
    router.restart()
    dropped = dut.wait_disassociated(args.outage_timeout)
    res.check(f"A: AP outage dropped DUT within {args.outage_timeout}s", dropped,
              f"rssi still {dut.tap.last('rssi')}" if not dropped else "")

    router.set_ap(args.ssid, args.psk)
    router.restart()
    associated = dut.wait_associated(args.reconnect_timeout)
    res.check(f"A: DUT reassociated within {args.reconnect_timeout}s", associated,
              f"rssi still {dut.tap.last('rssi')}" if not associated else "")

    res.check("A: DUT did not reboot through the outage",
              not dut.tap.saw("boot", since=t_outage))

    if associated:
        # give services a moment to self-heal
        time.sleep(2.0)
        assert_services(dut, {n: "Running" for n in NETWORK_SERVICES}, res, "A.post: svc")
        behavioural_telnet(args.router_wan_ip, 23, dut_hostname, res, "A.post")


def scenario_B_outage_during_boot(dut: Dut, router: Router, args, res: Results, dut_hostname: str):
    print("\n--- scenario B: outage DURING boot (regression case for the fix) ---", flush=True)

    # 1. AP is "off" before the DUT boots
    router.set_ap(args.off_ssid, args.off_psk)
    router.restart()

    # 2. reboot the DUT into a WiFi-less world
    t_boot = time.monotonic()
    rebooted = dut.restart(boot_timeout=30.0)
    res.check("B: DUT booted with AP down", rebooted, "no setup() banner seen")
    if not rebooted:
        return

    # let setup() finish (extra slack — `startEnabledAtBoot` is called late in setup)
    time.sleep(3.0)

    # 3. core regression check: network services are Stopped, NOT Failed
    states = dut.svc_list()
    bad_failed = {n: states[n] for n in NETWORK_SERVICES
                  if states.get(n) == "Failed"}
    res.check("B: network services NOT marked Failed at boot",
              not bad_failed,
              f"got {bad_failed}" if bad_failed else "")
    bad_running = {n: states[n] for n in NETWORK_SERVICES
                   if states.get(n) == "Running"}
    res.check("B: network services not Running while AP is down",
              not bad_running,
              f"got {bad_running}" if bad_running else "")
    # they should explicitly be Stopped
    assert_services(dut, {n: "Stopped" for n in NETWORK_SERVICES}, res, "B.no-wifi: svc")

    # 4. stable: no spontaneous reboot
    time.sleep(5.0)
    n_boots = len(dut.tap.events("boot", t_boot))
    res.check("B: DUT did not reboot itself while waiting", n_boots <= 1,
              f"n_boots={n_boots}")

    # 5. ensure converter idle (so wifiLoop() keeps retrying once AP is back)
    dut.force_idle()

    # 6. bring AP back
    router.set_ap(args.ssid, args.psk)
    router.restart()

    associated = dut.wait_associated(args.reconnect_timeout)
    res.check(f"B: DUT associated within {args.reconnect_timeout}s after AP came back",
              associated,
              f"rssi still {dut.tap.last('rssi')}" if not associated else "")

    if associated:
        time.sleep(2.0)
        assert_services(dut, {n: "Running" for n in NETWORK_SERVICES}, res, "B.post: svc")
        behavioural_telnet(args.router_wan_ip, 23, dut_hostname, res, "B.post")


# --- main ---------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dut", required=True, help="DUT serial port (e.g. /dev/cu.usbmodem...)")
    ap.add_argument("--router", required=True, help="NAT-router serial port")
    ap.add_argument("--router-wan-ip", required=True,
                    help="router's WAN-side IP (where telnet :23 is forwarded)")
    ap.add_argument("--ssid", required=True, help="real AP SSID (router uses this between rounds)")
    ap.add_argument("--psk", required=True, help="real AP password")
    ap.add_argument("--off-ssid", default="fugu-test-off",
                    help="decoy SSID used during outage windows (must NOT match wifi.conf)")
    ap.add_argument("--off-psk", default=None,
                    help="decoy psk (default: same as --psk; any non-empty value works)")
    ap.add_argument("--outage-timeout", type=float, default=30.0)
    ap.add_argument("--reconnect-timeout", type=float, default=90.0)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--scenario", choices=("both", "A", "B"), default="both")
    args = ap.parse_args()
    if args.off_psk is None:
        args.off_psk = args.psk

    res = Results()
    dut = Dut(args.dut)
    router = Router(args.router)
    try:
        print(f"DUT={args.dut}  router={args.router}  wan={args.router_wan_ip}  ssid={args.ssid}",
              flush=True)

        if not router.wait_ready(timeout=15):
            print("router not responding on its serial console", flush=True)
            return 2
        if not dut.wait_ready(timeout=30):
            print("DUT not responding on its serial console", flush=True)
            return 2

        # --- setup ---
        print("\n--- setup ---", flush=True)
        router.set_ap(args.ssid, args.psk)
        router.restart()
        res.check("setup: router back up after AP config", True)

        dut.wifi_add(args.ssid, args.psk)
        dut.force_idle()
        dut.restart(boot_timeout=30.0)
        if not dut.wait_associated(args.reconnect_timeout):
            res.check("setup: DUT associated to real AP", False, "no rssi after restart")
            return 1
        res.check("setup: DUT associated to real AP", True)

        dut_ip = dut.ip()
        print(f"  DUT IP on router = {dut_ip}", flush=True)
        router.add_portmap("TCP", 23, dut_ip, 23)

        # capture hostname for the behavioral probe (welcome banner is "Welcome to <host>")
        hostname_line = dut.tap.last("welcome") or ""
        m = re.search(r"Welcome to (\S+)", hostname_line)
        dut_hostname = m.group(1) if m else "fugu-"
        print(f"  expected banner contains: {dut_hostname!r}", flush=True)

        behavioural_telnet(args.router_wan_ip, 23, dut_hostname, res, "setup")

        # --- scenarios ---
        for rnd in range(1, args.rounds + 1):
            print(f"\n=== round {rnd}/{args.rounds} ===", flush=True)
            if args.scenario in ("both", "A"):
                scenario_A_outage_after_boot(dut, router, args, res, dut_hostname)
            if args.scenario in ("both", "B"):
                scenario_B_outage_during_boot(dut, router, args, res, dut_hostname)

    finally:
        # always restore the real AP so the bench doesn't sit dark after a failure
        try:
            print("\n--- cleanup: restoring real AP on router ---", flush=True)
            router.set_ap(args.ssid, args.psk)
            router.restart(boot_timeout=15.0)
        except Exception as e:
            print(f"  cleanup failed: {e}", flush=True)
        dut.close()
        router.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"), flush=True)
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
