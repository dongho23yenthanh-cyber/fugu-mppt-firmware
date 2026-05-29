#!/usr/bin/env bash
# One bisect step: build current checkout, flash app+otadata (NOT littlefs, preserving fry's
# config), wait for boot, run the ADC channel-mask probe.
# Exit codes for `git bisect run`: 0=good, 1=bad, 125=skip (build fail / inconclusive).
set -uo pipefail
cd /Users/fab/dev/pv/fugu-mppt-firmware
. ./idf-export.sh >/dev/null 2>&1
PORT="${ESPPORT:-/dev/cu.usbmodem1201}"
# Disable networking so the (pre-6f02574) WiFi stack-overflow crash loop can't mask the ADC under test.
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;netw_off.frag"
echo "###### BISECT STEP @ $(git rev-parse --short HEAD) : $(git log -1 --format=%s) ######"

# free the serial port from any lingering console reader (specific match; avoid self-kill)
pkill -f "fugu_console.py -p $PORT" 2>/dev/null; sleep 1

rm -f sdkconfig   # force regen so CONFIG_FUGU_WITH_NETW=n is applied at every checkout
idf.py build >/tmp/bisect_build.log 2>&1 || { echo "BUILD FAILED"; tail -25 /tmp/bisect_build.log; exit 125; }

python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset -p "$PORT" \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0xd000 build/ota_data_initial.bin 0x10000 build/fugu-firmware.bin >/tmp/bisect_flash.log 2>&1 \
  || { echo "FLASH FAILED"; tail -15 /tmp/bisect_flash.log; exit 125; }

sleep 14   # boot
for attempt in 1 2; do
  out=$(bash etc/adc_probe.sh "$PORT"); rc=$?
  echo "$out"
  [[ $rc -ne 2 ]] && exit $rc
  echo "(inconclusive, retry $attempt)"
done
exit 125
