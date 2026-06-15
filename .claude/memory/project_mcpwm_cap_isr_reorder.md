---
name: mcpwm-cap-isr-reorder
description: MCPWM CAP ISRs can complete out-of-order for edges <1µs apart; sort the ring by ts before sequential analysis
metadata: 
  node_type: memory
  type: project
  originSessionId: d4ceb270-1324-43fd-9eb4-1d2cc8a41800
---

The MCPWM CAP channels each have their own ISR. When two channels fire within
~1 µs of each other (e.g. HS-fall at t and LS-rise at t+200 ns from a
dead-band pair), the two ISRs can complete in either order, pushing samples
to the shared ring with timestamps that aren't monotonically increasing.

The HW capture timestamps themselves are correct — only the **enqueue order**
is wrong. Any state machine that assumes time-ordered iteration over the ring
(like `analyse_deadbands` in `test/test_pwm.cpp`) will see edges in the wrong
sequence and either drop pairs or pair across periods.

**Fix:** insertion-sort the ring by `ts` before sequential analysis. Use
signed-difference comparison to handle uint32_t wraparound:
```c
while (j > 0 && (int32_t)(g_ring[j-1].ts - cur.ts) > 0) { ... }
```

**Also:** state machines that pair edges across periods (e.g. LS-fall[k] →
HS-rise[k+1]) must invalidate their "have prev event" flag on every period
boundary AND in every "no-match" else branch. Otherwise a missed event leaks
a stale timestamp into the next period's pairing, producing N-period outliers
in the gap distribution. See [[mcpwm-dt-pair-two-calls]] for the underlying
fix that brought these tests online.

**How to apply:** any future test that uses multiple CapChan on one CapTimer
needs the ts-sort step. The sort is O(N) on a near-sorted ring so cheap.
