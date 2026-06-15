---
name: project-flat-console-via-havan-mqtt-broker
description: "Reach fry/flat console+coredump over MQTT via havan's docker mosquitto when NAT/BLE are down"
metadata: 
  node_type: memory
  type: project
  originSessionId: 2942d6e5-8a72-4ad2-9c4d-d09d05dfd848
---

When the NAT router (192.168.1.173) is down and the ESPHome BLE proxy (.231) is flaky, fry/flat are
still reachable over the **MQTT console** — but only from **havan**, not directly from the Mac.

- The broker flat/fry publish to is the **docker mosquitto** on havan (user `$MQTT_BROKER_USER` / pass `$MQTT_BROKER_PASS` — `.claude/memory/secrets.env`),
  in-container `172.17.0.1:1882`. **As of 2026-05-29 it's also reachable directly from the Mac at
  `havan.local:1882`** — `python3 etc/fugu_console.py --mqtt havan.local --mqtt-port 1882
  --mqtt-user $MQTT_BROKER_USER --mqtt-pass $MQTT_BROKER_PASS --name flat --stdin` learned hostname `flat`, saw `pv/log/flat`,
  and executed commands. (Earlier note that havan.local was a different/IPv6 broker no longer holds —
  it's the docker mosquitto now.)
  **UPDATE 2026-06-05: flat's `mqtt.conf` `broker_uri = mqtt://192.168.1.200:1882`, and that broker
  *does* carry flat's log now — reached it directly from the Mac with `--mqtt 192.168.1.200
  --mqtt-port 1882 --mqtt-user $MQTT_BROKER_USER --mqtt-pass $MQTT_BROKER_PASS --name flat` (earlier "200 doesn't carry flat"
  note no longer holds; brokers/topics drift — just read `config/dl/flat.*/conf/mqtt.conf`).**
- Device publishes status/log to `pv/log/<hostname>` (e.g. `pv/log/flat`) and **accepts console
  commands on `pv/log/<hostname>/cmd`**; replies come back on `pv/log/<hostname>`. Confirmed flat
  executes commands this way (uptime, coredump get).
- flat's WiFi/MQTT **flaps** (NAT instability): it goes silent for ~minute windows. A reliable puller
  subscribes `pv/log/#`, waits until it sees a fresh `pv/log/flat` line (link up *now*), then fires
  the command and collects — retrying across flap windows. ~1 attempt usually lands.
- This sidesteps the BLE 8 KB coredump truncation ([[project-coredump-get-ble-8kb-truncation]]):
  MQTT is message-framed, so `coredump get` (~666 base64 lines) comes through whole.

Run pullers on havan with `/home/fab/miniconda3/bin/python3` (has paho). From the Mac the docker
broker isn't directly routable with these creds.

**OTA over MQTT (when `etc/ota.py` can't see flat — weak rssi/NAT, telnet-undiscoverable).**
`ota.py` has NO MQTT transport (telnet-only, port 23). Do it manually: flat is behind NAT (192.168.4.x)
but reaches `192.168.1.x` *outbound* (that's how it hits the broker), so it can also pull an HTTP image
from the Mac's `192.168.1.x` IP. Recipe (done 2026-06-05, flashed flat to `fry-brk1-80-ga80a9189`):
1. `python3 -m http.server 9000` in repo root (serves `build/fugu-firmware.bin`). Get Mac IP: `ipconfig getifaddr en0` (was 192.168.1.234).
2. Send over MQTT: `printf 'ota http://<mac-ip>:9000/build/fugu-firmware.bin\n' | python3 etc/fugu_console.py --mqtt 192.168.1.200 --mqtt-port 1882 --mqtt-user $MQTT_BROKER_USER --mqtt-pass $MQTT_BROKER_PASS --name flat --stdin`
3. Watch progress in flat's log (`ssh havan.local tail -f pv/fugu_console.log | grep flat`) for the HTTP GET, reboot, fresh-boot banner. Verify with `uptime` (prints `App: ... <version>`).
4. Manually archive the ELF (auto-archive is in ota.py's telnet path, skipped here): `python3 -c "import sys;sys.path.insert(0,'etc/idf-devtools');import elf_archive;elf_archive.archive('flat',method='ota',version='<ver>')"`.
Safe-ish: native esp_ota only commits a fully-downloaded+verified image, so a stalled download just keeps the old image. But mind [[project-no-ota-rollback-no-boot-watchdog]] — a *bad* image still bricks if flat's bootloader predates rollback.
