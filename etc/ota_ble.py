#!/usr/bin/env python3
"""Push a firmware image to a Fugu device over BLE (no WiFi).

Mirrors the trigger/poll structure of ota.py, but the host *pushes* the image over the BLE NUS link
instead of the device pulling it over HTTP. The device must run a WITH_BLE build with the `ble`
service enabled.

Protocol (device side in src/etc/ota_ble.cpp):
  host -> RX  : "otab begin <size> <sha256hex>\n"   arm + erase passive partition
  dev  -> TX  : "OTAB READY ..."                    erased, ready to receive
  dev  -> TX  : "OTAB CRED <G>"                      may stream up to cumulative byte offset G
  host -> FW  : raw firmware bytes (write-no-response), throttled to the credit window
  dev  -> TX  : "OTAB PROG <w>/<size>" ...           progress
  host -> RX  : "otab end\n"                         finalize
  dev  -> TX  : "OTAB OK rebooting" | "OTAB FAIL <reason>"

Usage:
    python -m etc.ota_ble [build/fugu-firmware.bin] [device-name-or-address]

Requires: pip install bleak
"""
import asyncio
import hashlib
import os
import re
import sys

from bleak import BleakClient, BleakScanner

NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write: console commands
TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify: status/log mirror
FW_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  # write-no-response: firmware bytes


def progress_bar(done, total, label, width=30):
    frac = done / total if total else 0
    filled = int(frac * width)
    bar = "#" * filled + "-" * (width - filled)
    sys.stdout.write(f"\r  {label} [{bar}] {frac * 100:5.1f}% {done}/{total}")
    sys.stdout.flush()
    if done >= total:
        sys.stdout.write("\n")
        sys.stdout.flush()


async def find_device(name_or_addr):
    if name_or_addr and re.fullmatch(r"[0-9A-Fa-f:\-]{11,}", name_or_addr):
        dev = await BleakScanner.find_device_by_address(name_or_addr, timeout=15)
    elif name_or_addr:
        dev = await BleakScanner.find_device_by_name(name_or_addr, timeout=15)
    else:
        # No name given: pick the first peripheral advertising the NUS service.
        dev = await BleakScanner.find_device_by_filter(
            lambda d, ad: NUS_SVC in (s.lower() for s in (ad.service_uuids or [])), timeout=15)
    return dev


async def push(bin_path, name_or_addr):
    data = open(bin_path, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    print(f"image {bin_path}: {len(data)} bytes, sha256={sha}")

    dev = await find_device(name_or_addr)
    if not dev:
        print("device not found (advertising? ble service enabled?)")
        return False
    print(f"connecting to {dev.name or dev.address} ...")

    granted = 0          # cumulative byte offset the device permits us to send up to
    credit_ev = asyncio.Event()
    done_ev = asyncio.Event()    # OK or FAIL received
    full_ev = asyncio.Event()    # device confirmed it has flushed the whole image (PROG == size)
    ready_ev = asyncio.Event()
    disc_ev = asyncio.Event()    # link dropped (device rebooting after a successful end)
    result = {"ok": False, "fail": False}
    rxbuf = ""

    def on_tx(_, payload: bytearray):
        nonlocal granted, rxbuf
        rxbuf += payload.decode("utf-8", "replace")
        while "\n" in rxbuf:
            line, rxbuf = rxbuf.split("\n", 1)
            if "OTAB READY" in line:
                print("  <", line.strip()); ready_ev.set()
            elif "OTAB CRED" in line:
                granted = int(line.split()[-1]); credit_ev.set()
            elif "OTAB PROG" in line:
                try:
                    w = int(line.split()[-1].split("/")[0])
                    progress_bar(w, len(data), "flush ")
                except ValueError:
                    print("  <", line.strip())
                if line.split()[-1] == f"{len(data)}/{len(data)}":
                    full_ev.set()
            elif "OTAB OK" in line:
                print("  <", line.strip()); result["ok"] = True; done_ev.set()
            elif "OTAB FAIL" in line:
                print("  <", line.strip()); result["fail"] = True; done_ev.set()

    async with BleakClient(dev, disconnected_callback=lambda _: disc_ev.set()) as cli:
        await cli.start_notify(TX_UUID, on_tx)
        chunk = max(cli.mtu_size - 3, 20)
        print(f"connected, mtu={cli.mtu_size}, chunk={chunk}")

        await cli.write_gatt_char(RX_UUID, b"ping\n", response=True)
        await cli.write_gatt_char(
            RX_UUID, f"otab begin {len(data)} {sha}\n".encode(), response=True)
        try:
            await asyncio.wait_for(ready_ev.wait(), timeout=30)  # waits out the partition erase
        except asyncio.TimeoutError:
            print("timeout waiting for READY"); return False

        sent = 0
        while sent < len(data):
            if sent >= granted:
                credit_ev.clear()
                try:
                    await asyncio.wait_for(credit_ev.wait(), timeout=20)
                except asyncio.TimeoutError:
                    print("timeout waiting for credit"); return False
                continue
            n = min(chunk, granted - sent, len(data) - sent)
            await cli.write_gatt_char(FW_UUID, data[sent:sent + n], response=False)
            sent += n
            progress_bar(sent, len(data), "upload")

        # Wait until the device has flushed the whole image to flash before finalizing — otherwise the
        # last write-no-response packets may still be in flight when `end` runs (device sees it short).
        try:
            await asyncio.wait_for(full_ev.wait(), timeout=30)
        except asyncio.TimeoutError:
            print("timeout waiting for full flush"); return False

        await cli.write_gatt_char(RX_UUID, b"otab end\n", response=True)
        # On success the device reboots right after queuing OTAB OK, so the notify usually never drains:
        # accept either an explicit OK or the link dropping (reboot) as success; only OTAB FAIL fails.
        await asyncio.wait([asyncio.create_task(done_ev.wait()),
                            asyncio.create_task(disc_ev.wait())],
                           timeout=35, return_when=asyncio.FIRST_COMPLETED)
        if result["fail"]:
            return False
        if not (result["ok"] or disc_ev.is_set()):
            print("timeout waiting for OK/disconnect"); return False

    # Verify the device came back up (mirrors ota.py's reconnect probe).
    print("waiting for device to come back online ...")
    for _ in range(20):
        await asyncio.sleep(2)
        d = await find_device(name_or_addr)
        if d:
            print("device is advertising again — OTA successful")
            return True
    print("device did not reappear")
    return False


def main():
    bin_path = sys.argv[1] if len(sys.argv) > 1 else "build/fugu-firmware.bin"
    name = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("BLE_NAME")
    if not os.path.exists(bin_path):
        print(f"no such file: {bin_path}"); return 1
    ok = asyncio.run(push(bin_path, name))
    print("OTA over BLE:", "✅ success (device rebooting)" if ok else "❌ failed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
