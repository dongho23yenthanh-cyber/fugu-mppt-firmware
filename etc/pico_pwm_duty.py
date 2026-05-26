#!/usr/bin/env python3
"""Auto-stepped PicoScope sweep for the MCPWM endpoint-duty test.

Pairs with test_mcpwm_endpoint_duty_scope in test/test_pwm.cpp: the device dwells on
each duty in {0.05, 0.10, 0.25, 0.50, 0.75, 0.90, 0.95} for ~5 s and prints sync
markers ("[SCOPE] D=X.XX cmp=N start") on UART. This script tails the serial port,
captures a PicoScope window mid-dwell, measures the pulse-width / period over many
cycles, and prints a (configured, measured) table.

Like pico_capture.py, the ps2000 dylib is x86_64-only — auto re-execs under Rosetta
on Apple Silicon.

Wiring: PicoScope CH A probe on the device's HS pin (default GPIO 5), ground clip
to device GND. CH A range = 5 V DC (3.3 V logic fits with headroom).

Usage:
    arch -x86_64 /usr/bin/python3 etc/pico_pwm_duty.py \\
        --port /dev/cu.usbmodem59720648061 --fsw 39000
"""

import argparse
import os
import re
import sys
import time

# Reuse the PS2000 wrapper + Rosetta re-exec from pico_capture.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pico_capture as pc  # noqa: E402

MARK_RE = re.compile(r"\[SCOPE\] D=([0-9.]+) cmp=(\d+) start")
END_RE  = re.compile(r"\[SCOPE\] D=([0-9.]+) end")


def open_serial(port, baud=115200):
    import serial
    s = serial.Serial(port, baud, timeout=0.2)
    return s


class SerialLineSink:
    """Read lines from serial, preserving partial-line and post-match remainder
    across calls (wait_for losing buffered lines was the every-other-dwell bug)."""
    def __init__(self, port):
        self.port = port
        self.buf  = b""

    def wait_for(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace")
                m = pattern.search(text)
                if m:
                    return m, text
            chunk = self.port.read(2048)
            if chunk:
                self.buf += chunk
        return None, None


def measure_one_dwell(ps, tb, interval, nsamp, fsw, threshold_counts, ch):
    """Capture once mid-dwell, hunt rising/falling crossings, return (mean_duty, mean_pw_ns, n_pulses)."""
    # Edge trigger on the measurement channel's rising edge.
    ps.trigger(ch, threshold_counts, rising=True, delay_pct=-10, auto_ms=500)
    try:
        # ps_capture's `want_b` controls *which* channel(s) get returned. We need
        # the measurement channel back.
        a, b, ov = ps.capture(tb, nsamp, want_b=(ch == pc.CH_B), arm_timeout=2.0)
    except RuntimeError as e:
        return None, None, 0, str(e)
    if ov:
        return None, None, 0, "overflow"
    samples = b if ch == pc.CH_B else a
    rises, falls = [], []
    prev_above = samples[0] > threshold_counts
    for i in range(1, len(samples)):
        above = samples[i] > threshold_counts
        if above and not prev_above:
            rises.append(i)
        elif not above and prev_above:
            falls.append(i)
        prev_above = above
    if len(rises) < 2 or not falls:
        return None, None, 0, f"too few edges: rises={len(rises)} falls={len(falls)}"

    # Pulse widths: pair each rise with the next fall after it (interval > 0).
    widths = []
    for r in rises:
        nf = next((f for f in falls if f > r), None)
        if nf is None:
            break
        widths.append((nf - r) * interval)

    # Periods: rise[k+1] - rise[k]
    periods = [(rises[k + 1] - rises[k]) * interval for k in range(len(rises) - 1)]
    if not widths or not periods:
        return None, None, 0, "no pulse/period pairs"

    mean_pw = sum(widths) / len(widths)
    mean_pd = sum(periods) / len(periods)
    duty    = mean_pw / mean_pd
    return duty, mean_pw, len(widths), None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port for sync markers (bridge UART)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--channel", choices=["A", "B"], default="A",
                    help="scope channel that carries the HS signal (default A)")
    ap.add_argument("--range", default="5v", choices=pc.RANGES, help="measurement channel full-scale")
    ap.add_argument("--atten", type=float, default=1.0,
                    help="probe attenuation ratio on the measurement channel (e.g. 50; default 1)")
    ap.add_argument("--threshold-v", type=float, default=1.65,
                    help="logic mid-level (at the pin, before attenuation) for edge detection")
    ap.add_argument("--fsw", type=float, default=39000,
                    help="expected switching freq Hz; sizes the capture window")
    ap.add_argument("--periods", type=int, default=20,
                    help="how many periods to fit in one capture (default 20)")
    ap.add_argument("--samples", type=int, default=3000,
                    help="samples per block (PicoScope buffer cap ~3968)")
    ap.add_argument("--shots", type=int, default=4,
                    help="captures averaged per duty (default 4)")
    ap.add_argument("--total-timeout", type=float, default=90.0,
                    help="serial wait for the whole sweep (default 90 s)")
    ap.add_argument("--monitor", action="store_true",
                    help="don't run a sweep — just free-run capture both channels and print "
                         "min/max/pk-pk on CH A. Ctrl-C to stop. Use to validate wiring.")
    ap.add_argument("--vb-range", default="5v", choices=pc.RANGES, help="[--monitor] CH B full-scale")
    args = ap.parse_args()

    rng = pc.RANGES[args.range]
    fs_v = pc.RANGE_V[rng]
    threshold_at_scope = args.threshold_v / args.atten  # voltage at the BNC input
    threshold_counts = int(threshold_at_scope / fs_v * pc.MAX_ADC)
    want_interval = (args.periods / args.fsw) / args.samples
    meas_ch = pc.CH_A if args.channel == "A" else pc.CH_B

    ser  = open_serial(args.port, args.baud)
    sink = SerialLineSink(ser)
    print(f"serial:  {args.port} @ {args.baud}", flush=True)
    print(f"scope:   CH{args.channel}  range={args.range}  atten={args.atten:g}x  "
          f"threshold={args.threshold_v:.2f} V at pin = {threshold_at_scope * 1e3:.1f} mV at BNC "
          f"= {threshold_counts} counts", flush=True)

    s = pc.PS2000()
    try:
        s.channel(meas_ch, rng, pc.DC)
        if args.monitor:
            s.channel(pc.CH_B, pc.RANGES[args.vb_range], pc.DC)
            tb, interval, _ = s.timebase_for(want_interval, args.samples)
            s.trigger(5, 0, rising=True, delay_pct=0, auto_ms=0)  # free-run
            mva_a = 1.0 / args.atten  # scope-V → pin-V multiplier
            print(f"monitor @ {1 / interval / 1e6:.2f} MS/s, "
                  f"{interval * args.samples * 1e6:.0f} µs window. Ctrl-C to stop.", flush=True)
            try:
                while True:
                    a, b, ov = s.capture(tb, args.samples, want_b=True, arm_timeout=2.0)
                    va = pc.to_volts(a, rng)
                    vb = pc.to_volts(b, pc.RANGES[args.vb_range])
                    msg = (f"CH A {min(va)*1e3:+7.1f}..{max(va)*1e3:+7.1f} mV @ BNC "
                           f"= {min(va)/mva_a:+5.2f}..{max(va)/mva_a:+5.2f} V @ pin   "
                           f"CH B {min(vb):+5.2f}..{max(vb):+5.2f} V")
                    if ov:
                        msg += "  OVERFLOW"
                    print("  " + msg, flush=True)
                    time.sleep(0.3)
            except KeyboardInterrupt:
                print("\nstopped.", flush=True)
            return
        tb, interval, _ = s.timebase_for(want_interval, args.samples)
        print(f"timebase: {1 / interval / 1e6:.2f} MS/s ({interval * 1e9:.0f} ns/sample) "
              f"window={interval * args.samples * 1e3:.2f} ms (~{interval * args.samples * args.fsw:.1f} periods)",
              flush=True)
        print(f"waiting for [SCOPE] markers on serial (≤{args.total_timeout:.0f} s)...", flush=True)

        results = []  # (D_cmd, D_measured, pw_ns_measured, n_pulses_sum, shots_ok)
        deadline = time.monotonic() + args.total_timeout
        while time.monotonic() < deadline:
            m, _ = sink.wait_for(MARK_RE, timeout=deadline - time.monotonic())
            if not m:
                break
            d_cmd = float(m.group(1))
            cmp_v = int(m.group(2))

            # Wait a bit so the dwell is stable, then capture N shots.
            time.sleep(0.3)
            duties, pws, n_pulses_total, shots_ok = [], [], 0, 0
            errs = []
            for _ in range(args.shots):
                duty, pw, n, err = measure_one_dwell(s, tb, interval, args.samples,
                                                     args.fsw, threshold_counts, meas_ch)
                if err:
                    errs.append(err); continue
                duties.append(duty); pws.append(pw); n_pulses_total += n; shots_ok += 1
            if shots_ok == 0:
                print(f"  D={d_cmd:.2f} cmp={cmp_v} FAILED ({errs[:1]})", flush=True)
                results.append((d_cmd, None, None, 0, 0))
                continue
            duties.sort(); pws.sort()
            med_duty = duties[len(duties) // 2]
            med_pw   = pws[len(pws) // 2]
            err_ns   = (med_duty - d_cmd) * 1e9 / args.fsw
            print(f"  D={d_cmd:.2f} cmp={cmp_v}  measured={med_duty:.4f}  "
                  f"pw={med_pw * 1e9:.0f} ns  err={err_ns:+.0f} ns  "
                  f"(shots {shots_ok}/{args.shots}, n_pulses={n_pulses_total})", flush=True)
            results.append((d_cmd, med_duty, med_pw, n_pulses_total, shots_ok))

            # Drain until the matching "end" marker so we don't double-trigger on a stale "start".
            sink.wait_for(END_RE, timeout=10.0)

        if not results:
            sys.exit("no [SCOPE] markers seen — check serial port + that the test ran.")

        print("\n=== summary ===")
        print(f"{'D_cmd':>6} {'D_meas':>8} {'pw_ns':>8} {'err_ns':>8} {'shots':>6}")
        for d_cmd, d_meas, pw, n, ok in results:
            if d_meas is None:
                print(f"{d_cmd:>6.2f} {'FAIL':>8}")
                continue
            err_ns = (d_meas - d_cmd) * 1e9 / args.fsw
            print(f"{d_cmd:>6.2f} {d_meas:>8.4f} {pw * 1e9:>8.0f} {err_ns:>+8.0f} {ok:>6}")
    finally:
        s.close()
        ser.close()


if __name__ == "__main__":
    pc.reexec_x86()
    try:
        main()
    except RuntimeError as e:
        sys.exit(f"error: {e}")
