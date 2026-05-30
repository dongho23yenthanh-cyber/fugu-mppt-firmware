"""Scope-endpoint discovery + selection shared by fugu_scope.py and scope-client.py.

Endpoints come from two sources:
  * mDNS (`_scope._tcp`) for boards on the local LAN, and
  * `etc/nat.env`'s `$NAT_TELNET` (host:port, comma-separated) for NAT-routed boards (fry/flat)
    that aren't mDNS-reachable. The router forwards telnet on `23x` and scope on `24x`, mirroring
    the device ports (23 vs 24), so each scope endpoint is the telnet endpoint's port + 10.
"""
import os
import threading
import time

NAT_ENV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "nat.env")
SCOPE_PORT_OFFSET = 10   # forwarded scope port = forwarded telnet port + 10 (23x -> 24x)


def _load_env_file(path):
    """Fill os.environ from a KEY=VALUE file (shell-set vars win); silent if absent."""
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, _, v = line.partition("=")
                    os.environ.setdefault(k.strip(), v.strip())
    except FileNotFoundError:
        pass


def nat_scope_endpoints():
    """List of (host, port) scope endpoints from `$NAT_TELNET` (read from nat.env if unset);
    empty when none are configured."""
    if not os.environ.get("NAT_TELNET"):
        _load_env_file(NAT_ENV)
    out = []
    for ep in (os.environ.get("NAT_TELNET") or "").split(","):
        ep = ep.strip()
        host, _, port = ep.partition(":")
        if ep and port:
            out.append((host.strip(), int(port) + SCOPE_PORT_OFFSET))
    return out


def probe_telnet_hostname(host, telnet_port, timeout=2.0):
    """Hostname from the device's telnet welcome banner ("Welcome to <host> ..."), reusing
    `fugu_console.probe_welcome`. The telnet console sits one port-block below the scope service
    (23x vs 24x), so probing it never disturbs the scope stream. None if unreachable / no banner /
    `fugu_console` is unavailable."""
    import asyncio
    try:
        try:
            from fugu_console import probe_welcome
        except ImportError:
            from etc.fugu_console import probe_welcome
    except Exception:
        return None
    try:
        return asyncio.run(probe_welcome(host, telnet_port, timeout))
    except Exception:
        return None


def discover_endpoints(mdns=True, probe_nat=True, timeout=2.0):
    """All scope endpoints as (host, port, hostname|None): mDNS-advertised first (hostname from
    the advert), then nat.env-derived (hostname probed from the scope header when `probe_nat`)."""
    cand = []
    if mdns:
        try:
            try:
                from etc.fugu.discover import discover_scope_servers
            except ImportError:
                from fugu.discover import discover_scope_servers
            for a, p, name in discover_scope_servers(timeout=int(timeout)):
                host = (name or a).rstrip(".")
                host = host[:-6] if host.endswith(".local") else host
                cand.append((a, p, host))
        except ImportError:
            pass
        except Exception as e:
            print("mDNS discovery failed:", e)
    seen = {(h, p) for h, p, _ in cand}
    nat = [(h, p) for h, p in nat_scope_endpoints() if (h, p) not in seen]
    if probe_nat and nat:
        names = {}                       # probe all NAT endpoints at once (each blocks up to timeout)
        def _probe(h, p):
            names[(h, p)] = probe_telnet_hostname(h, p - SCOPE_PORT_OFFSET)
        threads = [threading.Thread(target=_probe, args=hp, daemon=True) for hp in nat]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        cand += [(h, p, names.get((h, p))) for h, p in nat]
    else:
        cand += [(h, p, None) for h, p in nat]
    return cand


class ScopeDiscovery:
    """Live scope-endpoint discovery for long-running clients (fugu_scope.py).

    Holds one persistent mDNS `_scope._tcp` browser, so repeated `snapshot()` calls are free and
    emit no per-sweep Zeroconf open/close chatter. nat.env endpoints are merged in and their
    hostnames resolved from the telnet welcome banner (`probe_telnet_hostname`) once, cached, and
    re-probed at most every `probe_interval` s while still unknown. `note_hostname()` lets a
    connected client feed back the hostname it learned, suppressing further probes for it.
    """

    def __init__(self, mdns=True, probe_nat=True, probe_interval=20.0):
        self._lock = threading.Lock()
        self._mdns = {}              # service name -> [(host, port, hostname)]
        self._nat_names = {}         # (host, scope_port) -> hostname
        self._nat_next = {}          # (host, scope_port) -> monotonic deadline for the next probe
        self._probe_nat = probe_nat
        self._probe_interval = probe_interval
        self._zc = None
        self._browser = None
        if mdns:
            self._start_mdns()

    def _start_mdns(self):
        try:
            from zeroconf import Zeroconf, ServiceBrowser
        except Exception as e:
            print("mDNS unavailable:", e)
            return
        self._zc = Zeroconf()
        self._browser = ServiceBrowser(self._zc, "_scope._tcp.local.", handlers=[self._on_change])

    def _on_change(self, zeroconf, service_type, name, state_change):
        from zeroconf import ServiceStateChange
        if state_change is ServiceStateChange.Removed:
            with self._lock:
                self._mdns.pop(name, None)
            return
        info = zeroconf.get_service_info(service_type, name, timeout=1000)
        if not info:
            return
        host = (info.server or name).rstrip(".")
        host = host[:-6] if host.endswith(".local") else host
        eps = [(a, info.port, host) for a in info.parsed_addresses()]
        with self._lock:
            self._mdns[name] = eps

    def note_hostname(self, host, port, name):
        """Record a hostname learned out-of-band (e.g. a connected client's scope header)."""
        if name:
            with self._lock:
                self._nat_names[(host, port)] = name

    def snapshot(self):
        """Current endpoints as (host, port, hostname|None): live mDNS adverts first, then the
        nat.env endpoints with their probed/cached hostname (None until first reached)."""
        with self._lock:
            cand = [ep for eps in self._mdns.values() for ep in eps]
            names, nexts = dict(self._nat_names), dict(self._nat_next)
        seen = {(h, p) for h, p, _ in cand}
        now = time.monotonic()
        for h, p in nat_scope_endpoints():
            if (h, p) in seen:
                continue
            name = names.get((h, p))
            if name is None and self._probe_nat and now >= nexts.get((h, p), 0.0):
                name = probe_telnet_hostname(h, p - SCOPE_PORT_OFFSET)
                with self._lock:
                    self._nat_next[(h, p)] = now + self._probe_interval
                    if name:
                        self._nat_names[(h, p)] = name
            cand.append((h, p, name))
        return cand

    def close(self):
        if self._zc is not None:
            self._zc.close()


def choose_endpoint(candidates, match=None, interactive=True):
    """Pick one (host, port, hostname) from `candidates`.

    With `match`, auto-pick the first whose hostname contains it. With a single candidate, auto-pick.
    Otherwise print a numbered menu (hostname when known, else host:port) and read the choice from
    stdin; falls back to index 0 on empty/invalid input or when not `interactive`. None if empty.
    """
    if not candidates:
        return None
    if match:
        for c in candidates:
            if c[2] and match in c[2]:
                return c
        print(f"no discovered host matches {match!r}")
        return None
    if len(candidates) == 1:
        return candidates[0]
    for i, (h, p, name) in enumerate(candidates):
        print(f"  [{i}] {name or '?':<24} {h}:{p}")
    if not interactive:
        return candidates[0]
    try:
        sel = input(f"connect to which? [0-{len(candidates) - 1}] (0): ").strip()
    except EOFError:
        return candidates[0]
    try:
        return candidates[int(sel)] if sel else candidates[0]
    except (ValueError, IndexError):
        print("invalid selection; using 0")
        return candidates[0]
