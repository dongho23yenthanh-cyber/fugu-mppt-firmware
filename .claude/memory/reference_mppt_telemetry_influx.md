---
name: reference_mppt_telemetry_influx
description: "Where the firmware's `mppt` InfluxDB telemetry actually lives, how to query it, and its quirks"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 1707be4d-f479-4663-a7d2-6e1746fb3422
---

Firmware telemetry (measurement `mppt`, tag `device=fry|flat|...`) path:
firmware → UDP binary line-proto → havan:8086 (HA addon `influxdb-udp-relay`) → forwards to the
host in HA's `configuration.yaml` `influxdb:` block = **tm.fabi.me:8086 / db `ha_van`**
(user `$INFLUX_HA_VAN_USER`, pass `$INFLUX_HA_VAN_PASS` — `.claude/memory/secrets.env`). It is NOT in influx.fabi.me/open_pe (that only has
batmon/cells/smart_shunt from batmon-ha, plus an old bench `pwr_test`).

Query (tm.fabi.me only resolves via NAT64; use the IPv4 over **HTTP** not HTTPS):
`curl -s -G http://135.181.43.162:8086/query --data-urlencode db=ha_van -u "$INFLUX_HA_VAN_USER:$INFLUX_HA_VAN_PASS"
 --data-urlencode "epoch=ms" --data-urlencode "q=SELECT ... FROM mppt WHERE device='fry' ..."`
Fields: Ui(=Vin), Uo(=Vout), I, P, P_filt, pwm_duty, pwm_ls_duty, mppt_state, ntc_temp, mcu_temp, E, E_today.

Gotchas:
- **Time offset: the havan console logger (`ssh havan.local tail pv/fugu_console.log`) runs UTC+1; influx
  timestamps are device UTC. So console_local = influx_UTC + 1.** (Confirmed by the duty 0→505 sweep edge.)
- `mppt_state` is stuck at 4 — don't use it to infer idle/START.
- While idle (duty=0), `max(P)` per bucket shows 2–4 W spikes = Iin noise × high Voc, NOT real power.

Related: [[project_influxdb_lineprotocol_inhouse]], [[project_binary_lineprotocol_and_tamp]], [[reference_creds_env_files]].
