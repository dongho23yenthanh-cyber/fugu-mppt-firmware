
victron bulk-float-absorption
https://communityarchive.victronenergy.com/questions/63640/charging-bulk-absorption-and-float.html
absorption = 3.55V
float = 3.375


# from claude:
With 15% daily DoD, cycle life is essentially a non-issue — you'd hit 6000+ cycles before the cells care. Calendar aging and BMS health dominate. Optimize for those.

**Daily profile — what to actually do**
Lower the daily absorption ceiling. Instead of charging to 3.65 V/cell every day, set the charge controller to 3.45–3.50 V/cell absorption with a short hold (15–30 min). This gets you to ~95% SoC and stops. Resting voltage settles around 3.35 V/cell. Result: pack spends most of the day at ~90–95% instead of pinned at 100%, which meaningfully reduces calendar aging at the top.

**Float**
Disable, or set to 3.35 V/cell (which is essentially a no-current rest voltage — the pack won't actually draw at that setpoint). Do not run a real float current into LFP all afternoon.

**Re-bulk**
Set re-bulk threshold around 3.30 V/cell so the controller doesn't oscillate. With 15% overnight DoD you'll re-bulk once each morning, cleanly.

**Periodic full charge for balancing**
Once a month (or when cell drift exceeds ~20–30 mV per your BMS), raise absorption to 3.60–3.65 V/cell and let the BMS top-balance. Schedule it for a sunny day so you actually reach the knee.

**Disable**
Temperature compensation (LFP doesn't want it — that's a lead-acid setting), equalization charges, any "desulfation" mode if the controller has lead-acid heritage.

**Reserve sizing**
Off-grid logic pushes against the "limit to 80%" rule of thumb — you need headroom for cloudy stretches. The 15% nightly suggests you're already sized comfortably. Aim to keep the working band ~30–95% with occasional excursions either way. Don't routinely pull below ~20% just to "exercise" the battery; that's worse than sitting high.

**Temperature**
Critical if the pack is in an unconditioned space. If it sees sub-zero mornings, the BMS must block charging until cells warm, or you need self-heating cells / a heater pad. Solar arriving at dawn into a frozen pack is the classic off-grid LFP killer.

**One thing to verify**
Your charge controller's "lithium" or "user" profile — many cheap units have a lithium preset that's actually still floating at 3.55 V/cell. Read the actual values, don't trust the label.