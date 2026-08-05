#!/usr/bin/env bash
# Deploy the BLE->InfluxDB telemetry bridge to the rpi (run from the repo root):
#   ./etc/deploy_ble_bridge_rpi.sh [rpi.local]
# ONE service (fugu-ble-bridge) that discovers every fugu/NUS device in range, streams its
# binary telemetry, and feeds decoded points to the EXISTING influxdb-udp-relay on
# 127.0.0.1:8086 (which owns batching + InfluxDB auth).
set -e
RPI=${1:-rpi.local}
DIR=/opt/fugu-ble-bridge

ssh "$RPI" "sudo mkdir -p $DIR/fugu && sudo chown -R \$USER $DIR"
scp -q etc/influx_binary_proxy.py "$RPI:$DIR/"
rsync -aq --exclude .git --exclude __pycache__ etc/fugu/ "$RPI:$DIR/fugu/"
# BLE stack: fl4p/bluek (kernel-direct, shadows bleak; see influx_binary_proxy.py). Shipped from
# the local checkout — it's WIP and may carry unpushed fixes. bleak stays installed as fallback.
BLUEK=${BLUEK_DIR:-$HOME/dev/ha/home-assistant-addons/bluek}
rsync -aq --exclude __pycache__ --exclude '*.egg-info' "$BLUEK/bluek/" "$RPI:$DIR/bluek/"
ssh "$RPI" "python3 -m venv $DIR/venv 2>/dev/null || true; $DIR/venv/bin/pip -q install bleak tamp pyserial paho-mqtt"

# drop any old per-device units
ssh "$RPI" 'for u in $(systemctl list-unit-files "fugu-ble-bridge-*.service" --no-legend 2>/dev/null | cut -d" " -f1); do sudo systemctl disable --now $u; sudo rm -f /etc/systemd/system/$u; done; true'

ssh "$RPI" "sudo tee /etc/systemd/system/fugu-ble-bridge.service >/dev/null" <<EOF
[Unit]
Description=Fugu BLE telemetry bridge (all devices)
After=bluetooth.target influxdb-udp-relay.service

[Service]
WorkingDirectory=$DIR
# bluetoothd keeps BLE connections alive when the previous bridge process died; a connected
# device stops advertising and can never be re-discovered — drop stale fugu links first.
ExecStartPre=-/bin/sh -c 'bluetoothctl devices Connected | grep -i fugu | cut -d" " -f2 | xargs -rn1 timeout 10 bluetoothctl disconnect'
ExecStart=$DIR/venv/bin/python3 $DIR/influx_binary_proxy.py --ble-all --forward-udp 127.0.0.1:8086
Restart=always
RestartSec=15

[Install]
WantedBy=multi-user.target
EOF
ssh "$RPI" "sudo systemctl daemon-reload && sudo systemctl enable fugu-ble-bridge && sudo systemctl restart fugu-ble-bridge && sleep 15 && journalctl -u fugu-ble-bridge --no-pager -n 8 -o cat"
