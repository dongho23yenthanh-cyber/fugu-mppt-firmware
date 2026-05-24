"""
GPU oscilloscope client for the fugu MPPT firmware.

Connects to the firmware 'scope' service (TCP port 24, mDNS _scope._tcp), decodes the
12-bit sample stream into a 100k-sample-per-channel ring buffer, and draws it like a
real oscilloscope with fastplotlib (WGPU/pygfx).

Channels stream at different (and not exactly known) sample rates and arrive in TCP
batches, so each channel tracks the wall-clock arrival time of its newest sample and
estimates the timestamps of older samples from its measured average period. The plot's
x-axis is therefore real time (seconds), shared across channels of different rates.

  * sliders set the horizontal time window and the vertical range (slider-driven view)
  * each channel has its own scale (gain), offset (position) and coupling (DC / AC)
  * AC coupling removes the DC component averaged over the visible interval
  * edge trigger with auto/normal mode, pre-trigger position, and a dotted level line
    you can drag with the mouse; the trigger follows the source channel's coupling

Connection + wire-format handling is modelled on the legacy scope-client.py reference.

  python etc/scope_client/fugu_scope.py                 # discover via mDNS
  python etc/scope_client/fugu_scope.py --ip 192.168.4.2 [--port 24]
"""
import argparse
import atexit
import collections
import os
import signal
import socket
import sys
import threading
import time

import numpy as np
import yaml

# allow running from anywhere: put repo root on path for etc.fugu.discover
_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

import fastplotlib as fpl
from fastplotlib.ui import EdgeWindow
from imgui_bundle import imgui

CHANNEL_COLORS = ["magenta", "cyan", "yellow", "green", "red", "white", "orange", "blue"]
GRID_COLOR = "#444444"
TRIGGER_COLOR = "#ff5555"
BUF_SAMPLES = 200_000          # per-channel sample buffer depth
COUPLINGS = ["DC", "AC"]
SETTINGS_PATH = os.path.join(os.path.dirname(__file__), "fugu_scope.yaml")


class Channel:
    def __init__(self, cid, name, typ, bitlen, median):
        self.cid = cid
        self.name = name
        self.typ = typ                       # 'u' | 'i' | 'f'
        self.bitlen = bitlen
        self.sign_mask = (1 << bitlen) if typ == 'i' else 0
        self.half = 1 << (bitlen - 1)
        self.ring = np.full(BUF_SAMPLES, np.nan, dtype=np.float32)
        self.ts = np.full(BUF_SAMPLES, np.nan, dtype=np.float64)   # per-sample wall-clock
        self.head = 0
        self.n_samples = 0
        self.t_first = 0.0
        self.dt_ch = 0.0                     # estimated sample period
        self.ts_last = 0.0                   # timestamp assigned to the newest sample
        self._t_prev = 0.0                   # arrival time of the previous batch
        self.line = None
        self.visible = True
        self.scale = 1.0
        self.coupling = "DC"
        self.offset = -float(self.half) if typ == 'u' else 0.0   # center unipolar DC
        self._med = collections.deque(maxlen=5) if median else None
        self._ndrawn = 0

    def add_batch(self, vals, t_now, fallback_dt):
        """Append a batch of samples that arrived together at t_now. Each sample gets a
        stable timestamp (never revised later); a gentle clock-recovery loop keeps the
        newest timestamp tracking the wall clock without snapping, so older samples don't
        jump when a new block arrives."""
        K = len(vals)
        if K == 0:
            return
        a = np.asarray(vals, dtype=np.float32)
        if self.sign_mask:
            a[a >= self.half] -= self.sign_mask
        if self._med is not None:
            for j in range(K):
                self._med.append(float(a[j]))
                a[j] = np.median(self._med)

        if self.t_first == 0.0:
            self.t_first = t_now
            self.dt_ch = fallback_dt
            self._t_prev = t_now
            prev_ts = t_now - K * self.dt_ch
            self.ts_last = t_now
        else:
            gap = t_now - self._t_prev
            self._t_prev = t_now
            if gap > 0:
                self.dt_ch += 0.1 * (gap / K - self.dt_ch)      # EWMA of true period
            prev_ts = self.ts_last
            naive = self.ts_last + K * self.dt_ch
            self.ts_last = naive + 0.2 * (t_now - naive)        # gentle resync, no snap
            if self.ts_last <= prev_ts:                         # keep monotonic
                self.ts_last = prev_ts + K * self.dt_ch
        ts = prev_ts + (np.arange(1, K + 1) / K) * (self.ts_last - prev_ts)

        pos = self.head % BUF_SAMPLES
        end = pos + K
        if end <= BUF_SAMPLES:
            self.ring[pos:end] = a
            self.ts[pos:end] = ts
        else:
            f = BUF_SAMPLES - pos
            self.ring[pos:] = a[:f]; self.ts[pos:] = ts[:f]
            self.ring[:end - BUF_SAMPLES] = a[f:]; self.ts[:end - BUF_SAMPLES] = ts[f:]
        self.head += K
        self.n_samples += K

    def dt_est(self, fallback):
        return self.dt_ch if self.dt_ch > 0 else fallback

    def recent(self, k):
        """Newest k (value, timestamp) samples, oldest..newest."""
        n = min(self.head, BUF_SAMPLES)
        k = min(k, n)
        pos = self.head % BUF_SAMPLES
        if k <= pos:
            return self.ring[pos - k:pos], self.ts[pos - k:pos]
        f = k - pos
        return (np.concatenate((self.ring[BUF_SAMPLES - f:], self.ring[:pos])),
                np.concatenate((self.ts[BUF_SAMPLES - f:], self.ts[:pos])))

    def ordered(self):
        """Whole buffer, oldest..newest (NaN-padded at front until full)."""
        p = self.head % BUF_SAMPLES
        return np.concatenate((self.ring[p:], self.ring[:p]))

    def sample_rate(self):
        return 1.0 / self.dt_ch if self.dt_ch > 0 else 0.0

    def window(self, t_anchor, x_left, x_right, fallback_dt):
        """Samples with a stored timestamp in [t_anchor+x_left, t_anchor+x_right].
        Returns (xs, ys) with xs in seconds relative to t_anchor, oldest..newest, or None."""
        n = min(self.head, BUF_SAMPLES)
        if n < 2:
            return None
        dt = self.dt_est(fallback_dt)
        need = int((self.ts_last - (t_anchor + x_left)) / dt) + 8
        vals, ts = self.recent(int(np.clip(need, 16, n)))
        lo = np.searchsorted(ts, t_anchor + x_left, "left")
        hi = np.searchsorted(ts, t_anchor + x_right, "right")
        if hi - lo < 1:
            return None
        return (ts[lo:hi] - t_anchor).astype(np.float32), vals[lo:hi]


class ScopeState:
    def __init__(self, args):
        self.args = args
        self.rate = args.rate
        self.dt = 1.0 / args.rate                       # fallback period
        self.channels = {}
        self.order = []
        self.lock = threading.Lock()
        self.t_window = 0.5                              # seconds shown
        self.t_offset = 0.0                              # horizontal position (s)
        self.vrange = float(1 << 12)                     # full vertical span
        # trigger
        self.trig_cid = -1
        self.trig_level = 0.0          # raw units, relative to mean when source is AC
        self.trig_rising = True
        self.trig_auto = True
        self.pretrig = 0.5
        self.dragging = False
        # status
        self.num_bytes = 0
        self.t0 = time.time()
        self.connected = False
        self.status = "discovering..."
        self.hostname = ""
        self.fps = 0.0
        self.do_autofit = False
        # persisted settings applied to channels as they appear
        self.saved_channels = {}
        self.saved_trig_source = None

    def add_channel(self, cid, name, typ, bitlen):
        with self.lock:
            if cid in self.channels:
                return
            ch = Channel(cid, name, typ, bitlen, self.args.median)
            s = self.saved_channels.get(name)
            if s:
                ch.scale = float(s.get("scale", ch.scale))
                ch.offset = float(s.get("offset", ch.offset))
                ch.coupling = s.get("coupling", ch.coupling)
                ch.visible = bool(s.get("visible", ch.visible))
            self.channels[cid] = ch
            self.order.append(cid)
            if self.trig_cid < 0:
                self.trig_cid = cid
            if self.saved_trig_source == name:
                self.trig_cid = cid

    def channel_list(self):
        with self.lock:
            return [self.channels[c] for c in self.order]


# ---------------------------------------------------------------------------
# wire protocol decoder (12-bit packed samples + ###ScopeHead header)
# ---------------------------------------------------------------------------
class Decoder:
    def __init__(self, state: ScopeState):
        self.s = state

    def decode(self, ba: bytes, t: float):
        if ba[:13] == b'###ScopeHead:' and b'###ENDHEAD\n' in ba[:256]:
            body = ba[13:ba.index(b'###ENDHEAD\n')].decode('utf-8', 'replace')
            for ent in body.strip(' ,').split(','):
                if not ent:
                    continue
                if ent.startswith('@host='):
                    self.s.hostname = ent[6:]
                    continue
                cid, rest = ent.split('$')
                name, typrepr = rest.split('=')
                self.s.add_channel(int(cid), name, typrepr[0], int(typrepr[1:]))
            print("scope header:", [(c.cid, c.name, c.typ, c.bitlen)
                                    for c in self.s.channel_list()])
            return
        chans = self.s.channels
        batches = {}                    # cid -> [values] in this chunk
        i, n = 0, len(ba)
        while i + 1 < n:
            b0 = ba[i]
            if b0 & 0x01:               # extended (16/32-bit) not implemented
                break
            cid = (b0 & 0x0E) >> 1
            v = (ba[i + 1] << 4) | ((b0 & 0xF0) >> 4)
            if cid in chans:
                batches.setdefault(cid, []).append(v)
            i += 2
        for cid, vals in batches.items():
            chans[cid].add_batch(vals, t, self.s.dt)


def receive_loop(state: ScopeState, dec: Decoder):
    while True:
        if state.args.ip:
            addr = (state.args.ip, state.args.port)
        else:
            from etc.fugu.discover import discover_scope_servers
            state.status = "discovering..."
            found = discover_scope_servers()
            if not found:
                time.sleep(1)
                continue
            addr = (found[0][0], found[0][1])
            host = found[0][2].rstrip('.')
            state.hostname = host[:-6] if host.endswith('.local') else host
            print("discovered", found)
        state.status = f"connecting {addr[0]}:{addr[1]}"
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(4)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            s.connect(addr)
        except Exception as e:
            state.status = f"connect failed: {e}"
            time.sleep(2)
            continue
        state.connected = True
        state.status = f"connected {addr[0]}:{addr[1]}"
        state.t0 = time.time()
        t_last = time.time()
        while True:
            try:
                buf = s.recv(1024 * 8)
                if not buf:
                    raise ConnectionError("closed")
                state.num_bytes += len(buf)
                t_last = time.time()
                dec.decode(buf, t_last)         # newest sample of the batch ~= now
            except Exception as e:
                if time.time() - t_last >= 8:
                    state.status = f"timeout: {e}"
                    s.close()
                    state.connected = False
                    break
                time.sleep(0.05)


# ---------------------------------------------------------------------------
# trigger: most recent edge crossing `level` with at least `min_post` samples after it.
# Returns the crossing's index into `a`, or None.
# ---------------------------------------------------------------------------
def find_trigger(a, level, rising, min_post):
    if rising:
        cross = (a[:-1] < level) & (a[1:] >= level)
    else:
        cross = (a[:-1] > level) & (a[1:] <= level)
    idx = np.nonzero(cross)[0] + 1          # trigger sample index (NaN compares False)
    if idx.size == 0:
        return None
    good = idx[(a.size - 1 - idx) >= min_post]
    if good.size == 0:
        return None
    return int(good[-1])                      # most recent qualifying crossing


# ---------------------------------------------------------------------------
# control panel
# ---------------------------------------------------------------------------
class Controls(EdgeWindow):
    def __init__(self, figure, state: ScopeState, size=330, location="right"):
        super().__init__(figure=figure, size=size, location=location, title="fugu scope")
        self.s = state

    def update(self):
        s = self.s
        title = s.hostname or (f"{s.args.ip}:{s.args.port}" if s.args.ip else "")
        imgui.text(title)
        imgui.text(s.status)
        imgui.text(f"fps {s.fps:4.0f}  rx {s.num_bytes/1e3/max(1e-3, time.time()-s.t0):6.1f} kB/s")
        chans = s.channel_list()

        imgui.separator()
        imgui.text("Horizontal")
        _, s.t_window = imgui.slider_float("window (s)", s.t_window, 0.002, 60.0,
                                           "%.3f", imgui.SliderFlags_.logarithmic)
        imgui.text(f"  {s.t_window/10*1e3:.2f} ms/div")
        tch = s.channels.get(s.trig_cid)
        dtb = tch.dt_ch if (tch and tch.dt_ch > 0) else s.dt
        back = BUF_SAMPLES * dtb                          # full buffer depth in seconds
        _, s.t_offset = imgui.slider_float("offset (s)", s.t_offset, -back, s.t_window)
        imgui.same_line()
        if imgui.button("0##hoff"):
            s.t_offset = 0.0

        imgui.separator()
        imgui.text("Vertical")
        _, s.vrange = imgui.slider_float("range", s.vrange, 1.0, float(1 << 13),
                                         "%.0f", imgui.SliderFlags_.logarithmic)

        imgui.separator()
        imgui.text("Trigger  (drag plot to set level)")
        if chans:
            names = [c.name for c in chans]
            cur = next((i for i, c in enumerate(chans) if c.cid == s.trig_cid), 0)
            ch, cur = imgui.combo("source", cur, names)
            if ch:
                s.trig_cid = chans[cur].cid
        _, s.trig_auto = imgui.checkbox("auto (free-run)", s.trig_auto)
        _, s.trig_rising = imgui.checkbox("rising edge", s.trig_rising)
        _, s.trig_level = imgui.slider_float("level", s.trig_level, -float(1 << 12), float(1 << 12))
        _, s.pretrig = imgui.slider_float("pre-trigger", s.pretrig, 0.0, 1.0)

        imgui.separator()
        imgui.text("Channels")
        for c in chans:
            imgui.push_id(c.cid)
            _, c.visible = imgui.checkbox(c.name, c.visible)
            if c.line is not None:
                c.line.visible = c.visible
            imgui.same_line()
            imgui.text_colored(_rgba(c.cid), f"{c.sample_rate():.0f} Hz")
            for mode in COUPLINGS:                       # DC | AC as two small buttons
                active = c.coupling == mode
                if active:
                    imgui.push_style_color(imgui.Col_.button, imgui.ImVec4(0.20, 0.45, 0.75, 1.0))
                if imgui.small_button(mode) and not active:
                    c.coupling = mode
                    c.offset = 0.0 if mode == "AC" else (-float(c.half) if c.typ == 'u' else 0.0)
                if active:
                    imgui.pop_style_color()
                imgui.same_line()
            imgui.text("coupling")
            _, c.scale = imgui.slider_float("scale", c.scale, 0.01, 100.0,
                                            "%.3f", imgui.SliderFlags_.logarithmic)
            _, c.offset = imgui.slider_float("offset", c.offset, -float(1 << 12), float(1 << 12))
            imgui.pop_id()

        imgui.separator()
        if imgui.button("auto-fit"):
            s.do_autofit = True
        imgui.same_line()
        if imgui.button("save CSV"):
            save_csv(s)


def _rgba(cid):
    import pygfx
    c = pygfx.Color(CHANNEL_COLORS[cid % len(CHANNEL_COLORS)])
    return imgui.ImVec4(c.r, c.g, c.b, 1.0)


def autofit(state: ScopeState):
    span = 1.0
    for c in state.channel_list():
        if not c.visible:
            continue
        o = c.ordered()
        finite = o[np.isfinite(o)]
        if finite.size < 2:
            continue
        c.scale = 1.0
        if c.coupling == "AC":
            c.offset = 0.0
        else:
            c.offset = -float(np.mean(finite))
        span = max(span, float(np.ptp(finite)))
    state.vrange = span * 1.4


def load_settings(state: ScopeState, path):
    try:
        with open(path) as f:
            d = yaml.safe_load(f) or {}
    except FileNotFoundError:
        return
    except Exception as e:
        print("settings load failed:", e)
        return
    v = d.get("view", {})
    state.t_window = float(v.get("window", state.t_window))
    state.t_offset = float(v.get("offset", state.t_offset))
    state.vrange = float(v.get("vrange", state.vrange))
    t = d.get("trigger", {})
    state.saved_trig_source = t.get("source")
    state.trig_auto = bool(t.get("auto", state.trig_auto))
    state.trig_rising = bool(t.get("rising", state.trig_rising))
    state.trig_level = float(t.get("level", state.trig_level))
    state.pretrig = float(t.get("pretrig", state.pretrig))
    state.saved_channels = d.get("channels", {}) or {}
    print("settings loaded from", path)


def save_settings(state: ScopeState, path):
    tch = state.channels.get(state.trig_cid)
    d = {
        "view": {"window": round(state.t_window, 6),
                 "offset": round(state.t_offset, 6),
                 "vrange": round(state.vrange, 3)},
        "trigger": {"source": tch.name if tch else state.saved_trig_source,
                    "auto": bool(state.trig_auto),
                    "rising": bool(state.trig_rising),
                    "level": round(float(state.trig_level), 3),
                    "pretrig": round(float(state.pretrig), 3)},
        "channels": {c.name: {"scale": round(float(c.scale), 4),
                              "offset": round(float(c.offset), 3),
                              "coupling": c.coupling,
                              "visible": bool(c.visible)}
                     for c in state.channel_list()},
    }
    # keep settings for channels seen in earlier sessions but absent now
    for name, s in state.saved_channels.items():
        d["channels"].setdefault(name, s)
    try:
        with open(path, "w") as f:
            yaml.safe_dump(d, f, sort_keys=False)
    except Exception as e:
        print("settings save failed:", e)


def save_csv(state: ScopeState):
    import csv
    for c in state.channel_list():
        fn = f"{c.name}_{int(round(c.sample_rate()))}hz.csv"
        with open(fn, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow([c.name])
            for v in c.ordered():
                if np.isfinite(v):
                    w.writerow([float(v)])
        print("written", fn)


# ---------------------------------------------------------------------------
# render
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ip", help="device IP (skip mDNS discovery)")
    ap.add_argument("--port", type=int, default=24)
    ap.add_argument("--rate", type=float, default=2000, help="fallback sample rate Hz")
    ap.add_argument("--median", action="store_true", help="5-tap median spike filter")
    args = ap.parse_args()

    state = ScopeState(args)
    load_settings(state, SETTINGS_PATH)
    atexit.register(save_settings, state, SETTINGS_PATH)
    dec = Decoder(state)
    threading.Thread(target=receive_loop, args=(state, dec), daemon=True).start()

    fig = fpl.Figure(size=(1100, 650))
    sub = fig[0, 0]
    sub.axes.grids.xy.visible = True
    sub.axes.grids.xy.material.major_color = GRID_COLOR
    sub.axes.grids.xy.material.minor_color = GRID_COLOR
    sub.controller.enabled = False        # the view is driven entirely by the sliders
    sub.camera.maintain_aspect = False

    trig_line = sub.add_line(
        data=np.array([[0., 0., 0.], [1., 0., 0.]], dtype=np.float32),
        thickness=1, colors=TRIGGER_COLOR, name="__trig__")
    try:
        trig_line.world_object.material.dash_pattern = (1, 3)
    except Exception:
        pass
    trig_line.visible = False

    def level_from_event(ev):
        world = sub.map_screen_to_world((ev.x, ev.y))
        if world is None:
            return
        tch = state.channels.get(state.trig_cid)
        if tch is None:
            return
        # displayed level = (trig_level + offset)*scale  (DC removed identically for AC)
        state.trig_level = float(world[1]) / (tch.scale or 1.0) - tch.offset

    def on_down(ev):
        if getattr(ev, "button", 1) == 1:
            state.dragging = True
            level_from_event(ev)

    def on_move(ev):
        if state.dragging:
            level_from_event(ev)

    def on_up(ev):
        state.dragging = False

    fig.renderer.add_event_handler(on_down, "pointer_down")
    fig.renderer.add_event_handler(on_move, "pointer_move")
    fig.renderer.add_event_handler(on_up, "pointer_up")

    frame_t = [time.time()]

    def update():
        now = time.time()
        dt = now - frame_t[0]
        if dt > 0:
            state.fps = 0.9 * state.fps + 0.1 * (1.0 / dt)
        frame_t[0] = now

        if state.do_autofit:
            autofit(state)
            state.do_autofit = False

        chans = state.channel_list()
        for c in chans:
            if c.line is None:
                col = CHANNEL_COLORS[c.cid % len(CHANNEL_COLORS)]
                data = np.full((BUF_SAMPLES, 3), np.nan, dtype=np.float32)
                data[:, 2] = 0.0
                c.line = sub.add_line(data=data, thickness=1, colors=col, name=c.name)
                c.line.visible = c.visible
                c._ndrawn = 0

        tw = state.t_window
        tch = state.channels.get(state.trig_cid)

        # trigger search on the source channel (raw units; AC -> relative to visible mean)
        triggered = False
        t_anchor = now
        if tch is not None and min(tch.head, BUF_SAMPLES) >= 2:
            dtc = tch.dt_est(state.dt)
            n_win = int(np.clip(tw / dtc, 16, min(tch.head, BUF_SAMPLES)))
            vals, ts = tch.recent(int(np.clip(3 * n_win, 16, min(tch.head, BUF_SAMPLES))))
            dc = np.nanmean(vals[-n_win:])
            if not np.isfinite(dc):
                dc = 0.0
            level = state.trig_level + (dc if tch.coupling == "AC" else 0.0)
            min_post = int((1.0 - state.pretrig) * n_win)
            c = find_trigger(vals, level, state.trig_rising, min_post)
            if c is not None:
                t_anchor = float(ts[c])                  # trigger event timestamp
                triggered = True

        if not triggered and not state.trig_auto:
            return                                       # normal mode: hold last frame
        if triggered:
            x_left, x_right = -state.pretrig * tw, (1.0 - state.pretrig) * tw
        else:
            x_left, x_right = -tw, 0.0

        t_anchor += state.t_offset      # horizontal position: pan the view in time

        for c in chans:
            if c.line is None or not c.visible:
                continue
            win = c.window(t_anchor, x_left, x_right, state.dt)
            if win is None:
                if c._ndrawn:
                    c.line.data[:c._ndrawn, 0] = np.nan
                    c._ndrawn = 0
                continue
            xs, ys = win
            if c.coupling == "AC":
                m = np.nanmean(ys)
                ys = ys - (m if np.isfinite(m) else 0.0)
            k = xs.size
            c.line.data[:k, 0] = xs
            c.line.data[:k, 1] = (ys + c.offset) * c.scale     # offset first, then scale
            if k < c._ndrawn:
                c.line.data[k:c._ndrawn, 0] = np.nan
            c._ndrawn = k

        # trigger level line (displayed level is independent of coupling DC removal)
        if tch is not None:
            disp = (state.trig_level + tch.offset) * tch.scale
            trig_line.data[:, 0] = [x_left, x_right]
            trig_line.data[:, 1] = disp
            trig_line.visible = True
        else:
            trig_line.visible = False

        cam = sub.camera
        cam.width = max(x_right - x_left, 1e-9)
        cam.height = state.vrange
        cam.local.position = ((x_left + x_right) / 2.0, 0.0, cam.local.position[2])

    def _on_signal(signum, frame):
        save_settings(state, SETTINGS_PATH)
        os._exit(0)
    for _sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(_sig, _on_signal)
        except Exception:
            pass

    sub.add_animations(update)
    fig.add_gui(Controls(fig, state))
    fig.show(maintain_aspect=False)
    fpl.loop.run()
    save_settings(state, SETTINGS_PATH)


if __name__ == "__main__":
    main()
