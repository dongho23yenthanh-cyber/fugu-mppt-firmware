*this document is an LLM generated placeholder*

# MCPWM synchronous-buck PWM driver

Design for replacing the LEDC gate driver (`src/pwm/ledc.h`) with an MCPWM-based
driver that preserves the software diode-emulation model in `src/buck.h`, adds
hardware dead-time and a zero-CPU GPIO fault brake, and supports interleaved legs.

## Goals

- Frequency and counts (`pwmMax`) configurable; default pins `pwmMax` to the current
  LEDC-equivalent so existing `rect_offset` / `pwmRectMin` calibration is bit-identical.
- A function computing the largest `period_ticks` (best duty resolution) for a frequency.
- Configurable hardware dead-time (`board.conf::pwm_deadtime_ns`).
- GPIO fault input that brakes both gates in hardware, no CPU involved.
- Multiple interleaved legs.
- Glitch-free, atomic duty updates.

## Non-goals

- Center-aligned (up-down) PWM — keep edge-aligned to preserve HS-at-TEZ alignment
  and the existing count semantics.
- On-chip analog comparator fault source — GPIO fault only for now.

## MCPWM resource mapping (ESP32-S3, `driver/mcpwm_prelude.h`)

S3: 2 groups, each with 3 timers, 3 operators, 2 generators + 2 comparators per
operator, plus dead-time and fault/brake modules.

One **synchronous leg** = one operator:

| Resource | Role |
|---|---|
| 1 timer, count-**up**, edge-aligned | period; HS rises at TEZ (count 0), preserving LEDC `hpoint=0` alignment |
| cmpHS, cmpLS | HS turn-off count, LS turn-off count |
| genHS → HS/IN pin, genLS → LS/EN pin | gate outputs |
| dead-time (per generator) | configurable posedge delay (HiLi only) |
| fault + brake (group level) | GPIO instant shutdown |

Generator actions (replicates the current LEDC scheme):

- `genHS`: TEZ → HIGH, cmpHS → LOW  ⟹ HS on `[0, hsOff]`
- `genLS`: cmpHS → HIGH, cmpLS → LOW ⟹ LS on `[hsOff, lsOff]`

So `hsOff ≡ pwmCtrl` and `lsOff ≡ pwmCtrl + pwmRect`. The DCM zero-crossing logic in
`buck.h` keeps computing `lsOff` exactly as today.

### Wiring modes (mirror `board.conf::pwm_driver_logic`)

- `HiLi` — genHS/genLS are the complementary HS/LS gates; dead-time module active.
- `InEn` — genHS = IN `[0, hsOff]`, genLS = EN window `[0, lsOff]`; dead-time 0 (the
  IR2814-class driver inserts it). Only difference: genLS "on" action moves from cmpHS
  to TEZ.

## Best-resolution timing

MCPWM clocks from PLL_160M. `period_ticks = resolution_hz / fsw`, capped at 16-bit
(65535). Maximize ticks → maximize resolution:

```cpp
struct PwmTiming { uint32_t resolution_hz, period_ticks, actual_freq; };

// Largest period_ticks (best duty resolution) for freq; group prescaler kept integer.
static PwmTiming bestTiming(uint32_t freq, uint32_t src_clk = 160'000'000) {
    uint32_t presc = 1;
    while (src_clk / presc / freq > 65535u) ++presc;     // 16-bit period limit
    uint32_t res   = src_clk / presc;
    uint32_t ticks = (res + freq / 2) / freq;            // rounded
    return { res, ticks, res / ticks };
}
```

At 39 kHz: ~4102 ticks (~12-bit) vs LEDC's 2048. Counts/frequency stay configurable —
pass explicit `period_ticks` to pin `pwmMax` to the old LEDC value (zero recalibration),
or call `bestTiming()` for max resolution on new boards. **Default = pin to LEDC-equivalent.**

## Leg class

```cpp
class MCPWM_SyncLeg {
    mcpwm_timer_handle_t timer{};
    mcpwm_oper_handle_t  oper{};
    mcpwm_cmpr_handle_t  cmpHS{}, cmpLS{};
    mcpwm_gen_handle_t   genHS{}, genLS{};
public:
    uint16_t pwmMax{};                       // = period_ticks (same role as LEDC)

    void init(int group, uint32_t freq, int pinHS, int pinLS,
              uint32_t dtTicks, bool enLogic, uint32_t fixedTicks = 0);

    // glitch-free: both comparators latch on next TEZ → atomic, no ordering dance
    inline void setHsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpHS, c); }
    inline void setLsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpLS, c); }

    void start();          // mcpwm_timer_start_stop(START_NO_STOP)
    void forceShutdown();  // SW one-shot brake → both gates low
    void recover();        // clear OST brake
};
```

Comparators are created with `flags.update_cmp_on_tez = true`. Setting `cmpHS` and
`cmpLS` in any order latches both together at the next period boundary — this removes
the `largerDecrease` / `direction<0` ordering gymnastics at `buck.h:313-350`.

Dead-time (HiLi): `mcpwm_generator_set_dead_time(genHS, genHS, {posedge_delay_ticks=dtTicks})`
and same on `genLS`. Delaying both rising edges by `dtTicks` guarantees ≥ dtTicks
dead-band at both transitions (HS→LS and the LS→HS wrap), regardless of `lsOff`.

## Fault GPIO — zero-CPU shutdown

```cpp
class MCPWM_FaultBrake {                      // one per group, shared by all legs
    mcpwm_fault_handle_t fault{};
public:
    void initGpio(int group, int pin, bool activeHigh);  // mcpwm_new_gpio_fault
    void bindLeg(mcpwm_oper_handle_t op, mcpwm_gen_handle_t hs, mcpwm_gen_handle_t ls);
        // mcpwm_operator_set_brake_on_fault(OST)
        // + mcpwm_generator_set_action_on_brake_event(force LOW) on hs & ls
};
```

OST (one-shot) brake latches both gates to the safe level the moment the pin asserts,
independent of `loopRT` — addresses the INA226-timeout sampler-starvation shutdown.
Recovery is explicit, so a fault cannot silently clear.

## Interleaving

N legs = N operators. Reuse the existing `PWMTimerSync` stub: one timer is the sync
source; each other leg's timer is phase-offset by `i * period_ticks / N` via
`mcpwm_timer_set_phase_on_sync`. Single-phase = N=1, not special-cased. A
`MCPWM_Converter` holds `std::array<MCPWM_SyncLeg, N>` + one `MCPWM_FaultBrake` and fans
`setHsOff/setLsOff` to all legs.

## buck.h adaptation

`pwmDriver` becomes a `MCPWM_SyncLeg` (or `MCPWM_Converter`). The two `update_pwm`
overloads collapse:

- `update_pwm(ch, duty)` → `setHsOff(duty)` (HS), or for the EN window → `setLsOff(duty)`
- `update_pwm(ch, hpoint, duty)` → `setLsOff(hpoint + duty)` (`hpoint == pwmCtrl`)

The `largerDecrease` branch and the direction-ordered writes at `buck.h:313-350` reduce
to: set both comparators. `disable()` → `forceShutdown()`. `pwmMax` semantics unchanged.

## Configuration (`board.conf`)

- `pwm_deadtime_ns` (new) — dead-time in ns; converted to ticks via `bestTiming`.
- existing: `pwm_freq`, `pwm_driver_logic`, `pwm_hi`/`pwm_li` or `pwm_in`/`pwm_en`, `pwm_sd`.
- new (optional): `pwm_fault_pin`, `pwm_fault_active_high`.

## Risks / open items

- `coil.conf::rect_offset` and `pwmRectMin` are in counts; identical only while
  `pwmMax` stays at the LEDC value. `bestTiming()` opt-in must rescale them per board.
- Confirm MCPWM update-on-TEZ latency vs the RT loop period at 39 kHz.
- `forceShutdown()` must be safe to call from the RT path (register write, no alloc).
