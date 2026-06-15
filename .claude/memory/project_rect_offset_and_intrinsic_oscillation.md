---
name: project_rect_offset_and_intrinsic_oscillation
description: "rect_offset calibrated per-board via on-device measure-coil ls (fry 100, flat 78 as of 2026-06-07); L-vs-H oscillation is intrinsic (WiFi-independent), dither can't fix it"
metadata: 
  node_type: memory
  type: project
  originSessionId: 0a0ec245-91f7-480d-ac84-ef1b597813ea
---

SR low-side dead-time offset calibrated via the ON-DEVICE `measure-coil ls [hs] [dwell_ms] [apply]` command (host-side `measure_coil.py --ls-sweep --apply` is the older path). Use a steep-edge HS (HS=500 ≈ D 0.24 on flat) where the Iout peak is locatable; **auto-HS is too low** — `measure-coil ls` with no HS picks ~244 on flat (D 0.12, Iout ~0.31A buried in noise) and returns garbage (peak pinned at the low sweep edge, e.g. -194ct). The robust quantity is the **offset (peak−ideal / peak−auto), not the absolute LS** — absolute LS shifts with Vin but the offset is constant across runs.

**fry rect_offset=100. flat rect_offset=78** (re-tuned 2026-06-07 from 57; HS=500 gave peak−ideal +90ct / peak−auto +58ct, reproducible across 3 runs, Iout_peak ~1.70A vs ~1.52A plateau = +12% at fixed duty). `apply` writes coil.conf rect_offset=peak−ideal−12 (safety bias) and sets it live. Also saved in gitignored `config/dl/<host>/conf/coil.conf` snapshots (corrected L0: fry 80e-6, flat 51e-6; flat's on-device coil.conf L0 currently 56e-6). rect_offset is L-independent (fixed gate-delay), per-board.

**Gotcha:** after `measure-coil ... apply`, flat did NOT auto-resume converting — it sat at duty 0 / 0.7W with st=↓MPPT ("MPPT already enabled" but stuck), recovered only after a manual `sweep`. measure-coil ends with duty 0/converter disabled and nothing triggers a fresh global sweep (no power-change to react to). Always confirm power recovered after measure-coil on a live converter; issue `sweep` if stranded at 0W.

The L-vs-H **oscillation** (~115 ct period on fry, ~100 on flat, ±10-25%) is **intrinsic**: a back-to-back BLE A/B with `wifi off` vs on was point-for-point identical, so it is NOT RF/CPU interference.

**Why the dither doesn't kill it:** the dither iterates fine even under held duty (RT loop calls `pwmPerturb(0)` each cycle → `computePwmRectMax`), toggling LS N↔N+1 to time-average the fractional ideal. But the Iout-vs-LS response is sharply asymmetric (flat body-diode plateau / steep reverse-current cliff), so averaging the toggled input across that nonlinearity gives a biased Iout that slides with H (Jensen). rect_offset=+100 moves the operating point toward the cliff (most asymmetric), so it can't help and may worsen the ripple — efficiency vs smoothness tradeoff.

**Why:** future "is the oscillation gone / is it WiFi" questions are answered — it's the [[project_diode_emulation_tests_todo]] SR-timing path, not WiFi.
**How to apply:** to actually attack it, test on `config/lab/dry_mock` sim (dither on/off × rect_offset 0/100); a fix likely needs asymmetric/stochastic LS dithering, not N↔N+1 toggling. Don't expect rect_offset or the current dither to reduce it.
