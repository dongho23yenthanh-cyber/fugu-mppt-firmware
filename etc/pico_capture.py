#!/usr/bin/env python3
"""Block-capture from a legacy PicoScope 2000 (ps2000 driver) headlessly via ctypes.

The driver dylibs ship inside the PicoScope app and are x86_64-only, so on Apple Silicon this
re-execs itself under Rosetta (`arch -x86_64 /usr/bin/python3`) with the app's Resources dir on
DYLD_LIBRARY_PATH (libps2000 lazily needs libpicoipp / libiomp5 from there).

Channel A is the current clamp (scale set by --mv-per-a, e.g. Hantek CC-65: 100 mV/A on the 20 A
range, 10 mV/A on the 65 A range). Reports peak-to-peak and a coarse ASCII waveform. With --fsw it
sizes the window to a few switching periods and prints the ripple in amps.

    arch -x86_64 /usr/bin/python3 etc/pico_capture.py --range 200mv --fsw 5000
"""

import argparse
import ctypes as C
import os
import platform
import sys

APP_RES = "/Applications/PicoScope 7 T&M.app/Contents/Resources"
DYLIB = f"{APP_RES}/libps2000.dylib"

# ps2000 enums
RANGES = {"20mv": 1, "50mv": 2, "100mv": 3, "200mv": 4, "500mv": 5,
          "1v": 6, "2v": 7, "5v": 8, "10v": 9, "20v": 10}
RANGE_V = {1: .02, 2: .05, 3: .1, 4: .2, 5: .5, 6: 1, 7: 2, 8: 5, 9: 10, 10: 20}
CH_A, CH_B = 0, 1
DC, AC = 1, 0
MAX_ADC = 32767
TIME_UNIT_S = {0: 1e-15, 1: 1e-12, 2: 1e-9, 3: 1e-6, 4: 1e-3, 5: 1.0}


def reexec_x86():
    """libps2000 is x86_64-only; on arm64 relaunch the same script under Rosetta."""
    if platform.machine() == "arm64" and not os.environ.get("_PICO_X86"):
        env = dict(os.environ, _PICO_X86="1",
                   DYLD_LIBRARY_PATH=APP_RES + ":" + os.environ.get("DYLD_LIBRARY_PATH", ""))
        os.execve("/usr/bin/arch",
                  ["arch", "-x86_64", "/usr/bin/python3", os.path.abspath(__file__)] + sys.argv[1:],
                  env)


class PS2000:
    def __init__(self):
        self.lib = C.CDLL(DYLIB)
        L = self.lib
        L.ps2000_open_unit.restype = C.c_int16
        L.ps2000_set_channel.restype = C.c_int16
        L.ps2000_set_trigger.restype = C.c_int16
        L.ps2000_get_timebase.restype = C.c_int16
        L.ps2000_run_block.restype = C.c_int16
        L.ps2000_ready.restype = C.c_int16
        L.ps2000_get_values.restype = C.c_int32
        L.ps2000_stop.restype = C.c_int16
        L.ps2000_close_unit.restype = C.c_int16
        self.h = L.ps2000_open_unit()
        if self.h <= 0:
            raise RuntimeError(f"ps2000_open_unit failed (handle={self.h}); is the scope plugged in "
                               "and not held by the PicoScope app?")

    def channel(self, ch, rng, coupling=DC, enabled=1):
        if not self.lib.ps2000_set_channel(self.h, ch, enabled, coupling, rng):
            raise RuntimeError("set_channel failed")

    def timebase_for(self, want_interval_s, nsamp):
        """Pick the fastest timebase whose interval >= want_interval_s; return (tb, interval_s, max)."""
        ti = C.c_int32()
        tu = C.c_int16()
        ms = C.c_int32()
        best = None
        for tb in range(1, 23):  # tb 0 needs ETS on these units
            if not self.lib.ps2000_get_timebase(self.h, tb, nsamp, C.byref(ti),
                                                C.byref(tu), 1, C.byref(ms)):
                continue
            interval = ti.value * 1e-9  # get_timebase returns ns regardless of time_units
            if interval <= 0:
                continue
            best = (tb, interval, ms.value)
            if interval >= want_interval_s:
                break
        if not best:
            raise RuntimeError("no usable timebase")
        return best

    def trigger(self, source, thresh_counts, rising=True, delay_pct=-20, auto_ms=0):
        """Arm a hardware trigger; auto_ms=0 waits forever (single shot), delay_pct<0 = pre-trigger."""
        if not self.lib.ps2000_set_trigger(self.h, source, thresh_counts, 0 if rising else 1,
                                           delay_pct, auto_ms):
            raise RuntimeError("set_trigger failed")

    def capture(self, tb, nsamp, want_b=False, arm_timeout=5.0):
        ind = C.c_int32()
        if not self.lib.ps2000_run_block(self.h, nsamp, tb, 1, C.byref(ind)):
            raise RuntimeError("run_block failed")
        import time
        t0 = time.monotonic()
        while not self.lib.ps2000_ready(self.h):
            if time.monotonic() - t0 > arm_timeout:
                self.lib.ps2000_stop(self.h)
                raise RuntimeError("capture timeout (no trigger?)")
            time.sleep(0.002)
        ba = (C.c_int16 * nsamp)()
        bb = (C.c_int16 * nsamp)() if want_b else None
        ov = C.c_int16()
        got = self.lib.ps2000_get_values(self.h, ba, bb if bb else None, None, None,
                                         C.byref(ov), nsamp)
        self.lib.ps2000_stop(self.h)
        return list(ba[:got]), (list(bb[:got]) if bb else None), ov.value

    def close(self):
        self.lib.ps2000_close_unit(self.h)


def to_volts(counts, rng):
    fs = RANGE_V[rng]
    return [c / MAX_ADC * fs for c in counts]


def fit_didt(amps, dt):
    """Least-squares di/dt over the linear part of a rising ramp (20–80% of baseline→peak).

    Returns (didt A/s, t_lo, t_hi, n) or None. Baseline = median of the pre-trigger samples;
    fitting between 20% and 80% of the rise skips the clamp toe and the current-limit knee.
    """
    if len(amps) < 20:
        return None
    base = sorted(amps[:max(5, len(amps) // 10)])
    base = base[len(base) // 2]
    pk = max(amps)
    span = pk - base
    if span <= 0:
        return None
    lo, hi = base + 0.2 * span, base + 0.8 * span
    idx = [i for i, a in enumerate(amps) if lo <= a <= hi]
    if len(idx) < 8:
        return None
    i0, i1 = idx[0], idx[-1]  # contiguous rising band
    xs = [(i - i0) * dt for i in range(i0, i1 + 1)]
    ys = amps[i0:i1 + 1]
    n = len(xs)
    sx, sy = sum(xs), sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))
    den = n * sxx - sx * sx
    if abs(den) < 1e-30:
        return None
    slope = (n * sxy - sx * sy) / den
    return slope, i0, i1, n


def didt_shot(s, args, rng, tb, interval, nsamp):
    """Arm the edge trigger, capture one ramp, fit di/dt, return L (or None). Prints a one-liner."""
    rising = args.trig_dir == "rising"
    thr = int((1 if rising else -1) * args.trig_frac * MAX_ADC)
    s.trigger(CH_A, thr, rising=rising, delay_pct=-20, auto_ms=0)
    print(f"  armed {args.trig_dir} @ {'+' if rising else '-'}{args.trig_frac * RANGE_V[rng] * 1e3:.0f} mV"
          f" ({args.trig_frac / (args.mv_per_a / 1000.0) * RANGE_V[rng]:.2f} A) — close switch "
          f"({args.arm:.0f}s)...")
    try:
        a, b, ov = s.capture(tb, nsamp, args.ch_b, arm_timeout=args.arm)
    except RuntimeError as e:
        print(f"  {e}")
        return None
    amps = [v / (args.mv_per_a / 1000.0) for v in to_volts(a, rng)]
    f = fit_didt(amps, interval)
    if not f:
        print("  fit failed: no clean rising ramp")
        return None
    slope, i0, i1, n = f
    if b is not None:
        vb = sorted(to_volts(b, RANGES[args.vb_range])[i0:i1 + 1])
        V, vsrc = vb[len(vb) // 2], "CH B"
    else:
        V, vsrc = args.didt, "typed"
    L = V / slope
    print(f"  di/dt {slope / 1e6:.4f} A/us  V {V:.3f} ({vsrc})  imax {max(amps):.2f} A  ->  "
          f"L = {L * 1e6:.1f} uH" + ("  OVERFLOW" if ov else ""))
    return L


def ascii_wave(ys, width=70, height=12):
    if len(ys) < 2:
        return
    step = max(1, len(ys) // width)
    s = ys[::step][:width]
    y0, y1 = min(s), max(s)
    if y1 == y0:
        y1 = y0 + 1e-9
    grid = [[" "] * len(s) for _ in range(height)]
    for x, v in enumerate(s):
        r = height - 1 - round((v - y0) / (y1 - y0) * (height - 1))
        grid[r][x] = "*"
    for row in grid:
        print("   " + "".join(row))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--range", default="200mv", choices=RANGES, help="channel A full-scale")
    ap.add_argument("--mv-per-a", type=float, default=100.0, help="clamp output mV per amp")
    ap.add_argument("--fsw", type=float, help="switching freq Hz: size window to ~4 periods")
    ap.add_argument("--samples", type=int, default=3000, help="samples per block (<= ~3968 buffer)")
    ap.add_argument("--window", type=float, help="capture window seconds (overrides --fsw sizing)")
    ap.add_argument("--coupling", choices=["dc", "ac"], default="dc")
    ap.add_argument("--ch-b", action="store_true",
                    help="capture channel B across the coil; [--didt] uses its mid-ramp voltage as V")
    ap.add_argument("--vb-range", default="2v", choices=RANGES, help="channel B full-scale")
    ap.add_argument("--didt", type=float, metavar="VOLTS",
                    help="single-shot L measurement: nominal coil voltage (trigger context); with "
                         "--ch-b the actual V is read from channel B. Reports L = V/(di/dt)")
    ap.add_argument("--trig-frac", type=float, default=0.05,
                    help="[--didt] trigger threshold as fraction of full-scale (default 0.05)")
    ap.add_argument("--trig-dir", choices=["rising", "falling"], default="rising",
                    help="[--didt] edge to trigger on (flip if your clamp ramps negative)")
    ap.add_argument("--arm", type=float, default=30.0,
                    help="[--didt] seconds to wait for the trigger after arming (close the switch)")
    ap.add_argument("--shots", type=int, default=1,
                    help="[--didt] re-arm and capture N times; report median L (cycle the switch each)")
    ap.add_argument("--monitor", action="store_true",
                    help="free-run loop printing CH A/B min/max/pk-pk (close the switch and watch "
                         "the sign + amplitude to set --trig-frac / --trig-dir)")
    args = ap.parse_args()

    nsamp = args.samples
    if args.window:
        want = args.window / nsamp
    elif args.didt:
        want = 400e-6 / nsamp  # ~400 us window spans a slow ramp
    elif args.fsw:
        want = (4.0 / args.fsw) / nsamp
    else:
        want = 250e-9  # 250 ns/sample default

    rng = RANGES[args.range]
    s = PS2000()
    try:
        s.channel(CH_A, rng, DC if args.coupling == "dc" else AC)
        if args.ch_b:
            s.channel(CH_B, RANGES[args.vb_range], DC)
        tb, interval, mx = s.timebase_for(want, nsamp)
        if args.monitor:
            import time
            s.trigger(5, 0, rising=True, delay_pct=0, auto_ms=0)
            mva = args.mv_per_a / 1000.0
            print(f"monitor @ {1 / interval / 1e6:.1f} MS/s, {interval * len(range(nsamp)) * 1e6:.0f} "
                  f"us window. Ctrl-C to stop. Close the switch and watch CH A.")
            try:
                while True:
                    a, b, ov = s.capture(tb, nsamp, args.ch_b, arm_timeout=2.0)
                    va = to_volts(a, rng)
                    msg = (f"CH A {min(va) * 1e3:+7.1f}..{max(va) * 1e3:+7.1f} mV  "
                           f"pp {(max(va) - min(va)) * 1e3:6.1f} mV = {(max(va) - min(va)) / mva:5.2f} A")
                    if b is not None:
                        vb = to_volts(b, RANGES[args.vb_range])
                        msg += f"   CH B {min(vb):+6.3f}..{max(vb):+6.3f} V"
                    if ov:
                        msg += "  OVERFLOW"
                    print("  " + msg)
                    time.sleep(0.2)
            except KeyboardInterrupt:
                print("\nstopped.")
            return
        if args.didt:
            print(f"di/dt mode @ {1 / interval / 1e6:.2f} MS/s, {interval * nsamp * 1e6:.0f} us window, "
                  f"{args.shots} shot(s)")
            Ls = []
            for k in range(args.shots):
                if args.shots > 1:
                    print(f"-- shot {k + 1}/{args.shots} (cycle the switch) --")
                L = didt_shot(s, args, rng, tb, interval, nsamp)
                if L:
                    Ls.append(L)
            if Ls:
                Ls.sort()
                med = Ls[len(Ls) // 2]
                lo, hi = Ls[0], Ls[-1]
                print(f"\n=== {len(Ls)}/{args.shots} good: L median {med * 1e6:.1f} uH"
                      + (f", range {lo * 1e6:.1f}..{hi * 1e6:.1f} ({(hi - lo) / med * 100:.0f}%)"
                         if len(Ls) > 1 else "") + " ===")
                print(f"  coil.conf L0={med:.3e}  (CH B includes i*DCR -> biased high; "
                      "lower supply V / current to shrink it)")
            return

        s.trigger(5, 0, rising=True, delay_pct=0, auto_ms=0)  # free-run single capture
        a, b, ov = s.capture(tb, nsamp, args.ch_b, arm_timeout=5.0)
        va = to_volts(a, rng)
        pp = max(va) - min(va)
        dur = interval * len(va)
        print(f"ps2000 handle={s.h}  range={args.range}  {1 / interval / 1e6:.2f} MS/s "
              f"({interval * 1e9:.0f} ns/sample)  {len(va)} samp = {dur * 1e6:.1f} us  overflow={ov}")
        print(f"  CH A: min {min(va) * 1e3:+.1f} mV  max {max(va) * 1e3:+.1f} mV  "
              f"pk-pk {pp * 1e3:.1f} mV  =>  {pp / (args.mv_per_a / 1000.0):.3f} A pk-pk @ "
              f"{args.mv_per_a:.0f} mV/A")
        ascii_wave(va)
    finally:
        s.close()


if __name__ == "__main__":
    reexec_x86()
    try:
        main()
    except RuntimeError as e:
        sys.exit(f"error: {e}")
