#!/usr/bin/env python3
"""Console client + exerciser for the Fugu MPPT firmware.

Talks the device's string command protocol (the same one served on UART/USB-CDC/telnet/MQTT/BLE,
see `doc/Console.md`) over any transport. Modes: a single command (`-c`), the test PLAN (`--test`,
walks every console command in a meaningful order — read-only diagnostics, config round-trips and
service ops, then the converter/PWM commands that move power — and reports PASS/FAIL/SKIP), or —
given a transport but no mode flag — an interactive REPL (the default). With *no arguments at all*
it scans every transport for reachable devices (serial port globs, mDNS scope/telnet hosts, the
NAT-forwarded telnet endpoints in `nat.env`, BLE NUS, and — when `$MQTT_HOST` is set — hostnames
seen on the broker's `pv/log/`) and prints what to pass to connect, without connecting. The transport and the line-console mechanics live in
the `fugu` package (`fugu.transport`, `fugu.console.Console`); this file is the CLI and the plan.
Defaults to serial; `--ble`/`--ip` select BLE or TCP/telnet.

The PWM/charger group is gated behind `--mock`. A mock build (fake ADC, no real switching —
`config/lab/*_mock`, sensor.conf using ADC_Fake) can run the full set safely; on real hardware
those commands can destroy the switches or disrupt an active charge, so they are skipped by
default and reported as SKIPPED. NVS-mutating and reboot/flash commands (wifi, hostname, ota,
restart) always require an explicit opt-in flag.

Requires `pyserial` (serial) and/or `bleak` (BLE):  pip install pyserial bleak

Examples:
    python etc/fugu_console.py                           # discover devices on all transports
    python etc/fugu_console.py -p /dev/cu.usbmodem1101   # interactive REPL over serial (default)
    python etc/fugu_console.py --ip 192.168.4.2          # interactive REPL over TCP/telnet
    python etc/fugu_console.py --ble                     # interactive REPL over BLE
    python etc/fugu_console.py --test --mock             # run the full command PLAN on a mock build
    python etc/fugu_console.py --ip 192.168.4.2 --test   # run the safe subset over TCP/telnet
    python etc/fugu_console.py --mqtt 192.168.1.134 --mqtt-port 1882 -c "svc list"   # over MQTT
    python etc/fugu_console.py --mqtt 192.168.1.134 --mqtt-readonly  # passive log monitor (REPL)
    python etc/fugu_console.py -c "svc list"             # run one command, print the reply
"""

import argparse
import asyncio
import glob
import os
import re
import sys
import time

try:  # works both as `python etc/fugu_console.py` and `python -m etc.fugu_console`
    from fugu.transport import SerialTransport, SocketTransport, BleTransport, MqttTransport
    from fugu.console import Console
    from fugu.discover import discover_scope_servers
except ImportError:
    from etc.fugu.transport import SerialTransport, SocketTransport, BleTransport, MqttTransport
    from etc.fugu.console import Console
    from etc.fugu.discover import discover_scope_servers

_PORT_GLOBS = [
    "/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*", "/dev/cu.SLAB_USBtoUART*",
    "/dev/ttyUSB*", "/dev/ttyACM*",
]


def load_env_file(path=None):
    """Populate os.environ from a KEY=VALUE file (default `mqtt.env` beside this script).

    Shell-set vars win — only keys not already in the environment are filled, so an explicit
    `MQTT_HOST=… fugu_console.py` or exported value overrides the file.
    """
    if path is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mqtt.env")
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, val = line.partition("=")
                os.environ.setdefault(key.strip(), val.strip())
    except FileNotFoundError:
        pass


def autodetect_port() -> str:
    if os.environ.get("ESPPORT"):
        return os.environ["ESPPORT"]
    for pat in _PORT_GLOBS:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    sys.exit("no serial port found; pass --port or set $ESPPORT")


def scan_serial():
    ports = []
    for pat in _PORT_GLOBS:
        ports.extend(sorted(glob.glob(pat)))
    return ports


_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
_WELCOME_RE = re.compile(r"Welcome to (\S+)")  # banner: "Welcome to <hostname> (192.168.4.2)"
_HOSTNAME_RE = re.compile(r"Hostname:\s*(\S+)")  # reply of the bare `hostname` command


def query_hostname(con: Console) -> str | None:
    """Ask the device for its hostname (the bare `hostname` command), or None on failure."""
    for ln in con.command("hostname", timeout=2.0):
        m = _HOSTNAME_RE.search(ln)
        if m:
            return m.group(1)
    return None


def nat_endpoints():
    """Telnet endpoints behind the NAT router, from `$NAT_TELNET` (comma-separated host:port).

    Filled from `nat.env` beside this script (see the device table in CLAUDE.md). Returns a list
    of (host, port); empty when unset.
    """
    out = []
    if not os.environ.get("NAT_TELNET"):
        load_env_file(os.path.join(os.path.dirname(os.path.abspath(__file__)), "nat.env"))  # NAT_TELNET
    for ep in (os.environ.get("NAT_TELNET") or "").split(","):
        ep = ep.strip()
        if not ep:
            continue
        host, _, port = ep.partition(":")
        out.append((host.strip(), int(port) if port else SocketTransport.DEFAULT_PORT))
    return out


async def probe_welcome(host, port, timeout=4.0):
    """Connect to a telnet endpoint and return the hostname from its welcome banner (or None)."""
    try:
        writer: asyncio.StreamWriter
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout)
    except (OSError, asyncio.TimeoutError):
        return None
    buf = b""
    deadline = time.monotonic() + timeout
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                chunk = await asyncio.wait_for(reader.read(1024), remaining)
            except (OSError, asyncio.TimeoutError):
                break
            if not chunk:
                break
            buf += chunk
            m = _WELCOME_RE.search(_ANSI.sub("", buf.decode("utf-8", "replace")))
            if m:
                return m.group(1)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except OSError:
            pass
    return None


async def scan_nat_async(timeout=4.0, reachable_only=False):
    """asyncio twin of `scan_nat`: probe all NAT endpoints concurrently.

    Returns a list of (host, port, hostname|None) for every configured endpoint (None = no
    banner / unreachable), or None when none are configured.
    """
    endpoints = nat_endpoints()
    if not endpoints:
        return []
    names = await asyncio.gather(
        *(probe_welcome(host, port, timeout) for host, port in endpoints))
    return [(host, port, name) for (host, port), name in zip(endpoints, names) if
            not reachable_only or name is not None]


def scan_nat(timeout=4.0):
    """Probe the NAT-forwarded telnet endpoints in `nat.env`, resolving each hostname.

    Returns a list of (host, port, hostname|None) for every configured endpoint (None = no
    banner / unreachable), or None when none are configured.
    """
    return asyncio.run(scan_nat_async(timeout))


def scan_telnet(timeout=2.0):
    """mDNS-advertised scope/telnet hosts; None if zeroconf isn't installed."""
    try:
        return sorted(set(discover_scope_servers(timeout=timeout)))
    except ImportError:
        return None


def scan_ble(timeout=5.0):
    """BLE peripherals advertising the NUS console; None if bleak isn't installed."""
    try:
        from bleak import BleakScanner
    except ImportError:
        return None
    nus = BleTransport.NUS_SERVICE

    async def _scan():
        found = []
        for dev, adv in (await BleakScanner.discover(timeout=timeout, return_adv=True)).values():
            if nus in (s.lower() for s in (adv.service_uuids or [])):
                found.append((dev.name or "?", dev.address))
        return found

    return asyncio.run(_scan())


def scan_mqtt(timeout=5.0):
    """ Returns Hostnames a broker has seen publishing under `pv/log/<hostname>`.

    Needs a broker (no default is compiled in): `$MQTT_HOST` (`$MQTT_PORT`/`$MQTT_USER`/`$MQTT_PASS`
    refine it). Returns None when no broker is configured or `paho-mqtt` isn't installed.
    """
    host = os.environ.get("MQTT_HOST")
    if not host:
        return None
    try:
        import paho.mqtt.client as mqtt
        from paho.mqtt.enums import CallbackAPIVersion
    except ImportError:
        return None
    hosts = set()

    def on_message(_c, _u, msg):
        parts = msg.topic.split("/")
        if len(parts) == 3:  # pv/log/<hostname>, not the deeper .../cmd echo
            hosts.add(parts[2])

    c = mqtt.Client(CallbackAPIVersion.VERSION2)
    user = os.environ.get("MQTT_USER", "pv")
    if user:
        c.username_pw_set(user, os.environ.get("MQTT_PASS") or "")
    c.on_message = on_message
    try:
        c.connect(host, int(os.environ.get("MQTT_PORT", "1883")), 60)
        c.subscribe(MqttTransport.LOG_ROOT + "#")
        c.loop_start()
        time.sleep(timeout)
        c.loop_stop()
        c.disconnect()
    except OSError as e:
        print(f"  (mqtt {host}: {e})")
        return []
    return sorted(hosts)


def discover_devices():
    """Scan every transport for reachable Fugu devices and print what to pass to connect.

    The scans block for different durations (serial is instant, mDNS ~2 s, BLE/MQTT ~5 s), so
    run them concurrently and join — total time is the slowest scan, not their sum.
    """
    from concurrent.futures import ThreadPoolExecutor
    print("scanning for Fugu devices on all transports …\n")

    with ThreadPoolExecutor(max_workers=5) as ex:
        f_serial = ex.submit(scan_serial)
        f_telnet = ex.submit(scan_telnet)
        f_ble = ex.submit(scan_ble)
        f_mqtt = ex.submit(scan_mqtt)
        f_nat = ex.submit(scan_nat)
        ports, hosts, devs, mqtt_hosts, nat = (
            f_serial.result(), f_telnet.result(), f_ble.result(), f_mqtt.result(),
            f_nat.result())

    print("serial:")
    for p in ports:
        print(f"  {p:<32} →  -p {p}")
    if not ports:
        print("  (none)")

    print("\ntelnet/scope (mDNS):")
    if hosts is None:
        print("  (skipped — `pip install zeroconf`)")
    else:
        for addr, port, name in hosts:
            print(f"  {name} {addr}:{port:<8} →  --ip {addr}")
        if not hosts:
            print("  (none)")

    print("\ntelnet via NAT (nat.env):")
    if nat is None:
        print("  (skipped — set $NAT_TELNET, or edit etc/nat.env)")
    else:
        for host, port, name in nat:
            label = name or "(unreachable)"
            print(f"  {label:<24} {host}:{port:<8} →  --ip {host}:{port}")
        if not nat:
            print("  (none)")

    print("\nBLE (NUS):")
    if devs is None:
        print("  (skipped — `pip install bleak`)")
    else:
        for name, address in devs:
            print(f"  {name:<24} {address}  →  --ble --address {address}")
        if not devs:
            print("  (none)")

    print("\nMQTT (pv/log):")
    if mqtt_hosts is None:
        print("  (skipped — set $MQTT_HOST, or pass --mqtt to connect)")
    else:
        broker = os.environ.get("MQTT_HOST")
        for h in mqtt_hosts:
            print(f"  {h:<24} →  --mqtt {broker} --name {h}")
        if not mqtt_hosts:
            print("  (none)")
    return 0


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
    ("scan-i2c", None, GROUP_ALWAYS, True),  # may report no devices on a mock
    ("svc list", "NAME", GROUP_ALWAYS, False),
    # --- config: dump, then a non-destructive round-trip on a scratch file ------------------
    ("get-config board.conf", "board.conf", GROUP_ALWAYS, False),
    ("get-config converter.conf", "converter.conf", GROUP_ALWAYS, False),
    ("set-config selftest.conf probe 4242", None, GROUP_ALWAYS, False),
    ("get-config selftest.conf probe", "4242", GROUP_ALWAYS, False),
    # --- harmless actuators ------------------------------------------------------------------
    ("led 030", None, GROUP_ALWAYS, False),
    ("fan 30", None, GROUP_ALWAYS, True),  # declined if no fan is configured (e.g. mock)
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
    ("dc 0", None, GROUP_MOCK, False),  # switches to manual PWM at zero duty
    ("+5", None, GROUP_MOCK, False),
    ("-5", None, GROUP_MOCK, False),
    ("sync on", None, GROUP_MOCK, False),
    ("sync off", None, GROUP_MOCK, False),
    ("sync forced", None, GROUP_MOCK, False),
    ("sync off", None, GROUP_MOCK, False),
    ("bf 1", None, GROUP_MOCK, True),  # rejected if no backflow switch configured
    ("bf 0", None, GROUP_MOCK, True),
    ("short-ls", None, GROUP_MOCK, True),  # only valid in boost with Vin~0
    ("dc 0", None, GROUP_MOCK, False),
    ("mppt", None, GROUP_MOCK, False),  # back to tracking (valid only in manual mode)
    ("sweep", None, GROUP_MOCK, False),
    # --- network / NVS / reboot (opt-in only) -----------------------------------------------
    ("wifi on", None, GROUP_NET, False),
    ("hostname fugu-test", None, GROUP_NET, False),
    # `ota <url>` and `wifi off`/`wifi-add` intentionally omitted: they flash/reboot or wipe NVS.
    # `wifi off <minutes>` (temporary, keeps the SSID) has its own test: e2e-test/test_wifi_off_timeout.py
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
    print("interactive console — type commands, Ctrl-C / EOF to quit (live output streams below)")
    hostname = query_hostname(con)
    prompt = f"{hostname}> " if hostname else "> "
    # Tap the reader's line stream so periodic status lines (and command replies) print as they
    # arrive, instead of being thrown away by command()'s drain() while we wait at the prompt.
    con.on_line = lambda ln: print(ln)
    try:
        while True:
            try:
                cmd = input(prompt).strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return
            if cmd:
                con.command(cmd)  # reply lines print via on_line
    finally:
        con.on_line = None


def make_transport(args):
    if args.ble:
        print(f"scanning for BLE NUS (name contains {args.name!r}) …")
        return BleTransport(name=args.name, address=args.address)
    if args.mqtt:
        ro = " (read-only)" if args.mqtt_readonly else ""
        print(f"connecting to MQTT broker {args.mqtt}:{args.mqtt_port}, device ~{args.name!r}{ro}")
        return MqttTransport(args.mqtt, port=args.mqtt_port,
                             username=args.mqtt_user, password=args.mqtt_pass,
                             device=args.name, writable=not args.mqtt_readonly)
    if args.ip:
        host, _, port = args.ip.partition(":")  # host:port for NAT-forwarded endpoints
        port = int(port) if port else SocketTransport.DEFAULT_PORT
        print(f"connecting to {host}:{port} (telnet)")
        return SocketTransport(host, port=port)
    port = args.port or autodetect_port()
    print(f"opening {port} @ {args.baud}")
    return SerialTransport(port, baud=args.baud, timeout=0.2)


def main():
    load_env_file()  # fill MQTT_* from etc/mqtt.env (shell-set vars still win)
    ap = argparse.ArgumentParser(description="Console client + exerciser for Fugu MPPT firmware")
    ap.add_argument("-p", "--port", default=None, help="serial port (default: $ESPPORT or autodetect)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--ble", action="store_true", help="use the BLE NUS transport instead of serial")
    ap.add_argument("--name", default="fugu",
                    help="device-name substring filter (with --ble: advertised name; with --mqtt: hostname)")
    ap.add_argument("--address", help="BLE address to connect to (with --ble)")
    ap.add_argument("--ip", help="use TCP/telnet to this address instead of serial")
    ap.add_argument("--mqtt", metavar="BROKER", help="use MQTT via this broker host instead of serial")
    ap.add_argument("--mqtt-port", type=int, default=int(os.environ.get("MQTT_PORT", "1883")),
                    help="MQTT broker port (default: $MQTT_PORT or 1883)")
    ap.add_argument("--mqtt-user", default=os.environ.get("MQTT_USER", "pv"), help="MQTT username")
    ap.add_argument("--mqtt-pass", default=os.environ.get("MQTT_PASS"), help="MQTT password")
    ap.add_argument("--mqtt-readonly", action="store_true",
                    help="read-only MQTT monitor: stream output, never publish commands")
    ap.add_argument("--mock", action="store_true",
                    help="device runs a mock setup (fake ADC, no real PWM) — enables the PWM/charger commands")
    ap.add_argument("--include-network", action="store_true",
                    help="also run network/NVS-mutating commands (wifi on, hostname)")
    ap.add_argument("-c", "--command", help="send a single command, print the reply, exit")
    ap.add_argument("--test", action="store_true",
                    help="run the PASS/FAIL/SKIP command PLAN instead of the interactive REPL")
    args = ap.parse_args()

    if len(sys.argv) == 1:  # no arguments: search every transport, don't connect
        return discover_devices()

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
        if args.test:
            print("waiting for device to be ready …")
            if not con.wait_ready():
                print("device did not respond to 'mem' — wrong port/address, baud, or still booting?")
                return 1
            print("device ready.\n")
            return 1 if run_plan(con, args.mock, args.include_network) else 0

        interactive(con)  # default
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
