#!/usr/bin/env python3
"""Estimate the series resistance of the battery connection path from logged
Iout / Vout samples.

Model in a short window:

    V(t) = V0 + R * I(t)

with V0 (battery EMF) approximately constant over the window. The window slope
R is reported sample-by-sample. When the current barely varies within a window
the slope is dominated by noise, so the estimate is set to NaN.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from datetime import datetime
from pathlib import Path

import numpy as np


_SI_PREFIX = {
    "":  1.0,
    "k": 1e3,
    "M": 1e6,
    "m": 1e-3,
    "u": 1e-6, "µ": 1e-6, "μ": 1e-6,
    "n": 1e-9,
}


def _parse_value_with_unit(s: str, base_unit: str) -> float:
    """Parse '24.79 A' / '702.065 mA' / '28.6118' into a float in the base unit.

    `base_unit` is the unit the result should be expressed in (e.g. 'A', 'V').
    A bare number with no unit is taken to already be in `base_unit`."""
    s = s.strip()
    parts = s.split()
    if len(parts) == 1:
        return float(parts[0])
    val = float(parts[0])
    unit = parts[1]
    if unit == base_unit:
        return val
    if unit.endswith(base_unit) and len(unit) == len(base_unit) + 1:
        prefix = unit[0]
        if prefix in _SI_PREFIX:
            return val * _SI_PREFIX[prefix]
    raise ValueError(f"unrecognized unit {unit!r} for base {base_unit!r}")


def _load(path: Path, base_unit: str):
    ts, vals = [], []
    with open(path, newline="") as f:
        r = csv.reader(f)
        next(r)  # header
        for row in r:
            if len(row) < 2 or not row[1].strip():
                continue
            ts.append(datetime.fromisoformat(row[0]).timestamp())
            vals.append(_parse_value_with_unit(row[1], base_unit))
    return np.asarray(ts, dtype=float), np.asarray(vals, dtype=float)


def load_pair(current_csv: Path, voltage_csv: Path):
    """Load and time-align current and voltage CSVs.

    Both files are expected to share a common sampling grid (intersection of
    timestamps is used). Values carrying an SI-prefixed unit (e.g. 'mA') are
    converted to the base unit."""
    tI, I = _load(current_csv, "A")
    tV, V = _load(voltage_csv, "V")
    common, iI, iV = np.intersect1d(tI, tV, return_indices=True)
    return common, I[iI], V[iV]


def rolling_resistance(
    I: np.ndarray,
    V: np.ndarray,
    window: int,
    min_std_I: float,
    min_valid_frac: float = 0.5,
):
    """Sliding-window OLS slope of V vs. I.

    Parameters
    ----------
    I, V : 1-D arrays of equal length, aligned in time.
    window : window length in samples. The estimate at index k uses samples
        [k-window+1 .. k] (causal / trailing window).
    min_std_I : minimum standard deviation of I within the window. Below this
        the slope is reported as NaN.
    min_valid_frac : minimum fraction of finite samples within the window
        required to attempt a fit.

    Returns
    -------
    R, V0, sigma_I, r2 : arrays of length len(I).
    """
    n = len(I)
    R = np.full(n, np.nan)
    V0 = np.full(n, np.nan)
    sigmaI = np.full(n, np.nan)
    r2 = np.full(n, np.nan)
    min_pts = max(3, int(min_valid_frac * window))
    for k in range(window - 1, n):
        Iw = I[k - window + 1 : k + 1]
        Vw = V[k - window + 1 : k + 1]
        m = np.isfinite(Iw) & np.isfinite(Vw)
        if m.sum() < min_pts:
            continue
        Iw = Iw[m]
        Vw = Vw[m]
        sI = float(Iw.std(ddof=1))
        sigmaI[k] = sI
        if sI < min_std_I:
            continue
        # closed-form OLS to avoid polyfit overhead
        Ibar = Iw.mean()
        Vbar = Vw.mean()
        dI = Iw - Ibar
        dV = Vw - Vbar
        Sxx = float(dI @ dI)
        if Sxx <= 0.0:
            continue
        slope = float(dI @ dV) / Sxx
        intercept = Vbar - slope * Ibar
        Syy = float(dV @ dV)
        R[k] = slope
        V0[k] = intercept
        r2[k] = 0.0 if Syy <= 0.0 else 1.0 - (Syy - slope * float(dI @ dV)) / Syy
    return R, V0, sigmaI, r2


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("current_csv", type=Path)
    p.add_argument("voltage_csv", type=Path)
    p.add_argument("-w", "--window", type=int, default=30,
                   help="window length in samples (default %(default)s, "
                        "~60 s at 2 s sampling)")
    p.add_argument("--min-std-i", type=float, default=0.5,
                   help="minimum stddev of I in the window required to emit a "
                        "non-NaN estimate, in amps (default %(default)s)")
    p.add_argument("--min-r2", type=float, default=0.0,
                   help="if >0, NaN out windows whose linear fit R² is below "
                        "this threshold")
    p.add_argument("-o", "--out", type=Path,
                   help="write per-sample CSV (t,I,V,R,V0,sigmaI,r2) here")
    p.add_argument("--plot", action="store_true",
                   help="show I, V, R timeseries with matplotlib")
    p.add_argument("--save-plot", type=Path,
                   help="save the plot to this path instead of (or in addition "
                        "to) showing it")
    args = p.parse_args(argv)

    t, I, V = load_pair(args.current_csv, args.voltage_csv)
    if len(t) == 0:
        sys.exit("no overlapping samples in the two CSVs")

    R, V0, sI, r2 = rolling_resistance(
        I, V,
        window=args.window,
        min_std_I=args.min_std_i,
    )
    if args.min_r2 > 0:
        R = np.where(r2 >= args.min_r2, R, np.nan)

    valid = np.isfinite(R)
    print(f"samples            : {len(t)}")
    print(f"windows with R     : {int(valid.sum())} "
          f"({100*valid.mean():.1f}%)")
    if valid.any():
        print(f"R median           : {np.nanmedian(R)*1000:+.2f} mΩ")
        print(f"R 10/90 percentile : {np.nanpercentile(R,10)*1000:+.2f} / "
              f"{np.nanpercentile(R,90)*1000:+.2f} mΩ")
        print(f"sigma(I) median    : {np.nanmedian(sI):.2f} A")
        print(f"R² median          : {np.nanmedian(r2):.3f}")

    if args.out:
        with open(args.out, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_unix", "I_A", "V_V", "R_ohm", "V0_V", "sigmaI_A", "r2"])
            for i in range(len(t)):
                w.writerow([
                    f"{t[i]:.0f}",
                    f"{I[i]:.4f}",
                    f"{V[i]:.4f}",
                    "" if not math.isfinite(R[i]) else f"{R[i]:.6f}",
                    "" if not math.isfinite(V0[i]) else f"{V0[i]:.4f}",
                    "" if not math.isfinite(sI[i]) else f"{sI[i]:.4f}",
                    "" if not math.isfinite(r2[i]) else f"{r2[i]:.4f}",
                ])
        print(f"wrote {args.out}")

    if args.plot or args.save_plot:
        import matplotlib.pyplot as plt
        td = (t - t[0]) / 60.0
        fig, axs = plt.subplots(3, 1, sharex=True, figsize=(11, 7))
        axs[0].plot(td, I, lw=0.8)
        axs[0].set_ylabel("I [A]")
        axs[0].grid(True, alpha=0.3)
        axs[1].plot(td, V, lw=0.8, color="tab:orange")
        axs[1].set_ylabel("V [V]")
        axs[1].grid(True, alpha=0.3)
        axs[2].plot(td, R * 1000, lw=0.8, color="tab:green")
        axs[2].set_ylabel("R [mΩ]")
        axs[2].set_xlabel("t [min]")
        axs[2].grid(True, alpha=0.3)
        if valid.any():
            axs[2].axhline(np.nanmedian(R) * 1000, color="k",
                           lw=0.5, ls="--",
                           label=f"median {np.nanmedian(R)*1000:.1f} mΩ")
            axs[2].legend(loc="best")
        fig.suptitle(f"Battery path resistance — window={args.window} samples, "
                     f"min σ(I)={args.min_std_i} A")
        fig.tight_layout()
        if args.save_plot:
            fig.savefig(args.save_plot, dpi=120)
            print(f"wrote {args.save_plot}")
        if args.plot:
            plt.show()


if __name__ == "__main__":
    main()
