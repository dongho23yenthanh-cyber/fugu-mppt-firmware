#!/bin/bash
# Serial-flash with a named device, archiving the build ELF for later coredump
# symbolication. Thin wrapper: it just names the device, then the idf.py
# extension (idf_ext.py) does the archiving after `flash`/`app-flash`.
#
#   ./flash.sh <device-name> [extra idf.py args, e.g. -p /dev/cu.usbmodemXXX flash monitor]
#
# With no idf.py args it builds + flashes. You can also skip this wrapper and run
#   FUGU_DEVICE=<name> idf.py flash monitor
set -e

export FUGU_DEVICE="${1:?usage: ./flash.sh <device-name> [idf.py args]}"
shift || true

if ! command -v idf.py >/dev/null; then
  . ./idf-export.sh
fi

if [ "$#" -eq 0 ]; then
  idf.py build flash
else
  idf.py "$@"
fi
