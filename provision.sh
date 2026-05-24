#!/bin/bash
set -e

FOLDER="config"
BOARD="$1"

# Resolve the source config directory ($SRC). Accept either a board name under
# config/ (e.g. "fmetal") or an arbitrary path to a config dir holding conf/
# (e.g. "./etc/config-tool/dl/fry.local.192.168.4.2").
if [[ -d "$BOARD/conf" ]]; then
  SRC="${BOARD%/}"
elif [[ -d "$FOLDER/$BOARD" ]] && [[ -n "$BOARD" ]]; then
  SRC="$FOLDER/$BOARD"
else
  echo "invalid board '$BOARD', choose from"
  ls $FOLDER/
  exit 1
fi

echo "SRC=$SRC"

BOARD_CONF="$SRC/conf/board.conf"
if [[ -f "$BOARD_CONF" ]]; then
  MCU=$(grep -E '^mcu=' "$BOARD_CONF" | head -n1 | cut -d= -f2)
  if [[ -n "$MCU" ]] && [[ -n "$IDF_TARGET" ]] && [[ "$MCU" != "$IDF_TARGET" ]]; then
    echo "ERROR: board.conf mcu='$MCU' does not match IDF_TARGET='$IDF_TARGET'"
    exit 1
  fi
fi

if [[ -z "$ESPPORT" ]]; then
  echo "ERROR: ESPPORT is not set"
  exit 1
fi
BIN="$SRC.bin"
littlefs-python create "$SRC" "$BIN" -v --fs-size=0x20000 --name-max=64 --block-size=4096
littlefs-python  list "$BIN" --block-size=4096

echo parttool.py --port $ESPPORT write_partition --partition-name littlefs --input "$BIN"
parttool.py --port $ESPPORT write_partition --partition-name littlefs --input "$BIN"