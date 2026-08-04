#!/usr/bin/env python3
"""Read the device's littlefs partition over serial and unpack it to a directory.

Usage:
    ./etc/dump_littlefs.py [outdir]              # read from $ESPPORT, unpack
    ./etc/dump_littlefs.py -p /dev/cu.usb... cfg
    ./etc/dump_littlefs.py --input fs.bin cfg    # unpack an image, no serial

Env:
    ESPPORT               - serial port (unless -p/--input given)
    LITTLEFS_BLOCK_SIZE   - default 4096
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

try:
    from littlefs import LittleFS
except ImportError:
    sys.exit("littlefs-python missing: .venv/bin/pip install littlefs-python "
             "(and run this with .venv/bin/python3)")


def read_partition(port: str, name: str, out: Path):
    parttool = shutil.which("parttool.py") or "parttool.py"
    cmd = ([sys.executable, parttool] if os.name == "nt" else [parttool]) + [
        "--port", port, "read_partition", "--partition-name", name, "--output", str(out)]
    print(" ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        sys.exit("parttool.py not found - source ESP-IDF first: . ./idf-export.sh")


def unpack(img: bytes, block_size: int, dest: Path):
    if len(img) % block_size:
        sys.exit(f"image size {len(img)} is not a multiple of block size {block_size}")
    fs = LittleFS(block_size=block_size, block_count=len(img) // block_size, mount=False)
    fs.context.buffer = bytearray(img)
    fs.mount()
    n = 0
    for root, _dirs, files in fs.walk("/"):
        for f in files:
            src = f"{root.rstrip('/')}/{f}"
            dst = dest / src.lstrip("/")
            dst.parent.mkdir(parents=True, exist_ok=True)
            with fs.open(src, "rb") as fh:
                dst.write_bytes(fh.read())
            print(f"{src} -> {dst} ({dst.stat().st_size} B)")
            n += 1
    print(f"{n} file(s) extracted to {dest}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir", nargs="?", default="littlefs-dump",
                    help="destination directory (default: littlefs-dump)")
    ap.add_argument("-p", "--port", default=os.environ.get("ESPPORT"), help="serial port")
    ap.add_argument("--partition", default="littlefs", help="partition name (default: littlefs)")
    ap.add_argument("--input", help="unpack this image instead of reading from serial")
    ap.add_argument("--keep-image", metavar="PATH", help="where to keep the raw image "
                                                         "(default: <outdir>.bin)")
    ap.add_argument("--block-size", type=int,
                    default=int(os.environ.get("LITTLEFS_BLOCK_SIZE", 4096)))
    args = ap.parse_args()

    if args.input:
        img_path = Path(args.input)
    else:
        if not args.port:
            sys.exit("no serial port: set ESPPORT or pass -p")
        img_path = Path(args.keep_image or (args.outdir.rstrip("/") + ".bin"))
        read_partition(args.port, args.partition, img_path)

    unpack(img_path.read_bytes(), args.block_size, Path(args.outdir))


if __name__ == "__main__":
    main()
