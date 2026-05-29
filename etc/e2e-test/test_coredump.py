#!/usr/bin/env python3
"""E2E regression test for the on-flash coredump path.

Exercises the whole chain on a real device (a bench unit — this *deliberately panics* it):

  1. ``coredump info``         -> baseline (usually ``none``)
  2. ``crash <null|abort|stack>`` -> deliberate panic; the firmware saves a dump to the
                                  ``coredump`` flash partition during the panic handler
  3. reboot                    -> ``coredump info`` reports the dump (``check=ok``)
  4. ``coredump get``          -> base64 stream over the console, decoded here to a .bin
  5. symbolicate               -> ``esp-coredump`` (full: registers + task stacks; needs the
                                  *exact* build ELF) and ``addr2line`` on the panic backtrace
                                  (SHA-independent fallback, always works)
  6. ``coredump erase``        -> back to ``none``

It also asserts the dump was saved cleanly: **no "Double exception"** during save and a
healthy ``bytes left free`` on the coredump stack (regression for COREDUMP_STACK_SIZE being
too small — at 1024 the ELF dump left 8 bytes and double-faulted; 2304 leaves ~1192).

The USB-CDC port number changes on every reboot, so the console is re-discovered between
steps by probing each ``/dev/cu.usbmodem*`` with ``uptime``.

Requires: pyserial; an ESP-IDF env on PATH for ``esp-coredump`` / ``xtensa-esp32s3-elf-addr2line``
(run after ``. ./idf-export.sh``). Run from the repo root so the default ELF path resolves.

  python etc/e2e-test/test_coredump.py                 # autodetect port, crash=null
  python etc/e2e-test/test_coredump.py --crash stack   # stack-overflow path
  python etc/e2e-test/test_coredump.py --port /dev/cu.usbmodem1201 --elf build/fugu-firmware.elf
"""
import argparse, base64, glob, re, subprocess, sys, time

import serial  # pyserial


def ports():
    return sorted(glob.glob('/dev/cu.usbmodem*'))


def find_console(prefer=None, timeout=25):
    """Return (Serial, port) for a usbmodem port that answers `uptime`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for p in ([prefer] if prefer else []) + ports():
            if not p:
                continue
            try:
                s = serial.Serial(p, 115200, timeout=1)
            except Exception:
                continue
            try:
                time.sleep(0.2)
                s.reset_input_buffer()
                s.write(b"uptime\n")
                buf = b""
                t = time.time() + 2
                while time.time() < t:
                    buf += s.read(2048)
                if re.search(r'Uptime:|OK: uptime', buf.decode('utf-8', 'replace')):
                    return s, p
            except Exception:
                pass
            s.close()
        time.sleep(1)
    return None, None


def command(s, c, wait=8.0):
    """Send `c`, collect reply lines until 'OK: c' / 'ERR: c' / timeout."""
    s.reset_input_buffer()
    s.write((c + "\n").encode())
    ok, err = "OK: " + c, "ERR: " + c
    buf, lines = "", []
    t = time.time() + wait
    while time.time() < t:
        buf += s.read(4096).decode('utf-8', 'replace')
        while "\n" in buf:
            ln, buf = buf.split("\n", 1)
            ln = ln.rstrip("\r")
            lines.append(ln)
            if ok in ln:
                return lines, True
            if err in ln:
                return lines, False
    return lines, None


def coredump_lines(lines, key):
    return [l for l in lines if key in l]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="serial port (default: autodetect usbmodem)")
    ap.add_argument("--crash", default="null", choices=["null", "abort", "stack"],
                    help="which deliberate panic to induce (default null)")
    ap.add_argument("--elf", default="build/fugu-firmware.elf", help="firmware ELF for symbolication")
    ap.add_argument("--out", default="/tmp/coredump.bin", help="where to write the decoded dump")
    ap.add_argument("--a2l", default="xtensa-esp32s3-elf-addr2line", help="addr2line binary")
    args = ap.parse_args()

    fails = []

    def check(cond, msg):
        print(("  PASS " if cond else "  FAIL ") + msg)
        if not cond:
            fails.append(msg)

    s, p = find_console(args.port)
    if not s:
        print("FAIL: no console found"); return 1
    print(f"console on {p}")
    ln, ok = command(s, "coredump info")
    print("  pre-crash:", coredump_lines(ln, "coredump:"))
    check(ok, "`coredump info` recognised (firmware is coredump-capable)")

    # --- crash, capture the live panic ---
    print(f"\n=== crash {args.crash} ===")
    s.reset_input_buffer()
    s.write(f"crash {args.crash}\n".encode())
    buf = ""
    t = time.time() + 7
    while time.time() < t:
        buf += s.read(4096).decode('utf-8', 'replace')
    s.close()
    check("Save core dump to flash" in buf or "esp_core_dump" in buf, "panic saved a core dump")
    check("Double exception" not in buf, "no Double-exception during dump save")
    m = re.search(r'used .* bytes on stack\. (\d+) bytes left free', buf)
    if m:
        free = int(m.group(1))
        print(f"  coredump stack: {free} bytes free")
        check(free > 64, f"coredump stack has healthy margin ({free} B free)")
    pcs = []
    for l in buf.splitlines():
        mb = re.search(r'Backtrace:(.*)', l)
        if mb:
            pcs = re.findall(r'0x4[0-9a-f]{7}', mb.group(1))

    # --- reboot + retrieve ---
    print("\n=== reboot + retrieve ===")
    time.sleep(3)
    s, p = find_console()
    if not s:
        print("FAIL: no console after reboot"); return 1
    ln, ok = command(s, "coredump info")
    info = coredump_lines(ln, "coredump:")
    print("  post-crash:", info)
    check(any("present" in l and "check=ok" in l for l in info), "dump present with check=ok")

    # crash-time stamp: the field is always printed; it is only filled once the clock is SNTP-synced
    # (so a no-network bench unit legitimately shows crashed=0). If filled, it must be recent.
    mts = re.search(r"crashed=(\d+)", " ".join(info))
    check(mts is not None, "`coredump info` reports a crashed= field")
    if mts:
        ts = int(mts.group(1))
        if ts == 0:
            print("  crashed=0 (clock not synced — expected without network)")
        else:
            age = time.time() - ts
            print(f"  crashed={ts} ({age:.0f}s ago)")
            check(0 <= age < 1800, f"crash timestamp is recent ({age:.0f}s ago)")

    ln, ok = command(s, "coredump get", wait=40.0)
    size = None
    b64, cap = [], False
    for l in ln:
        mm = re.search(r"==COREDUMP-BEGIN size=(\d+)==", l)
        if mm:
            size, cap, b64 = int(mm.group(1)), True, []
            continue
        if "==COREDUMP-END==" in l:
            cap = False
            continue
        if cap and re.fullmatch(r"[A-Za-z0-9+/=]+", l.strip()):
            b64.append(l.strip())
    check(size is not None, "`coredump get` streamed a dump")
    if size is not None:
        data = base64.b64decode("".join(b64))
        open(args.out, "wb").write(data)
        check(len(data) == size, f"decoded size matches header ({len(data)}=={size})")
        print(f"  wrote {len(data)} bytes -> {args.out}")

    # --- symbolicate ---
    print("\n=== esp-coredump (full; needs exact ELF) ===")
    r = subprocess.run(["esp-coredump", "info_corefile", "--core-format", "raw", "-c", args.out, args.elf],
                       capture_output=True, text=True)
    if r.returncode == 0:
        crashed = [l for l in r.stdout.splitlines() if "Crashed task" in l]
        print("  ", crashed[0] if crashed else "(parsed OK)")
        check(True, "esp-coredump parsed the dump")
    else:
        why = (r.stdout + r.stderr).strip().splitlines()[-1:] or ["?"]
        print("  esp-coredump skipped:", why[0])  # SHA mismatch if ELF != flashed build (not a hard fail)

    print("\n=== addr2line backtrace (SHA-independent) ===")
    if pcs:
        out = subprocess.run([args.a2l, "-pfiC", "-e", args.elf] + pcs, capture_output=True, text=True).stdout
        for l in out.splitlines()[:8]:
            print("  ", l)
        check(any("/src/" in l or ".cpp:" in l for l in out.splitlines()), "addr2line resolved app symbols")
    else:
        print("  (no backtrace PCs captured)")

    # --- erase ---
    print("\n=== erase ===")
    ln, ok = command(s, "coredump erase")
    check(ok and any("erased" in l for l in coredump_lines(ln, "coredump:")), "`coredump erase` ok")
    ln, _ = command(s, "coredump info")
    check(any("none" in l for l in coredump_lines(ln, "coredump:")), "dump gone after erase")
    s.close()

    print("\n" + ("FAILED: " + "; ".join(fails) if fails else "ALL PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
