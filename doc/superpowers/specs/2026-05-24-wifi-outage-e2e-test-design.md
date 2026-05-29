*this document is an LLM generated placeholder*

> **Update:** the two webhook tests this spec refers to were merged into
> `etc/e2e-test/test_wifi_outage.py` (`--mode stick|roam`), and the shared `Tap`/`Results`/poll
> helpers now live in `etc/e2e-test/_harness.py`. The test this spec describes is
> `etc/e2e-test/test_wifi_outage_service_recovery.py`.

# E2E test — WiFi outage / network-service recovery (serial-controlled router)

## Purpose

Regression-test the recent `startEnabledAtBoot()` fix on real hardware: when WiFi
is down, the network-requiring services (`tele`, `ftp`, `telnet`, `scope`) must stay
`Stopped` (not `Failed`), and must come up correctly via `startEnabledNetworkServices()`
on the WiFi-up edge — both for outages **after** boot and outages **at** boot.

Unlike the existing webhook-based e2e tests in `etc/e2e-test/`, this one is fully
USB-serial: the host drives the DUT *and* an esp32 acting as the WiFi router
(`fl4p/esp32_nat_router_extended` fork) over their respective serial consoles.
That keeps the test independent of any networking infrastructure beyond the two
boards and the host.

## Hardware setup

- **DUT**: regular fugu firmware on an ESP32-S3, USB serial → `--dut`.
- **Router**: `esp32_nat_router_extended` on a second ESP32 (any variant), USB serial
  → `--router`. WAN side reachable from host at `--router-wan-ip`.
- Both connected to the host concurrently; the router's AP is the only network the
  DUT can join (i.e. `wifi.conf` on the DUT references only the router's SSID, or the
  test rewrites it).

Console commands used (literal strings, configurable via CLI args):

| Side   | Command                                  | Purpose                        |
|--------|------------------------------------------|--------------------------------|
| router | `set_ap <ssid> <psk>`                    | reconfigure AP (NVS-persistent)|
| router | `portmap add TCP 23 <dut-ip> 23`         | expose DUT telnet on WAN side  |
| router | `restart`                                | apply new AP/portmap settings  |
| DUT    | `wifi-add <ssid>:<psk>`                  | write SSID to `/littlefs/conf/wifi.conf` |
| DUT    | `restart`                                | reboot                         |
| DUT    | `ip`                                     | print station IP               |
| DUT    | `svc` / `svc list`                       | dump service states            |
| DUT    | `dc 0`                                   | force converter idle (so `wifiLoop()` keeps attempting reconnect) |

## Components

Two thin helper classes in the test file (no extraction to `etc/fugu/` — that is a
separate repo per `project_fugu_py_shared_console`):

```text
Dut(serial_port)
  .tap                 # Tap{} — collects status-line events (N, rssi, connect, boot)
  .restart()           # send `restart`, wait for "setup() done"
  .ip() -> str         # parse `ip` reply
  .svc_list() -> dict  # parse `svc` reply: {name: state}
  .wifi_add(ssid, psk) # send `wifi-add ssid:psk`
  .force_idle()        # send `dc 0`
  .wait_associated(timeout) -> bool   # status-line rssi != 0
  .wait_disassociated(timeout) -> bool # status-line rssi == 0

Router(serial_port)
  .set_ap(ssid, psk)       # send `set_ap ...`
  .add_portmap(proto, ext_port, dut_ip, int_port)
  .restart()               # send `restart`, wait for the router's ready prompt
```

Plus one free function:

```text
telnet_probe(host, port, timeout) -> str
  # opens TCP, reads ~256 bytes, returns the welcome banner;
  # raises if the connection times out or banner is empty
```

All console I/O goes through `etc/fugu` (`SerialTransport` + `Console`), same as
`test_wifi_outage.py`. The status-line `Tap` pattern is reused too (now `_harness.EventLog`).

## Setup (run once, at test start)

1. Open both serial consoles; `con.wait_ready(30s)` on each.
2. **Router**: `set_ap <real-ssid> <psk>` → `restart`; wait for router back up
   (banner / `>` prompt).
3. **DUT**: `wifi-add <real-ssid>:<psk>` → `dc 0` → `restart`.
4. DUT: `wait_associated(60s)`. If it fails here, abort with a clear message —
   wiring or credentials are wrong, no point continuing.
5. DUT: `dut_ip = ip()`.
6. **Router**: `portmap add TCP 23 <dut_ip> 23`. (No restart needed; portmap is
   live-applied per the fl4p fork's behavior. If it requires a restart in your
   build, the user can pass `--portmap-needs-restart` and we'll restart once here.)
7. Baseline behavioral check: `telnet_probe(router-wan-ip, 23)` must contain
   `Welcome to` and the DUT hostname. Fail fast if not — confirms the portmap +
   DUT telnet path before any outage testing.

## Scenario A — outage **after** boot

1. Pre: `svc_list()` shows `tele`, `ftp`, `telnet`, `scope` all `Running`.
2. Router: `set_ap <off-ssid> <psk>` → `restart`.
3. DUT: `wait_disassociated(--outage-timeout, default 30s)`.
4. Router: `set_ap <real-ssid> <psk>` → `restart`.
5. DUT: `wait_associated(--reconnect-timeout, default 90s)`.
6. Verify: `svc_list()` shows all four `Running`; `telnet_probe()` succeeds.
7. Assert no fresh boot banner since step 2 (no reboot through the cycle).

## Scenario B — outage **during** boot  *(the fix's regression case)*

1. Router: `set_ap <off-ssid> <psk>` → `restart`. Wait router back.
2. DUT: `restart`. Wait for `setup() done`.
3. DUT: `svc_list()`. Assert `tele`, `ftp`, `telnet`, `scope` are **`Stopped`**,
   **not** `Failed`. **This is the core regression check for the recent fix.**
4. Wait 5 s; re-read `svc_list()`; states unchanged; no second `setup() done`
   (DUT didn't reboot itself).
5. Router: `set_ap <real-ssid> <psk>` → `restart`.
6. DUT: `wait_associated(--reconnect-timeout, default 90s)`.
7. Verify: `svc_list()` all `Running`; `telnet_probe()` succeeds.

## Cleanup (try/finally, always runs)

```python
finally:
    try: router.set_ap(args.ssid, args.psk); router.restart()
    except Exception: pass     # never mask the original failure
    dut.close(); router.close()
```

So a failed run never leaves the bench AP "off" and unreachable.

## CLI

```
--dut /dev/cu.usbmodem...       # required
--router /dev/cu.usbmodem...    # required
--router-wan-ip 192.168.1.173   # required
--ssid <name> --psk <pw>        # required: the real AP credentials
--off-ssid <name> [--off-psk]   # default: "fugu-test-off" / same psk
--outage-timeout 30
--reconnect-timeout 90
--rounds 1                      # repeat A and B N times
--scenario {both,A,B}           # default both
--portmap-needs-restart         # flag, if your fork requires it
```

Exit code: `0` if every scenario in every round passes; non-zero otherwise.
Per-check pass/fail printed line-by-line (reuses the `Results` class pattern from
existing tests).

## What this does NOT cover

- Multi-AP roaming / stick (covered by `test_wifi_outage.py --mode stick|roam`).
- `wifi off <minutes>` timer behavior (covered by `test_wifi_off_timeout.py`).
- The 30-min MPPT re-sweep gate, charger flow, etc.

## Risks / pitfalls (notes to future-me)

- **NVS persistence**: `set_ap` writes to NVS on the router. If a test run is
  killed *between* "off" and "on", the bench AP stays off. The try/finally guards
  the normal failure path; a SIGKILL still leaves the bench broken — bench admin
  must know about `set_ap <real> + restart` as the manual recovery.
- **Portmap survival**: the fl4p fork keeps portmaps across `restart`. If a future
  version doesn't, `--portmap-needs-restart` (or a re-add per scenario) becomes
  needed.
- **DUT IP stability**: DHCP-MAC binding on the router usually re-issues the same
  IP. If it ever shifts, scenario B's post-reconnect telnet probe will fail; we'd
  need to re-read `ip` on the DUT and re-add the portmap. Cheap to add later if
  it becomes a problem.
- **DCM/`dc 0`**: forcing the converter idle silences `mppt.protectLf` paths that
  depend on real Iout. The test is purely network-oriented; this is intentional.
