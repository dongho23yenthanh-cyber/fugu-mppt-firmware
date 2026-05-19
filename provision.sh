#!/bin/bash
set -e

FOLDER="config"
BOARD="$1"

if [[ -d "$BOARD" ]] && [[ "${BOARD:0:7}" = config/ ]]; then
  BOARD="${BOARD:7}"
fi


echo "BOARD=$BOARD"

if [[ ! -d "$FOLDER/$BOARD" ]] || [[ "" == "$BOARD" ]]; then
  echo "invalid board '$BOARD', choose from"
  ls $FOLDER/
  exit 1
fi

PINS_CONF="$FOLDER/$BOARD/conf/pins.conf"
if [[ -f "$PINS_CONF" ]]; then
  MCU=$(grep -E '^mcu=' "$PINS_CONF" | head -n1 | cut -d= -f2)
  if [[ -n "$MCU" ]] && [[ -n "$IDF_TARGET" ]] && [[ "$MCU" != "$IDF_TARGET" ]]; then
    echo "ERROR: pins.conf mcu='$MCU' does not match IDF_TARGET='$IDF_TARGET'"
    exit 1
  fi
fi

if [[ -z "$ESPPORT" ]]; then
  echo "ERROR: ESPPORT is not set"
  exit 1
fi
littlefs-python create $FOLDER/"$BOARD" $FOLDER/"$BOARD".bin -v --fs-size=0x20000 --name-max=64 --block-size=4096
littlefs-python  list $FOLDER/"$BOARD".bin --block-size=4096

echo parttool.py --port $ESPPORT write_partition --partition-name littlefs --input $FOLDER/"$BOARD".bin
parttool.py --port $ESPPORT write_partition --partition-name littlefs --input $FOLDER/"$BOARD".bin