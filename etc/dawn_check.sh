#!/bin/bash
# Reports fry/flat dawn-start behavior from the havan console log: the "START blocked: <reason>"
# diagnostic emitted while idle in START (src/main.cpp lfStatusLine), plus the actual start
# (mppt: Start sweep). Run after dawn (~07:00 local). Console logger is UTC+1; the firmware sits
# idle as Voc climbs (~06:05–06:35 local), so the dawn window is 04:00–07:30 in the log's clock.
set -u
DAY="${1:-$(date +%Y-%m-%d)}"
LOG=pv/fugu_console.log
echo "######## dawn_check $(date '+%Y-%m-%d %H:%M:%S %z') ########"
for dev in fry flat; do
    echo "=== $dev  $DAY ==="
    ssh -o ConnectTimeout=10 havan.local \
        "grep -a '${dev}:' $LOG | grep -aE '${DAY} 0[4-7]:' \
         | grep -aiE 'START blocked|mppt: Start sweep|mppt: Stop sweep|backoff' \
         | head -60" 2>&1
    echo
done
