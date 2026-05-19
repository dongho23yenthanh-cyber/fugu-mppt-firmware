*this document is an LLM generated placeholder*


# Charge Termination

`Li_ChgTerminationCondition` in `src/charger.h` implements a charge-termination
line for LFP and other lithium chemistries, as described in
[Charging Marine Lithium Battery Banks](https://nordkyndesign.com/charging-marine-lithium-battery-banks/).

The model: the cell sits at `cv_min` (the "float" voltage) at zero current and
at `cv_eoc` (the absorption voltage) when the charging current equals the
tail-current threshold. Between those two operating points, a straight line in
the (current, voltage) plane defines the termination boundary — anything above
the line is "still charging," anything below is "done."

Restated as an apparent series resistance:

```
r = (cv_eoc - cv_min) / (tail_c_rate * Cbat)
```

so the per-current termination voltage becomes

```
v_term(ibat) = cv_min + ibat * r       (clamped at cv_eoc)
```

The charger declares termination when the highest reported cell voltage
crosses `v_term`.

## The `tail_c_rate` parameter

`tail_c_rate` is the C-rate at which the cell is considered "fully charged" —
the tail current expressed as a fraction of capacity. It's configurable via
`charger.conf:tail_c_rate` and defaults to `0.05` (LFP).

| Chemistry                            | `tail_c_rate` |
|--------------------------------------|---------------|
| LFP                                  | `0.05`        |
| Sanyo/Panasonic NCR18650GA (67 mA / 3500 mAh, [datasheet](https://www.orbtronic.com/content/Datasheet-specs-Sanyo-Panasonic-NCR18650GA-3500mah.pdf)) | `~0.02` |
| EVE INR18650                         | `0.033`       |

**Higher = safer.** A larger `tail_c_rate` produces a steeper termination line,
so termination triggers at a higher current — i.e. *earlier* in the
constant-voltage phase, leaving the cell less fully charged but with more
headroom against over-charge.

**Lower = more thorough but tighter margin.** Smaller `tail_c_rate` means the
charger holds CV down to a smaller tail current before terminating; the cell
ends up closer to its true full-charge state but the cell-voltage tolerance is
narrower.

For unknown cells, the default `0.05` is the conservative choice — termination
may be slightly early for chemistries that prefer a smaller tail, but it will
never be premature on LFP and never over-charges.

## Recharge hysteresis (DoD-based release)

LFP cells discharge nearly flat: a 1 % SoC drop from full can take voltage from
3.45 V down to 3.35 V. Releasing termination on `vcell_high < cv_min` alone
therefore causes micro-cycling at the top of charge — the charger oscillates
between "terminated" and "charging" every few minutes while the cells barely
move.

To fix this, the charger integrates `-ibat` over time (positive = pack
discharging) and tracks Ah-since-the-last-full event. Termination releases
when *either* of:

- **Ah condition (primary)**: `ahSinceFull > recharge_dod * Cbat`. With the
  default `recharge_dod = 0.20` and a 280 Ah pack, this is ~56 Ah of net
  discharge before recharge is permitted.
- **Voltage floor (fallback)**: `vcell_high < cv_min - 0.05 V` (3.32 V for
  LFP). Catches integrator drift, wrong `Cbat`, missing BMS — anything that
  would otherwise leave the charger stuck terminated while the pack is
  genuinely empty. Reliability beats elegance here.

On every termination event the integrator is re-zeroed, so it doesn't have to
be a precise long-term SoC gauge — it's a "deficit since the last known full"
counter that self-recalibrates each cycle.

State is **RAM-only**. On reboot the counter starts at 0, and the next full
charge re-establishes the reference. The first post-boot cycle releases on the
voltage-floor fallback (same as the pre-Ah-counting behaviour).

`recharge_dod` is configurable via `charger.conf:recharge_dod`. Higher = release
later (deeper discharges between full charges = fewer cycles but more time at
mid-SoC). Lower = release sooner (more frequent topping = more cycle wear but
less time below 100 %). LFP off-grid systems commonly run 0.10–0.30.

## Related parameters

- `cv_eoc` — absorption voltage (LFP: 3.65 V)
- `cv_float` (loaded into `cv_min`) — float voltage (LFP: 3.37 V)
- `bat_c` — effective pack capacity in Ah. For parallel packs, use the summed
  Ah (e.g. 2P 280 Ah → 560). The model assumes `Cbat` is the parallel-effective
  capacity.
- `recharge_dod` — DoD threshold for releasing termination (see above).

If `bat_c` is missing, `r` is non-finite and the impedance-compensation branch
degrades to "absorption-only" — pack-voltage pinning still caps at `cv_eoc`
via `Vbat_fallback`, so the charger is safe but does not fully charge. The Ah
release condition is also disabled in that case; only the voltage floor remains.
