---
name: wsync-coupling-network-numbers
description: Worked numbers for the wired-sync AC coupling net (R1 33k/R2 10k/C1 1n/C2 10n) — real bias is 0.64 V not 0.77 V, C2 is in series with C1, no CM rejection above DC
metadata:
  type: project
---

Verified against `doc/dev-notes/wired-sync.md` (ESP32-S3: R_PD = 45 kΩ typ, V_IL,max = 0.25·VDD =
0.825 V, V_IH,min = 0.75·VDD = 2.475 V).

- Doc's 0.77 V idle bias ignores the internal pull-down that `initSyncIn` explicitly forces
  (`GPIO_PULLDOWN_ONLY` + sync-src `pull_down=1`). With 45 k in series with the 1 k, the pin sits at
  **0.64 V**; over the 10–80 kΩ R_PD spread it ranges 0.41–0.69 V. Direction is safe (further under
  V_IL), but 0.77 V alone would leave only 57 mV of V_IL margin.
- Thevenin: 33k‖10k = 7.67 k (doc's 7.6 k ✓), but with the 1 k + R_PD branch it is **6.58 k**.
- **C2 (10 nF) is electrically in series with C1 (1 nF)** in the pulse loop → C_eff = 0.909 nF, τ =
  6.1 µs not 8 µs. Raising C2 to 100 nF makes it a true AC bond (C_eff 0.99 nF) at zero cost.
- 1 µs pulse: node steps to 3.89 V (upper ESD diode clamps at ~3.6 V, 1 k limits to ~0.3 mA), droops
  14–15 %, ends at 3.40 V — far above V_IH ✓. Post-fall undershoot lands at +0.17 V, i.e. it does
  **not** go negative; the BAT54S/undershoot claim in the doc is unnecessary at 1 µs. Negative
  undershoot starts at ~1.4 µs pulse width, so the 1 µs choice sits only 1.4× from that corner.
- Peak pulse current 0.49 mA, ~0.5 nC/pulse — C2's 45 Ω @350 kHz is fine for return energy.
- **The network rejects only DC common mode.** A ground-to-ground dV/dt step couples through C1
  (53 Ω @3 MHz vs the 6.6 k node) at near-full amplitude — a fast CM transient from the power stage
  is indistinguishable from the sync pulse. Calling it "galvanically isolated" is wrong. Real fixes:
  pulse transformer / digital isolator / differential pair, or at minimum a Schmitt buffer + glitch
  filter. See [[wsync-sync-event-shootthrough]] for why a false edge is expensive.
