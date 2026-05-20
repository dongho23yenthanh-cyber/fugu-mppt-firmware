#!/usr/bin/env python3
"""BLE console client for the Fugu MPPT firmware (Nordic UART Service).

Connects to the device's BleConsoleService over BLE and talks the same string command
protocol used on UART/telnet/MQTT. Useful for testing the BLE console without a phone or a
Web Bluetooth page.

Requires `bleak` (cross-platform BLE):  pip install bleak

Examples:
    python etc/ble_console.py                 # scan, connect, run a self-test
    python etc/ble_console.py --name fugu      # filter advertised name (default: "fugu")
    python etc/ble_console.py -c "get-config board.conf"   # run one command, print reply
    python etc/ble_console.py -i               # interactive REPL

Notes:
  * The RX (command) characteristic requires an encrypted link when the device runs with
    ble_security=justworks|passkey. On macOS/Windows the OS performs Just Works pairing
    transparently on first write; with ble_security=passkey the OS will prompt for the PIN.
  * UUIDs are the standard Nordic UART Service.
"""

import argparse
import asyncio
import re
import sys

_ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("bleak is required:  pip install bleak")

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write  (client -> device)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify (device -> client)


class BleConsole:
    """Line-oriented wrapper around the NUS characteristics."""

    def __init__(self, client: BleakClient):
        self.client = client
        self._buf = ""
        self._lines: asyncio.Queue[str] = asyncio.Queue()

    def _on_notify(self, _char, data: bytearray):
        self._buf += _ANSI.sub("", data.decode("utf-8", "replace"))
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            self._lines.put_nowait(line.rstrip("\r"))

    async def start(self):
        await self.client.start_notify(NUS_TX, self._on_notify)

    async def write(self, text: str):
        # chunk to the conservative 20-byte ATT payload (works pre-MTU-negotiation).
        # write-with-response: the RX char requires an encrypted write (justworks/passkey); some
        # stacks drop write-without-response on an encrypted characteristic.
        data = (text).encode()
        for off in range(0, len(data), 20):
            await self.client.write_gatt_char(NUS_RX, data[off:off + 20], response=True)

    async def command(self, cmd: str, timeout: float = 4.0) -> list[str]:
        """Send a command and collect reply lines until 'OK: <cmd>' / rejection or timeout."""
        # drain anything buffered (status lines etc.)
        while not self._lines.empty():
            self._lines.get_nowait()
        await self.write(cmd + "\r\n")
        out, ok_marker = [], "OK: " + cmd
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                break
            try:
                line = await asyncio.wait_for(self._lines.get(), remaining)
            except asyncio.TimeoutError:
                break
            out.append(line)
            if ok_marker in line or "unknown or unexpected command" in line:
                break
        return out


async def find_device(name: str, timeout: float):
    print(f"scanning for a device advertising NUS (name contains {name!r}) …")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, adv: (NUS_SERVICE in (adv.service_uuids or [])) or (name.lower() in (d.name or "").lower()),
        timeout=timeout,
    )
    return dev


async def run(args):
    dev = await find_device(args.name, args.scan_timeout)
    if dev is None:
        print("no matching device found — is it advertising? (service start ble)")
        return 1
    print(f"connecting to {dev.name}  [{dev.address}] …")
    # macOS may keep a stale bond after the device is reflashed/re-paired ("Peer removed pairing
    # information", CBError 14). The failed attempt usually makes the OS drop the stale bond, so retry.
    client = BleakClient(dev)
    for attempt in range(1, 4):
        try:
            await client.connect()
            break
        except Exception as e:
            print(f"  connect attempt {attempt} failed: {e}")
            if attempt == 3:
                print("give up. On macOS, forget the device in Bluetooth settings or `blueutil --unpair`.")
                return 1
            await asyncio.sleep(1.5)
    try:
        con = BleConsole(client)
        await con.start()
        print("connected.\n")

        if args.command:
            for line in await con.command(args.command):
                print(line)
            return 0

        if args.interactive:
            print("interactive console — type commands, Ctrl-C to quit")
            loop = asyncio.get_event_loop()
            while True:
                try:
                    cmd = await loop.run_in_executor(None, input, "ble> ")
                except (EOFError, KeyboardInterrupt):
                    print()
                    break
                if not cmd.strip():
                    continue
                for line in await con.command(cmd):
                    print(line)
            return 0

        # default: self-test a few read-only commands and check expected output
        checks = [
            ("mem", "Free heap"),
            ("get-config board.conf", "board.conf:mcu"),
            ("service list", "ble"),
        ]
        failures = 0
        for cmd, expect in checks:
            reply = await con.command(cmd)
            joined = "\n".join(reply)
            # the expected content in the reply is proof the command ran; the trailing "OK: <cmd>"
            # line often arrives after the window amid the streamed status lines, so don't require it.
            ok = expect in joined
            print(f"=== {cmd} === {'PASS' if ok else 'FAIL'} (expect {expect!r})")
            for line in reply:
                print("   " + line)
            print()
            failures += 0 if ok else 1
        print("SELF-TEST:", "all passed" if failures == 0 else f"{failures} failed")
        return 1 if failures else 0
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description="BLE NUS console client for Fugu MPPT")
    ap.add_argument("--name", default="fugu", help="advertised-name substring filter (default: fugu)")
    ap.add_argument("--scan-timeout", type=float, default=10.0, help="scan timeout seconds")
    ap.add_argument("-c", "--command", help="run a single command and print the reply")
    ap.add_argument("-i", "--interactive", action="store_true", help="interactive REPL")
    args = ap.parse_args()
    try:
        sys.exit(asyncio.run(run(args)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
