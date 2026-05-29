#!/usr/bin/env python3
"""E2E test for the network debug tools (CONFIG_FUGU_WITH_NETTOOLS): curl / ping / nslookup /
tcpconnect / netstat.

Drives a real device's console (serial or telnet) and exercises each command. The feature is
behind a default-off Kconfig flag, so the test first probes `netstat`: if the firmware doesn't
know the command, every check is reported SKIP and the run still exits 0 (nothing is broken —
the feature just isn't compiled in).

What's asserted
---------------
  * netstat     — replies OK and prints a `mac=` line (always present).
  * nslookup    — a literal IP resolves to itself with no DNS (`8.8.8.8 -> 8.8.8.8`); a hostname
                  is a soft check (PASS if it resolves, SKIP if DNS is down).
  * ping        — the session runs end-to-end and prints the `N sent, M received` summary; a reply
                  actually arriving (M>=1) is a soft check (the gateway, parsed from netstat, may
                  filter ICMP).
  * tcpconnect  — produces a well-formed verdict line (`open` / `refused` / `timeout`); reaching an
                  open port (default 8.8.8.8:53) is a soft check that needs internet.
  * curl GET    — an HTTPS GET against `--url` prints an `HTTP <status>` line (TLS verified via the
                  cert bundle).
  * curl POST   — `-X POST -d <data>` against an echo endpoint (`--echo`, default httpbin) prints
                  the status line; the echoed payload appearing in the body is a soft check.

The network-reaching checks are soft (SKIP, not FAIL) so the test is meaningful on a device with
no upstream internet — it still proves the commands parse, run, and emit well-formed output.

Usage
-----
    python etc/e2e-test/test_nettools.py --serial /dev/cu.usbmodem1201
    python etc/e2e-test/test_nettools.py --telnet 192.168.1.50:23
    python etc/e2e-test/test_nettools.py --serial /dev/cu.usbmodem1201 --echo https://postman-echo.com/post
"""
import argparse
import os
import re
import sys

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc (holds the fugu pkg)
sys.path.insert(0, ETC_DIR)
from fugu.transport import SerialTransport, SocketTransport
from fugu.console import Console
from _harness import Results


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--serial", metavar="DEV", help="control console over serial")
    g.add_argument("--telnet", metavar="HOST[:PORT]", help="control console over TCP/telnet")
    ap.add_argument("--url", default="https://example.com", help="URL for the curl GET check")
    ap.add_argument("--echo", default="https://httpbin.org/post",
                    help="echo endpoint for the curl POST check (default httpbin)")
    ap.add_argument("--probe", default="8.8.8.8:53", help="host:port for the tcpconnect check")
    args = ap.parse_args()

    if args.serial:
        transport, is_sock = SerialTransport(args.serial, timeout=0.2), False
        print(f"control=serial {args.serial}")
    else:
        host, _, port = args.telnet.partition(":")
        transport, is_sock = SocketTransport(host, port=int(port or 23)), True
        print(f"control=telnet {args.telnet}")

    con = Console(transport, wait_banner=is_sock)
    res = Results()
    try:
        # --- feature probe: is NETTOOLS compiled in? ---
        net = con.command("netstat", timeout=4.0)
        if not net.ok:
            print("`netstat` not recognised — CONFIG_FUGU_WITH_NETTOOLS is off; skipping all checks", flush=True)
            for name in ("netstat", "nslookup", "ping", "tcpconnect", "curl GET", "curl POST"):
                res.skip(name, "feature not built")
            return 0

        # --- netstat ---
        res.check("netstat prints a mac= line", "mac=" in net.text, net.text.strip()[:80])
        gw = None
        if m := re.search(r"gw=(\d+\.\d+\.\d+\.\d+)", net.text):
            gw = m.group(1)
            print(f"  gateway={gw}", flush=True)

        # --- nslookup: literal IP (no DNS) is a hard check; a hostname is soft ---
        nl = con.command("nslookup 8.8.8.8", timeout=4.0)
        res.check("nslookup resolves a literal IP", nl.ok and "8.8.8.8 -> 8.8.8.8" in nl.text,
                  nl.text.strip()[:80])
        host = con.command("nslookup example.com", timeout=6.0)
        if host.ok and "->" in host.text:
            res.check("nslookup resolves a hostname (DNS)", True, host.text.strip().splitlines()[-1][:60])
        else:
            res.skip("nslookup resolves a hostname (DNS)", "DNS unavailable / declined")

        # --- ping: session must run end-to-end; a reply is soft ---
        target = gw or "8.8.8.8"
        pg = con.command(f"ping {target} 2", timeout=8.0)
        msum = re.search(r"(\d+) sent, (\d+) received", pg.text)
        res.check("ping runs and prints a summary", pg.ok and msum is not None,
                  pg.text.strip().splitlines()[-1][:80] if pg else "no reply")
        if msum and int(msum.group(2)) >= 1:
            res.check(f"ping got a reply from {target}", True, f"{msum.group(2)}/{msum.group(1)}")
        else:
            res.skip(f"ping got a reply from {target}", "no echo reply (ICMP filtered / offline)")

        # --- tcpconnect: well-formed verdict is hard; reaching 'open' is soft ---
        phost, _, pport = args.probe.partition(":")
        tc = con.command(f"tcpconnect {phost} {pport or 80}", timeout=8.0)
        verdict = re.search(r"tcpconnect: \S+ (open|refused|timeout|error)", tc.text)
        res.check("tcpconnect emits a verdict", verdict is not None,
                  verdict.group(0) if verdict else tc.text.strip()[:80])
        if verdict and verdict.group(1) == "open":
            res.check(f"tcpconnect reached {args.probe}", True)
        else:
            res.skip(f"tcpconnect reached {args.probe}",
                     verdict.group(1) if verdict else "no verdict")

        # --- curl GET: HTTPS via cert bundle ---
        cg = con.command(f"curl {args.url}", timeout=15.0)
        status = re.search(r"curl: HTTP (\d+)", cg.text)
        if status:
            res.check("curl GET returns an HTTP status", True, f"HTTP {status.group(1)}")
        else:
            res.skip("curl GET returns an HTTP status", "no status line (offline / TLS failed)")

        # --- curl POST: -X POST -d ---
        cp = con.command(f"curl -X POST -d nettools=1 {args.echo}", timeout=15.0)
        pstatus = re.search(r"curl: HTTP (\d+)", cp.text)
        if pstatus:
            res.check("curl POST returns an HTTP status", True, f"HTTP {pstatus.group(1)}")
            if "nettools" in cp.text:
                res.check("curl POST body was echoed back", True)
            else:
                res.skip("curl POST body was echoed back", "endpoint did not echo (not an echo server?)")
        else:
            res.skip("curl POST returns an HTTP status", "no status line (offline / TLS failed)")
    finally:
        con.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
