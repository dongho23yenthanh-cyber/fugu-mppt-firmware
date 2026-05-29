#!/usr/bin/env python3
"""Exercise the MqttService console-input gate (mqtt.conf::cmd_input) and lifecycle
*without rebooting the device*.

Two channels are used:

  * MQTT (broker creds from etc/mqtt.env or the environment) -- passively observe the
    device's log mirror on ``pv/log/<host>`` and publish test commands to
    ``pv/log/<host>/cmd``.
  * a control console that works regardless of cmd_input -- telnet (``--telnet ip:port``)
    or serial (``--serial /dev/...``) -- to run ``set-config`` / ``svc`` commands. This
    must reach the *same* physical device the MQTT log stream comes from.

Checks (no device reboot, so `svc rs mqtt` is the only thing that can reload cmd_input):

  1. device is publishing logs on ``pv/log/<host>``
  2. cmd_input=1  + ``svc rs mqtt``  -> a command published to .../cmd IS processed
  3. cmd_input=0  + ``svc rs mqtt``  -> the same command is NOT processed
     (proves the restart reloads cmd_input, i.e. close() clears the handler map)
  4. ``svc off mqtt``                -> the log stream stops (broker connection torn down)
  5. ``svc on mqtt``                 -> the log stream resumes

cmd_input is restored to its original value before exit.

The device under test is identified automatically: a unique nonce is sent over the control
channel and the pv/log/<host> topic that echoes it is the device (no --name needed, and a
broker with several Fugus won't be confused with the wrong host).

Usage:
    python etc/e2e-test/test_mqtt_cmd_input.py --telnet 192.168.1.173:232
    python etc/e2e-test/test_mqtt_cmd_input.py --serial /dev/cu.usbmodem1201
Broker is read from $MQTT_HOST/$MQTT_PORT/$MQTT_USER/$MQTT_PASS or etc/mqtt.env.
"""
import argparse
import os
import sys
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt required: pip install paho-mqtt")

ETC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo/etc (holds the fugu pkg + mqtt.env)
sys.path.insert(0, ETC_DIR)
from fugu.transport import SocketTransport, SerialTransport
from fugu.console import Console
from _harness import Results, wait_for

LOG_ROOT = "pv/log/"


def load_env(path):
    """Populate os.environ from a key=value file (values already in env win)."""
    if not os.path.isfile(path):
        return
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        os.environ.setdefault(k.strip(), v.strip())


class LogTap:
    """Subscribes to pv/log/# and records every device's log lines as (host, text).

    The host under test is pinned via `identify()` (token correlation through the control
    channel), so a busy broker with several Fugus can't confuse the observation.
    """

    def __init__(self, host, port, user, pw):
        self.hostname = None                  # pinned device; observation filters to it
        self.lines = []                       # (host, text), in arrival order
        self._lock = threading.Lock()
        try:
            self._c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except (AttributeError, TypeError):   # paho < 2.0
            self._c = mqtt.Client()
        if user:
            self._c.username_pw_set(user, pw)
        self._c.on_connect = lambda c, *a: c.subscribe(LOG_ROOT + "#")
        self._c.on_message = self._on_message
        self._c.connect(host, port, keepalive=30)
        self._c.loop_start()

    def _on_message(self, client, userdata, msg):
        parts = msg.topic.split("/")
        if len(parts) != 3:                   # want pv/log/<host>, not .../cmd
            return
        text = msg.payload.decode("utf-8", "replace")
        with self._lock:
            self.lines.append((parts[2], text))

    def identify(self, ctrl, timeout=8):
        """Run a unique nonce on the control channel; the pv/log/<host> that echoes it is
        the device under test. Works regardless of cmd_input (console output is always
        mirrored to MQTT). Pins self.hostname and returns it (or None)."""
        token = "e2eid_" + os.urandom(4).hex()
        cursor = self._raw_len()
        ctrl.run(token, timeout=4)            # unknown command: still echoed + logged
        marker = f"received serial command: '{token}'" # todo use CliMarkers.RX_CMD
        raise NotImplementedError()
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            with self._lock:
                for host, text in self.lines[cursor:]:
                    if marker in text:
                        self.hostname = host
                        return host
            time.sleep(0.1)
        return None

    def _raw_len(self):
        with self._lock:
            return len(self.lines)

    def mark(self):
        """Cursor into the (pinned-host) line stream."""
        with self._lock:
            return sum(1 for h, _ in self.lines if h == self.hostname)

    def _pinned(self):
        return [t for h, t in self.lines if h == self.hostname]

    def since(self, cursor):
        with self._lock:
            return self._pinned()[cursor:]

    def count_since(self, cursor):
        with self._lock:
            return len(self._pinned()) - cursor

    def cmd_topic(self):
        return f"{LOG_ROOT}{self.hostname}/cmd"

    def publish(self, cmd):
        self._c.publish(self.cmd_topic(), cmd, qos=0)

    def close(self):
        self._c.loop_stop()
        self._c.disconnect()


class Ctrl:
    """The out-of-band control console (telnet/serial) that always accepts commands."""

    def __init__(self, transport):
        self._c = Console(transport)
        time.sleep(1.0)         # let any welcome banner / backlog arrive

    def run(self, cmd, timeout=8):
        r = self._c.command(cmd, timeout=timeout)
        return r.ok

    def get_cmd_input(self):
        r = self._c.command("get-config mqtt.conf cmd_input", timeout=8)
        for ln in r:
            if "cmd_input' = '" in ln:
                return ln.split("= '", 1)[1].split("'", 1)[0]
        return ""

    def close(self):
        self._c._stop.set()
        self._c.transport.close()


def reload_with(ctrl, value):
    """set cmd_input=<value> and restart the mqtt service so onStart re-reads it."""
    ctrl.run(f"set-config mqtt.conf cmd_input {value}")
    ctrl.run("svc rs mqtt")


def cmd_processed(tap, cmd, settle=5.0):
    """Publish a command and report whether the device echoed having handled it."""
    cursor = tap.mark()
    tap.publish(cmd)
    marker = f"received serial command: '{cmd}'"
    seen = wait_for(lambda: any(marker in t for t in tap.since(cursor)), settle)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--telnet", metavar="IP:PORT", help="control console over telnet")
    g.add_argument("--serial", metavar="DEV", help="control console over serial")
    ap.add_argument("--mqtt", default=None, help="broker host (default $MQTT_HOST)")
    ap.add_argument("--mqtt-port", type=int, default=None)
    args = ap.parse_args()

    load_env(os.path.join(ETC_DIR, "mqtt.env"))
    host = args.mqtt or os.environ.get("MQTT_HOST")
    port = args.mqtt_port or int(os.environ.get("MQTT_PORT", "1883"))
    user = os.environ.get("MQTT_USER")
    pw = os.environ.get("MQTT_PASS")
    if not host:
        sys.exit("no broker: set $MQTT_HOST / etc/mqtt.env or pass --mqtt")

    if args.telnet:
        ip, _, p = args.telnet.partition(":")
        transport = SocketTransport(ip, port=int(p or 23), timeout=8)
    else:
        transport = SerialTransport(args.serial)

    print(f"broker {host}:{port}  control={'telnet ' + args.telnet if args.telnet else 'serial ' + args.serial}")
    tap = LogTap(host, port, user, pw)
    ctrl = Ctrl(transport)
    res = Results()
    original = None
    try:
        # 1. identify the device under test by correlating a control-channel nonce with the
        #    MQTT log stream -> proves logs flow AND pins the same device the control channel
        #    drives (so a multi-device broker can't be confused with the wrong host).
        hostname = tap.identify(ctrl, timeout=8)
        res.check("device under test is publishing logs (control<->MQTT correlated)",
                  hostname is not None, f"host={hostname}" if hostname else "no echo seen on any pv/log/<host>")
        if not hostname:
            return 1

        original = ctrl.get_cmd_input()
        print(f"  (cmd_input currently '{original or '<unset>'}')")

        # 2. cmd_input=1 -> command accepted
        reload_with(ctrl, 1)
        time.sleep(2.5)                       # mqtt reconnects, resubscribes
        res.check("svc rs mqtt with cmd_input=1 -> command accepted",
                  cmd_processed(tap, "rt-stats"))

        # 3. cmd_input=0 -> command ignored (this is the close()/reload check)
        reload_with(ctrl, 0)
        time.sleep(2.5)
        cur = tap.mark()
        res.check("device still logging after restart (mqtt up)",
                  wait_for(lambda: tap.count_since(cur) > 0, 5))
        res.check("svc rs mqtt with cmd_input=0 -> command ignored",
                  not cmd_processed(tap, "rt-stats"))

        # 4. svc off mqtt -> log stream stops
        ctrl.run("svc off mqtt")
        time.sleep(2.0)                       # let the disconnect settle
        cursor = tap.mark()
        time.sleep(4.0)
        res.check("svc off mqtt -> log stream stops", tap.count_since(cursor) == 0,
                  f"{tap.count_since(cursor)} msgs after off")

        # 5. svc on mqtt -> log stream resumes
        cur = tap.mark()
        ctrl.run("svc on mqtt")
        res.check("svc on mqtt -> log stream resumes",
                  wait_for(lambda: tap.count_since(cur) > 0, 10))
    finally:
        # restore original cmd_input (default to 1 if it was set, else 0)
        if original is not None:
            restore = original if original in ("0", "1") else "1"
            print(f"  restoring cmd_input='{restore}'")
            reload_with(ctrl, restore)
        ctrl.close()
        tap.close()

    print("\n" + ("ALL CHECKS PASSED" if res.ok() else "SOME CHECKS FAILED"))
    return 0 if res.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
