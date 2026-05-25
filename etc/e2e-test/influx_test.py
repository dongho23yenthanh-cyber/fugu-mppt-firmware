#!/usr/bin/env python3
"""InfluxDB telemetry test for the Fugu MPPT firmware.

Verifies that the device emits well-formed InfluxDB line-protocol points over UDP. The flow
(see `test/vibe tests.md`, `## influxdb`):

  1. open a UDP socket on this host listening on port 8086 (the InfluxDB UDP port the firmware
     flushes to, hard-coded in `src/tele/telemetry.cpp::influxWritePointsUDP`)
  2. (optional) `./provision.py config/lab/dry_mock` to flash the mock board config
  3. over the serial console, point the device at this host:
       set-config tele.conf influxdb_host <this-host-LAN-ip>
       set-config tele.conf enabled 1
  4. reboot the device (tele.influxdbHost is read once at boot, in setup())
  5. wait for WiFi + NTP time-sync, then collect the UDP datagrams and validate every line

The device only flushes once WiFi is connected AND time is synced (`MpptController::telemetry`
gates on `timeSynced`), so WiFi credentials must already be provisioned (wifi.conf / NVS). If no
data arrives, that gate is the first thing to check.

Uses `fugu.console.Console` over a serial transport. Requires pyserial.

After collecting, the test also stops the telemetry service (`svc off tele`) and asserts the
device goes silent — no more datagrams arrive once the producer and flush task are gated. It
then re-enables the service to leave the device as it found it. Skip with `--no-stop-test`.

Examples:
    python etc/e2e-test/influx_test.py --mock                       # auto-detect port + LAN IP
    python etc/e2e-test/influx_test.py --host 192.168.1.50          # force the advertised host IP
    python etc/e2e-test/influx_test.py --provision config/lab/dry_mock
    python etc/e2e-test/influx_test.py --duration 20                # collect for 20 s after sync
    python etc/e2e-test/influx_test.py --no-stop-test               # skip the svc-off silence check
"""

import argparse
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo/etc (fugu pkg + fugu_console)
from fugu.console import Console  # noqa: E402
from fugu.transport import SerialTransport  # noqa: E402
from fugu_console import autodetect_port  # noqa: E402

UDP_PORT = 8086
MEASUREMENT = "mppt"
# Fields the firmware always populates for an enabled converter (telemetry() in src/mppt.cpp).
EXPECTED_FIELDS = {"I", "Ui", "Uo", "P", "P_smooth", "E", "mppt_state"}


def lan_ip() -> str:
    """Best-effort primary LAN IPv4 of this host (no traffic actually leaves)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def _split_unescaped(s: str, sep: str) -> list[str]:
    """Split on `sep` honouring line-protocol backslash escapes."""
    out, cur, esc = [], [], False
    for ch in s:
        if esc:
            cur.append(ch)
            esc = False
        elif ch == "\\":
            cur.append(ch)
            esc = True
        elif ch == sep:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


def _valid_field_value(v: str) -> bool:
    if v.endswith("i"):                       # integer field: digits + trailing 'i'
        body = v[:-1]
        return bool(body) and (body.lstrip("-").isdigit())
    if v in ("t", "T", "true", "True", "TRUE", "f", "F", "false", "False", "FALSE"):
        return True
    if len(v) >= 2 and v[0] == '"' and v[-1] == '"':   # quoted string
        return True
    try:                                      # float
        float(v)
        return True
    except ValueError:
        return False


def validate_line(line: str) -> list[str]:
    """Return a list of error strings for one line-protocol record ([] == valid)."""
    errs: list[str] = []
    parts = _split_unescaped(line, " ")
    if len(parts) < 2:
        return [f"too few space-separated parts ({len(parts)})"]
    key, fieldset = parts[0], parts[1]
    ts = parts[2] if len(parts) >= 3 else None

    # measurement + tags
    keytoks = _split_unescaped(key, ",")
    if keytoks[0] != MEASUREMENT:
        errs.append(f"measurement {keytoks[0]!r} != {MEASUREMENT!r}")
    tags = {}
    for t in keytoks[1:]:
        if "=" not in t:
            errs.append(f"malformed tag {t!r}")
            continue
        k, v = t.split("=", 1)
        tags[k] = v
    if "device" not in tags:
        errs.append("missing tag 'device'")

    # fields
    fields = {}
    for f in _split_unescaped(fieldset, ","):
        if "=" not in f:
            errs.append(f"malformed field {f!r}")
            continue
        k, v = f.split("=", 1)
        fields[k] = v
        if not _valid_field_value(v):
            errs.append(f"bad value for field {k!r}: {v!r}")
    if not fields:
        errs.append("no fields")

    # timestamp: ms precision => ~13 digits (1e12..1e13 ≈ 2001..2286)
    if ts is None:
        errs.append("missing timestamp")
    elif not ts.isdigit():
        errs.append(f"non-numeric timestamp {ts!r}")
    elif not (10 ** 12 <= int(ts) < 10 ** 13):
        errs.append(f"timestamp {ts!r} not plausible ms epoch (NTP not synced?)")

    return errs


def configure_and_reboot(con: Console, host: str) -> bool:
    print(f"configuring tele.conf influxdb_host = {host}")
    con.command(f"set-config tele.conf influxdb_host {host}")
    con.command("set-config tele.conf enabled 1")
    got = "\n".join(con.command("get-config tele.conf influxdb_host"))
    if host not in got:
        print(f"  ! get-config did not echo {host}:\n{got}")
        return False
    print("rebooting device …")
    con.command("restart", timeout=1.0)       # reboots; no OK to wait for
    return True


def collect(sock: socket.socket, duration: float, settle: float) -> list[str]:
    """Wait up to `settle` for the first datagram, then collect lines for `duration`."""
    sock.settimeout(settle)
    print(f"waiting up to {settle:.0f}s for first datagram (WiFi + NTP sync) …")
    try:
        first, addr = sock.recvfrom(65535)
    except socket.timeout:
        return []
    print(f"first datagram from {addr[0]}; collecting for {duration:.0f}s …")
    lines = first.decode("utf-8", "replace").splitlines()
    deadline = time.monotonic() + duration
    sock.settimeout(1.0)
    while time.monotonic() < deadline:
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        lines.extend(data.decode("utf-8", "replace").splitlines())
    return [ln for ln in lines if ln.strip()]


def verify_stopped(con: Console, sock: socket.socket, drain: float, quiet: float) -> bool:
    """Stop telemetry, drain in-flight datagrams, then require `quiet` seconds of silence.

    `svc off tele` gates both the producer (Service::tick skips onTick) and the flush task
    (telemetryFlushEnabled(false)), so the wire must fall silent. A few points may already be
    on the wire when the stop lands; absorb those during the drain window before asserting.
    Re-enables the service afterwards so the device is left as it was found.
    """
    print("\nstopping telemetry: svc off tele")
    con.command("svc off tele")

    sock.settimeout(0.5)
    deadline = time.monotonic() + drain
    drained = 0
    while time.monotonic() < deadline:
        try:
            sock.recvfrom(65535)
            drained += 1
        except socket.timeout:
            pass
    print(f"drained {drained} in-flight datagram(s); requiring silence for {quiet:.0f}s …")

    sock.settimeout(quiet)
    ok = True
    try:
        data, addr = sock.recvfrom(65535)
        snippet = data.decode("utf-8", "replace").splitlines()[:1]
        print(f"FAIL: datagram from {addr[0]} after stop: {snippet}")
        ok = False
    except socket.timeout:
        print(f"PASS: no telemetry for {quiet:.0f}s after stopping the service")

    con.command("svc on tele")   # restore prior state
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description="InfluxDB UDP telemetry test for Fugu MPPT firmware")
    ap.add_argument("-p", "--port", default=None, help="serial port (default: $ESPPORT or autodetect)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--host", default=None, help="advertised host IP (default: this host's LAN IP)")
    ap.add_argument("--mock", action="store_true", help="device runs a mock setup (informational)")
    ap.add_argument("--provision", metavar="BOARD", default=None,
                    help="run ./provision.py BOARD first (e.g. config/lab/dry_mock)")
    ap.add_argument("--settle", type=float, default=60.0, help="seconds to wait for first packet")
    ap.add_argument("--duration", type=float, default=15.0, help="seconds to collect after first packet")
    ap.add_argument("--no-reboot", action="store_true",
                    help="skip set-config/reboot; just listen (device already configured)")
    ap.add_argument("--no-stop-test", action="store_true",
                    help="skip the post-collect 'svc off tele' silence check")
    ap.add_argument("--stop-drain", type=float, default=3.0,
                    help="seconds to absorb in-flight datagrams after svc off")
    ap.add_argument("--stop-quiet", type=float, default=8.0,
                    help="seconds of required silence after the drain window")
    args = ap.parse_args()

    host = args.host or lan_ip()
    serial_port = args.port or autodetect_port()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("0.0.0.0", UDP_PORT))
    except OSError as e:
        print(f"cannot bind UDP :{UDP_PORT} — {e} (is an InfluxDB/telegraf already listening?)")
        return 1
    print(f"listening for InfluxDB line protocol on udp/{UDP_PORT}, advertising host {host}")

    if args.provision:
        print(f"provisioning {args.provision} …")
        try:
            subprocess.run(["./provision.py", args.provision], check=True)
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            print(f"provision failed: {e}")
            return 1

    con = Console(SerialTransport(serial_port, baud=args.baud, timeout=0.2))
    stop_ok = True
    try:
        if not args.no_reboot:
            print(f"opening console on {serial_port}")
            if not con.wait_ready():
                print("device did not respond to 'mem' — wrong port/baud, or still booting?")
                return 1
            if not configure_and_reboot(con, host):
                return 1
            con.wait_ready(timeout=45.0)      # back from reboot; WiFi/NTP may still be pending
        lines = collect(sock, args.duration, args.settle)
        if lines and not args.no_stop_test:
            stop_ok = verify_stopped(con, sock, args.stop_drain, args.stop_quiet)
    finally:
        con.close()
        sock.close()

    if not lines:
        print("\nFAIL: no UDP datagrams received.")
        print("  check: WiFi credentials provisioned? device IP on this LAN? tele.conf enabled=1?")
        print("  the firmware only flushes once timeSynced (NTP) — give it more time via --settle.")
        return 1

    bad = 0
    seen_fields: set[str] = set()
    measurements: set[str] = set()
    for ln in lines:
        errs = validate_line(ln)
        head = _split_unescaped(ln, " ")
        measurements.add(_split_unescaped(head[0], ",")[0])
        if len(head) >= 2:
            for f in _split_unescaped(head[1], ","):
                seen_fields.add(f.split("=", 1)[0])
        if errs:
            bad += 1
            if bad <= 10:
                print(f"[FAIL] {ln}")
                for e in errs:
                    print(f"        - {e}")

    missing = EXPECTED_FIELDS - seen_fields
    print("\n" + "=" * 60)
    print(f"received {len(lines)} line(s); measurements seen: {sorted(measurements)}")
    print(f"distinct fields: {len(seen_fields)} ({', '.join(sorted(seen_fields))})")
    if missing:
        print(f"WARN: expected fields not seen: {sorted(missing)}")
    if bad:
        print(f"FAIL: {bad}/{len(lines)} line(s) failed line-protocol validation")
        return 1
    if not stop_ok:
        print("FAIL: telemetry kept arriving after the service was stopped")
        return 1
    print(f"PASS: all {len(lines)} line(s) valid; device went silent after svc off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
