---
name: reference_creds_env_files
description: where InfluxDB + MQTT broker creds live (gitignored env files) for queries/console
metadata: 
  node_type: memory
  type: reference
  originSessionId: 7e1c4c73-968e-42dd-bcff-0648ab45af8e
---

Credentials live in **gitignored** env files under `etc/` (both listed in `.gitignore`):

- `etc/influx.env` — InfluxDB v1 `open_pe` (telemetry store; verification query source).
  `INFLUX_URL=https://influx.fabi.me:8086`, user `openpe`, db `open_pe`. Query:
  `curl -su "$INFLUX_USER:$INFLUX_PASS" -G "$INFLUX_URL/query" --data-urlencode db=$INFLUX_DB --data-urlencode "epoch=ms" --data-urlencode "q=SELECT ..."`
- `etc/mqtt.env` — broker `192.168.1.200:1882`, user `pv` (fugu_console `--mqtt`).

Source the file (`set -a; . etc/influx.env; set +a`) — don't hardcode secrets in scripts or commit them.
The relay host `192.168.1.200` is reachable over `ssh` from the 192.168.1.x network.
