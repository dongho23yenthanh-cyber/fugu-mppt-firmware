#!/usr/bin/env python3
"""MCPWM gate-drive automated verification: drives the device via console + captures HS/LS
gates on a PicoScope 2000, asserts frequency / duty / dead-time / no-shoot-through.

Wiring: Ch A on the HS gate pin (IN or HI), Ch B on the LS gate pin (EN or LI). 0-3.3V logic,
no clamp. Both DC-coupled, 5V range.

Run after flashing WITH_MCPWM=1 and provisioning a board.conf with `pwm_deadtime_ns`. The
device must be in manual-PWM mode (the `dc N` command switches to it).

    etc/mcpwm_gate_verify.py --serial /dev/cu.usbmodem* --duty-sweep 50,200,500,1000
    etc/mcpwm_gate_verify.py --ip 192.168.1.173 --port 232  --pwm-max 2048 --deadtime-ns 80
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from fugu.console import Console
from fugu.transport import SerialTransport, SocketTransport

# pico_capture exposes a PS2000 class; reuse it directly.
import pico_capture as pc


LOGIC_THRESHOLD_V = 1.65  # midway through 0-3.3V logic swing


def open_console(args):
    if args.serial:
        t = SerialTransport(args.serial, 115200)
    else:
        t = SocketTransport(args.ip, args.port)
    return Console(t)


def edges(samples_v, threshold=LOGIC_THRESHOLD_V):
    """Return (rising_idx, falling_idx) lists from a 0..3.3V logic waveform."""
    rises, falls = [], []
    prev = samples_v[0] > threshold
    for i in range(1, len(samples_v)):
        cur = samples_v[i] > threshold
        if cur and not prev:
            rises.append(i)
        elif prev and not cur:
            falls.append(i)
        prev = cur
    return rises, falls


def capture_pair(scope, fsw, window_periods=4, samples=3000):
    """Block-capture both channels around a HS rising edge; returns (dt_sec, hs_v, ls_v)."""
    window = window_periods / fsw
    tb, interval_ns, nsamp = scope.timebase_for(window, samples)  # adapt to pico_capture API
    hs_counts, ls_counts = scope.capture_block_ab(tb, nsamp, trig_chan="A", trig_dir="rising")
    dt = interval_ns * 1e-9
    return dt, pc.to_volts(hs_counts, pc.RANGES["5v"]), pc.to_volts(ls_counts, pc.RANGES["5v"])


def check_waveform(dt, hs, ls, *, fsw_expect, pwm_max, pwm_ctrl, deadtime_ns):
    """Returns (pass: bool, report: dict)."""
    r_hs, f_hs = edges(hs)
    r_ls, f_ls = edges(ls)
    if len(r_hs) < 2 or len(f_hs) < 1 or len(r_ls) < 1 or len(f_ls) < 1:
        return False, {"err": "too few edges (gates not switching?)"}

    period_s = (r_hs[1] - r_hs[0]) * dt
    freq = 1.0 / period_s
    hs_high_s = (f_hs[0] - r_hs[0]) * dt
    duty_hs = hs_high_s / period_s
    duty_expect = pwm_ctrl / pwm_max

    # Dead-time at HS->LS edge (HS falls, LS rises). Find first LS rise after r_hs[0].
    ls_rise_after = next((i for i in r_ls if i > f_hs[0]), None)
    dt_hl_ns = (ls_rise_after - f_hs[0]) * dt * 1e9 if ls_rise_after else None
    # Dead-time at LS->HS edge.
    ls_fall_after = next((i for i in f_ls if i > ls_rise_after), None)
    dt_lh_ns = (r_hs[1] - ls_fall_after) * dt * 1e9 if ls_fall_after else None

    shoot_through = sum(1 for i in range(len(hs)) if hs[i] > LOGIC_THRESHOLD_V and ls[i] > LOGIC_THRESHOLD_V)

    rep = {
        "freq_hz": freq, "freq_err_pct": 100 * (freq - fsw_expect) / fsw_expect,
        "duty_hs": duty_hs, "duty_err_pct": 100 * (duty_hs - duty_expect),
        "deadtime_hl_ns": dt_hl_ns, "deadtime_lh_ns": dt_lh_ns,
        "shoot_through_samples": shoot_through,
    }
    ok = (abs(rep["freq_err_pct"]) < 0.5
          and abs(rep["duty_err_pct"]) < 2.0
          and (deadtime_ns == 0 or (dt_hl_ns and dt_hl_ns >= 0.7 * deadtime_ns))
          and (deadtime_ns == 0 or (dt_lh_ns and dt_lh_ns >= 0.7 * deadtime_ns))
          and shoot_through == 0)
    return ok, rep


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--serial", help="serial port (e.g. /dev/cu.usbmodem*)")
    src.add_argument("--ip", help="device IP")
    ap.add_argument("--port", type=int, default=23, help="telnet port (default 23)")
    ap.add_argument("--fsw", type=float, default=39000.0)
    ap.add_argument("--pwm-max", type=int, default=2048)
    ap.add_argument("--deadtime-ns", type=float, default=0.0)
    ap.add_argument("--duty-sweep", default="100,500,1000,1500,1900",
                    help="comma-sep pwmCtrl values to test")
    args = ap.parse_args()

    pc.reexec_x86()
    scope = pc.PS2000()
    # Setup once: Ch A + Ch B at 5V DC; rising trigger on A at 1.65V.
    scope.set_channel("A", range_key="5v", coupling="dc")
    scope.set_channel("B", range_key="5v", coupling="dc")
    scope.set_trigger("A", direction="rising", threshold_v=LOGIC_THRESHOLD_V)

    con = open_console(args)
    con.command("disable")          # converter off, gates low

    fails = 0
    for d in [int(x) for x in args.duty_sweep.split(",")]:
        con.command(f"dc {d}")
        time.sleep(0.1)             # let it settle
        dt, hs, ls = capture_pair(scope, args.fsw)
        ok, rep = check_waveform(dt, hs, ls,
                                 fsw_expect=args.fsw, pwm_max=args.pwm_max,
                                 pwm_ctrl=d, deadtime_ns=args.deadtime_ns)
        mark = "PASS" if ok else "FAIL"
        print(f"[{mark}] dc={d:4d}  f={rep.get('freq_hz', 0):.0f}Hz "
              f"({rep.get('freq_err_pct', 0):+.2f}%)  duty={rep.get('duty_hs', 0):.3f} "
              f"({rep.get('duty_err_pct', 0):+.2f}%)  "
              f"DT H->L={rep.get('deadtime_hl_ns')}ns L->H={rep.get('deadtime_lh_ns')}ns  "
              f"shoot={rep.get('shoot_through_samples')}")
        if not ok:
            fails += 1

    con.command("disable")
    scope.close()
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
