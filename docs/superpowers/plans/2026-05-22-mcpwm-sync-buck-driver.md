*this document is an LLM generated placeholder*

# MCPWM Synchronous-Buck Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an MCPWM-based gate driver for the synchronous buck/boost converter that preserves the existing software diode-emulation model, adds hardware dead-time and a zero-CPU GPIO fault brake, supports interleaved legs, and updates duty glitch-free.

**Architecture:** A new `src/pwm/mcpwm.h` replaces the legacy stub. One MCPWM operator = one synchronous leg (HS+LS generator pair, two comparators, per-generator dead-time). A group-level GPIO fault drives an OST brake. `bestTiming()` (pure, host-testable) picks `period_ticks`. `buck.h` selects LEDC vs MCPWM at compile time via `WITH_MCPWM`; the MCPWM path collapses the LEDC ordering dance into two unordered comparator writes. Default `period_ticks` is pinned to the LEDC-equivalent so calibration is unchanged.

**Tech Stack:** ESP-IDF 5.5 `driver/mcpwm_prelude.h`, ESP32-S3, C++ (gnu++17), Unity on-target tests, host g++ for the pure timing function.

**Spec:** `doc/mcpwm-sync-buck-driver.md`

---

### Task 1: `bestTiming()` pure timing function (host-TDD)

Pure arithmetic, no ESP-IDF dependency, so it lives in its own header and gets a real host unit test.

**Files:**
- Create: `src/pwm/mcpwm_timing.h`
- Create: `test/host-stub/mcpwm-timing-test.cpp`

- [ ] **Step 1: Write the failing test**

`test/host-stub/mcpwm-timing-test.cpp`:
```cpp
#include <cassert>
#include <cstdio>
#include "../../src/pwm/mcpwm_timing.h"

int main() {
    // 39 kHz off 160 MHz: prescaler 1, ~4102 ticks, ~12-bit, freq within 0.1%
    auto t = bestTiming(39000);
    assert(t.resolution_hz == 160000000u);
    assert(t.period_ticks == 4103u);            // round(160e6/39000)
    assert(t.actual_freq > 38900 && t.actual_freq < 39100);

    // 5 kHz still fits 16-bit at prescaler 1 (32000 ticks)
    auto lo = bestTiming(5000);
    assert(lo.period_ticks == 32000u && lo.resolution_hz == 160000000u);

    // 2 kHz would need 80000 ticks > 65535 → prescaler bumps to 2
    auto vlo = bestTiming(2000);
    assert(vlo.period_ticks <= 65535u);
    assert(vlo.resolution_hz == 80000000u);

    printf("mcpwm-timing-test OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `c++ -std=gnu++17 test/host-stub/mcpwm-timing-test.cpp -o /tmp/mcpwm-timing-test && /tmp/mcpwm-timing-test`
Expected: FAIL — `fatal error: '../../src/pwm/mcpwm_timing.h' file not found`

- [ ] **Step 3: Write minimal implementation**

`src/pwm/mcpwm_timing.h`:
```cpp
#pragma once
#include <cstdint>

struct PwmTiming {
    uint32_t resolution_hz;
    uint32_t period_ticks;
    uint32_t actual_freq;
};

// Largest period_ticks (best duty resolution) for `freq`, keeping the group
// prescaler an integer divide of `src_clk` and period within 16 bits.
static inline PwmTiming bestTiming(uint32_t freq, uint32_t src_clk = 160000000u) {
    uint32_t presc = 1;
    while (src_clk / presc / freq > 65535u) ++presc;
    uint32_t res   = src_clk / presc;
    uint32_t ticks = (res + freq / 2) / freq;       // rounded
    return PwmTiming{res, ticks, res / ticks};
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `c++ -std=gnu++17 test/host-stub/mcpwm-timing-test.cpp -o /tmp/mcpwm-timing-test && /tmp/mcpwm-timing-test`
Expected: PASS — prints `mcpwm-timing-test OK`

- [ ] **Step 5: Commit**

```bash
git add src/pwm/mcpwm_timing.h test/host-stub/mcpwm-timing-test.cpp
git commit -m "feat(mcpwm): bestTiming() period-ticks resolution picker"
```

---

### Task 2: `MCPWM_SyncLeg` core — timer, operator, comparators, generators

Replaces the stub in `src/pwm/mcpwm.h`. Edge-aligned count-up, HS rises at TEZ, comparators latch on TEZ (glitch-free).

**Files:**
- Modify (replace contents): `src/pwm/mcpwm.h`
- Modify: `src/buck.h:7` (remove legacy `#include "driver/mcpwm.h"` and the inline `PWM_MCPWM`/`PWMTimerSync` block at `src/buck.h:1-82`; these move/retire)

- [ ] **Step 1: Write the leg core**

`src/pwm/mcpwm.h` (replace whole file):
```cpp
#pragma once

#include "driver/mcpwm_prelude.h"
#include "esp_err.h"
#include "mcpwm_timing.h"

// One synchronous leg = one MCPWM operator. HS on [0, hsOff], LS on [hsOff, lsOff].
// enLogic=true → LS pin is the EN window [0, lsOff] (driver chip inserts dead-time).
class MCPWM_SyncLeg {
    mcpwm_timer_handle_t timer_ = nullptr;
    mcpwm_oper_handle_t  oper_  = nullptr;
    mcpwm_cmpr_handle_t  cmpHS_ = nullptr, cmpLS_ = nullptr;
    mcpwm_gen_handle_t   genHS_ = nullptr, genLS_ = nullptr;

public:
    uint16_t pwmMax = 0;   // = period_ticks (same role as LEDC pwmMax)

    mcpwm_oper_handle_t oper() const { return oper_; }
    mcpwm_gen_handle_t  genHS() const { return genHS_; }
    mcpwm_gen_handle_t  genLS() const { return genLS_; }
    mcpwm_timer_handle_t timer() const { return timer_; }

    // fixedTicks>0 pins period_ticks (LEDC-equivalent); 0 → bestTiming(freq).
    void init(int group, uint32_t freq, int pinHS, int pinLS,
              uint32_t dtTicks, bool enLogic, uint32_t fixedTicks = 0) {
        PwmTiming t = fixedTicks ? PwmTiming{freq * fixedTicks, fixedTicks, freq}
                                 : bestTiming(freq);
        pwmMax = (uint16_t) t.period_ticks;

        mcpwm_timer_config_t tc = {
            .group_id = group,
            .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
            .resolution_hz = t.resolution_hz,
            .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
            .period_ticks = t.period_ticks,
            .flags = {},
        };
        ESP_ERROR_CHECK(mcpwm_new_timer(&tc, &timer_));

        mcpwm_operator_config_t oc = {.group_id = group, .flags = {}};
        ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &oper_));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper_, timer_));

        mcpwm_comparator_config_t cc = {.flags = {.update_cmp_on_tez = true}};
        ESP_ERROR_CHECK(mcpwm_new_comparator(oper_, &cc, &cmpHS_));
        ESP_ERROR_CHECK(mcpwm_new_comparator(oper_, &cc, &cmpLS_));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpHS_, 0));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpLS_, 0));

        mcpwm_generator_config_t gHS = {.gen_gpio_num = pinHS, .flags = {}};
        mcpwm_generator_config_t gLS = {.gen_gpio_num = pinLS, .flags = {}};
        ESP_ERROR_CHECK(mcpwm_new_generator(oper_, &gHS, &genHS_));
        ESP_ERROR_CHECK(mcpwm_new_generator(oper_, &gLS, &genLS_));

        // HS: HIGH at TEZ, LOW at cmpHS  ->  on [0, hsOff]
        ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_timer_event(genHS_,
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH),
            MCPWM_GEN_TIMER_EVENT_ACTION_END()));
        ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genHS_,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpHS_, MCPWM_GEN_ACTION_LOW),
            MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

        // LS: HIGH at (enLogic ? TEZ : cmpHS), LOW at cmpLS -> on [hsOff|0, lsOff]
        if (enLogic) {
            ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_timer_event(genLS_,
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH),
                MCPWM_GEN_TIMER_EVENT_ACTION_END()));
        } else {
            ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genLS_,
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpHS_, MCPWM_GEN_ACTION_HIGH),
                MCPWM_GEN_COMPARE_EVENT_ACTION_END()));
        }
        ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(genLS_,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpLS_, MCPWM_GEN_ACTION_LOW),
            MCPWM_GEN_COMPARE_EVENT_ACTION_END()));

        (void) dtTicks; // dead-time wired in Task 3
    }

    inline void setHsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpHS_, c); }
    inline void setLsOff(uint16_t c) { mcpwm_comparator_set_compare_value(cmpLS_, c); }

    void start() {
        ESP_ERROR_CHECK(mcpwm_timer_enable(timer_));
        ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer_, MCPWM_TIMER_START_NO_STOP));
    }
};
```

- [ ] **Step 2: Remove the legacy MCPWM block from buck.h**

In `src/buck.h`: delete `#include "driver/mcpwm.h"` (line 7) and the entire `class PWM_MCPWM { ... };` plus `struct PWMTimerSync { ... };` block (lines ~14-82). Leave the `#include "pwm/ledc.h"` at line 13 untouched for now.

- [ ] **Step 3: Build to verify the leg compiles on-target**

Run: `. ./idf-export.sh && MAIN_SRC=../test/test_buck.cpp idf.py build 2>&1 | tail -20`
Expected: build succeeds (mcpwm.h not yet referenced by buck.h, but must compile when included — verify by temporarily adding `#include "pwm/mcpwm.h"` to `test/test_buck.cpp` top, building, then removing it).

- [ ] **Step 4: Commit**

```bash
git add src/pwm/mcpwm.h src/buck.h
git commit -m "feat(mcpwm): MCPWM_SyncLeg core (timer/oper/cmp/gen), drop legacy stub"
```

---

### Task 3: Hardware dead-time

Delay both generators' rising edges by `dtTicks` → guaranteed dead-band at both transitions (HiLi only; `dtTicks==0` is a no-op for InEn).

**Files:**
- Modify: `src/pwm/mcpwm.h` (the `(void) dtTicks;` line in `init`)

- [ ] **Step 1: Replace the dead-time placeholder**

In `MCPWM_SyncLeg::init`, replace `(void) dtTicks; // dead-time wired in Task 3` with:
```cpp
        if (dtTicks) {
            mcpwm_dead_time_config_t dt = {
                .posedge_delay_ticks = dtTicks,
                .negedge_delay_ticks = 0,
                .flags = {},
            };
            ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(genHS_, genHS_, &dt));
            ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(genLS_, genLS_, &dt));
        }
```

- [ ] **Step 2: Build to verify**

Run: `. ./idf-export.sh && MAIN_SRC=../test/test_buck.cpp idf.py build 2>&1 | tail -10` (with the temporary `#include "pwm/mcpwm.h"` from Task 2 Step 3 re-added)
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/pwm/mcpwm.h
git commit -m "feat(mcpwm): configurable hardware dead-time per generator"
```

---

### Task 4: GPIO fault brake (zero-CPU shutdown)

Group-level GPIO fault → OST brake forces both gates to the safe level in hardware.

**Files:**
- Modify: `src/pwm/mcpwm.h` (add `MCPWM_FaultBrake` class)

- [ ] **Step 1: Add the fault-brake class**

Append to `src/pwm/mcpwm.h` (before the final newline):
```cpp
// One GPIO fault per group, shared by all legs. OST brake latches gates LOW in HW.
class MCPWM_FaultBrake {
    mcpwm_fault_handle_t fault_ = nullptr;

public:
    void initGpio(int group, int pin, bool activeHigh) {
        mcpwm_gpio_fault_config_t fc = {
            .group_id = group,
            .gpio_num = pin,
            .flags = {
                .active_level = (uint32_t) (activeHigh ? 1 : 0),
                .pull_up = (uint32_t) (activeHigh ? 0 : 1),
                .pull_down = (uint32_t) (activeHigh ? 1 : 0),
            },
        };
        ESP_ERROR_CHECK(mcpwm_new_gpio_fault(&fc, &fault_));
    }

    // Bind a leg's operator+generators: OST brake, force both gens LOW on fault.
    void bindLeg(mcpwm_oper_handle_t op, mcpwm_gen_handle_t hs, mcpwm_gen_handle_t ls) {
        mcpwm_brake_config_t bc = {
            .fault = fault_,
            .brake_mode = MCPWM_OPER_BRAKE_MODE_OST,
            .flags = {.cbc_recover_on_tez = 0, .cbc_recover_on_tep = 0},
        };
        ESP_ERROR_CHECK(mcpwm_operator_set_brake_on_fault(op, &bc));
        ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_brake_event(hs,
            MCPWM_GEN_BRAKE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_OPER_BRAKE_MODE_OST, MCPWM_GEN_ACTION_LOW),
            MCPWM_GEN_BRAKE_EVENT_ACTION_END()));
        ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_brake_event(ls,
            MCPWM_GEN_BRAKE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_OPER_BRAKE_MODE_OST, MCPWM_GEN_ACTION_LOW),
            MCPWM_GEN_BRAKE_EVENT_ACTION_END()));
    }

    mcpwm_fault_handle_t handle() const { return fault_; }
};
```

- [ ] **Step 2: Add explicit recovery to MCPWM_SyncLeg**

Add to `MCPWM_SyncLeg` (public), so a latched OST brake can be cleared deliberately:
```cpp
    void recoverFault() { ESP_ERROR_CHECK(mcpwm_operator_recover_from_fault(oper_)); }
```

- [ ] **Step 3: Build to verify**

Run: `. ./idf-export.sh && MAIN_SRC=../test/test_buck.cpp idf.py build 2>&1 | tail -10`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/pwm/mcpwm.h
git commit -m "feat(mcpwm): GPIO fault OST brake, explicit recovery"
```

---

### Task 5: Software shutdown / enable (force-level)

`disable()` must force both gates low immediately without relying on the timer; `recover()` removes the force.

**Files:**
- Modify: `src/pwm/mcpwm.h` (`MCPWM_SyncLeg`)

- [ ] **Step 1: Add force-level shutdown**

Add to `MCPWM_SyncLeg` (public):
```cpp
    // Immediate, register-only: force both gates LOW (safe), hold until cleared.
    void forceShutdown() {
        mcpwm_generator_set_force_level(genHS_, 0, true);
        mcpwm_generator_set_force_level(genLS_, 0, true);
    }
    // Remove the forced level, returning control to the timer/comparator actions.
    void clearForce() {
        mcpwm_generator_set_force_level(genHS_, -1, true);
        mcpwm_generator_set_force_level(genLS_, -1, true);
    }
```

- [ ] **Step 2: Build to verify**

Run: `. ./idf-export.sh && MAIN_SRC=../test/test_buck.cpp idf.py build 2>&1 | tail -10`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/pwm/mcpwm.h
git commit -m "feat(mcpwm): force-level software shutdown/clear"
```

---

### Task 6: `board.conf` keys + docs (three-source rule)

New keys: `pwm_deadtime_ns`, `pwm_fault_pin`, `pwm_fault_active_high`. CLAUDE.md requires updating the reference table AND the editor metadata together.

**Files:**
- Modify: `doc/Configuration.md` (board.conf table)
- Modify: `etc/config-tool/conf-editor.html` (`META` + `FILE_KEYS`)
- Modify: `config/fmetal/conf/board.conf` and `config/lab/dry_mock/conf/board.conf` (document defaults as comments; values optional)

- [ ] **Step 1: Add the keys to the board.conf reference table**

In `doc/Configuration.md`, in the `board.conf` section, add rows:
```
| pwm_deadtime_ns | HiLi dead-time in ns (HS/LS gate insertion); 0 = none (default) | 0 |
| pwm_fault_pin | GPIO for hardware OST brake (255 = disabled) | 255 |
| pwm_fault_active_high | 1 if fault asserts high, 0 if low | 0 |
```

- [ ] **Step 2: Add the same keys to the editor metadata**

In `etc/config-tool/conf-editor.html`, add to `META` (descriptions/types) and `FILE_KEYS` for `board.conf`:
```js
// META additions:
pwm_deadtime_ns:       {t:'int', d:'HiLi dead-time (ns), 0=none'},
pwm_fault_pin:         {t:'int', d:'GPIO for HW OST brake, 255=off'},
pwm_fault_active_high: {t:'bool', d:'fault asserts high'},
// FILE_KEYS['board.conf'] additions: 'pwm_deadtime_ns','pwm_fault_pin','pwm_fault_active_high'
```

- [ ] **Step 3: Commit**

```bash
git add doc/Configuration.md etc/config-tool/conf-editor.html config/fmetal/conf/board.conf config/lab/dry_mock/conf/board.conf
git commit -m "doc(conf): board.conf pwm_deadtime_ns/pwm_fault_pin keys"
```

---

### Task 7: Integrate MCPWM into buck.h behind `WITH_MCPWM`

Select driver at compile time; map the two `update_pwm` overloads onto `setHsOff`/`setLsOff`; collapse the ordering dance.

**Files:**
- Modify: `src/buck.h` (driver typedef, `init`, the update block at ~`buck.h:313-350`, `disable`, `setManualRect` at `buck.h:148-149`)

- [ ] **Step 1: Select the driver type**

Near the top of `src/buck.h`, replace the `#include "pwm/ledc.h"` line with:
```cpp
#if WITH_MCPWM
#include "pwm/mcpwm.h"
using PwmDriver = MCPWM_SyncLeg;
#else
#include "pwm/ledc.h"
using PwmDriver = PWM_ESP32_ledc;
#endif
```
Change the member declaration `PWM_ESP32_ledc pwmDriver;` (`buck.h:60`) to `PwmDriver pwmDriver;`.

- [ ] **Step 2: Add driver-agnostic update helpers**

Add private helpers to `SynchronousConverter` so call sites are identical for both drivers:
```cpp
#if WITH_MCPWM
    void drvHsOff(uint16_t hsOff) { pwmDriver.setHsOff(hsOff); }
    void drvLsOff(uint16_t lsOff) { pwmDriver.setLsOff(lsOff); }
#else
    void drvHsOff(uint16_t hsOff) { pwmDriver.update_pwm(pwmCh_Ctrl, hsOff); }
    void drvLsOff(uint16_t lsOff) {
        if (pwmEnLogic) pwmDriver.update_pwm(pwmCh_Rect, lsOff);
        else            pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl, lsOff - pwmCtrl);
    }
#endif
```

- [ ] **Step 3: Rewrite the update block to use the helpers**

Replace the `largerDecrease` / `direction<0` / else block at `src/buck.h:313-350` with:
```cpp
#if WITH_MCPWM
        // Comparators latch together on TEZ — order-independent, glitch-free.
        drvHsOff(pwmCtrl);
        drvLsOff(pwmCtrl + pwmRect);
#else
        if (largerDecrease) {
            if (pwmEnLogic) { pwmDriver.update_pwm(pwmCh_Rect, 0); drvHsOff(pwmCtrl); drvLsOff(pwmCtrl + pwmRect); }
            else { if (pinSd != 255) digitalWrite(pinSd, 1); else pwmDriver.update_pwm(pwmCh_Rect, 0);
                   drvHsOff(pwmCtrl); drvLsOff(pwmCtrl + pwmRect); if (pinSd != 255) digitalWrite(pinSd, 0); }
        } else if (direction < 0) { drvLsOff(pwmCtrl + pwmRect); drvHsOff(pwmCtrl); }
        else { drvHsOff(pwmCtrl); drvLsOff(pwmCtrl + pwmRect); }
#endif
```

- [ ] **Step 4: Update `setManualRect` and `disable`**

`setManualRect` (`buck.h:148-149`): replace the `if (pwmEnLogic) ... else ...` pair with `drvLsOff(pwmCtrl + pwmRect);`.

`disable()` (`buck.h:377-383`): wrap the stop calls:
```cpp
#if WITH_MCPWM
        pwmDriver.forceShutdown();
#else
        pwmDriver.stop(pwmCh_Ctrl, 0);
        pwmDriver.stop(pwmCh_Rect, 0);
#endif
```

- [ ] **Step 5: Wire init (driver-specific signature)**

In `init` (`buck.h:216-217`), replace the two `pwmDriver.init_pwm(...)` calls with:
```cpp
#if WITH_MCPWM
        auto dtNs = boardConf.getFloat("pwm_deadtime_ns", 0.f);
        uint32_t dtTicks = (uint32_t) (dtNs * 1e-9f * (float) (pwmFrequency) * 0); // placeholder
        // dead-time ticks are resolution-relative: dtNs * resolution_hz; computed post-init below
        pwmDriver.init(0, pwmFrequency, pinCtrl, pinRect, 0, pwmEnLogic, /*fixedTicks=*/2048);
        // recompute dead-time now that resolution is known, then re-init dt:
        // (kept simple: pass dtTicks via a second init arg in Step 6)
        (void) dtTicks;
        uint8_t faultPin = boardConf.getByte("pwm_fault_pin", 255);
        if (faultPin != 255) {
            faultBrake.initGpio(0, faultPin, boardConf.getByte("pwm_fault_active_high", 0));
            faultBrake.bindLeg(pwmDriver.oper(), pwmDriver.genHS(), pwmDriver.genLS());
        }
        pwmDriver.start();
#else
        pwmDriver.init_pwm(pwmCh_Ctrl, pinCtrl, pwmFrequency);
        pwmDriver.init_pwm(pwmCh_Rect, pinRect, pwmFrequency);
#endif
```
Add a member (guarded): `#if WITH_MCPWM\n    MCPWM_FaultBrake faultBrake;\n#endif` near `buck.h:60`.

Note: `fixedTicks=2048` pins `pwmMax` to the LEDC-equivalent (default per spec §2). Dead-time ticks = `pwm_deadtime_ns * 1e-9 * resolution_hz`; since `resolution_hz = pwmFrequency * pwmMax`, `dtTicks = pwm_deadtime_ns * 1e-9 * pwmFrequency * pwmMax`. Pass that as the 5th `init` arg (replace the `0`).

- [ ] **Step 6: Set dead-time ticks correctly**

Replace the placeholder dtTicks math and the `init(..., 0, ...)` call in Step 5 with:
```cpp
        uint32_t dtTicks = (uint32_t) std::lround(
            boardConf.getFloat("pwm_deadtime_ns", 0.f) * 1e-9f
            * (float) pwmFrequency * 2048.f);   // 2048 = fixedTicks (pwmMax)
        pwmDriver.init(0, pwmFrequency, pinCtrl, pinRect, dtTicks, pwmEnLogic, 2048);
```
(Delete the earlier two-step init/placeholder — this single call replaces it.)

- [ ] **Step 7: Build both variants**

Run:
```bash
. ./idf-export.sh
MAIN_SRC=../test/test_buck.cpp idf.py build 2>&1 | tail -5            # LEDC path
WITH_MCPWM=1 idf.py build 2>&1 | tail -5                              # MCPWM path
```
Expected: both succeed. (Add `WITH_MCPWM` to the build defs the same way `WITH_BLE`/`WITH_BINARY_TELE` are wired in `CMakeLists.txt`/`main/CMakeLists.txt` — mirror that pattern; if absent, add `add_compile_definitions(WITH_MCPWM=$ENV{WITH_MCPWM})` guarded by `if(DEFINED ENV{WITH_MCPWM})`.)

- [ ] **Step 8: On-target gate verification (manual, real hardware)**

On a **bench device only** (e.g. `139C` mock-ADC board — never fry/flat unprovisioned): flash `WITH_MCPWM=1`, set a low manual duty (`dc 50`), and confirm with a scope or `scope` TCP stream that HS and LS are complementary with the configured dead-time and that `pwm_freq` matches. Then assert the fault GPIO and confirm both gates go low with the CPU halted (breakpoint) — proving zero-CPU brake.
Expected: complementary gates, correct fsw, dead-band present, fault forces both low.

- [ ] **Step 9: Commit**

```bash
git add src/buck.h CMakeLists.txt main/CMakeLists.txt
git commit -m "feat(buck): WITH_MCPWM driver path, glitch-free comparator updates"
```

---

### Task 8: `MCPWM_Converter<N>` interleaving wrapper

N phase-shifted legs sharing one fault. N=1 today; not special-cased.

**Files:**
- Modify: `src/pwm/mcpwm.h` (add `MCPWM_Converter`)

- [ ] **Step 1: Add the interleaving wrapper**

Append to `src/pwm/mcpwm.h`:
```cpp
#include <array>

// N interleaved synchronous legs, timers phase-shifted by period/N, one shared fault.
template <int N>
class MCPWM_Converter {
    std::array<MCPWM_SyncLeg, N> legs_;
    MCPWM_FaultBrake fault_;
    mcpwm_sync_handle_t sync_ = nullptr;

public:
    uint16_t pwmMax = 0;

    void init(int group, uint32_t freq, const int (&pinHS)[N], const int (&pinLS)[N],
              uint32_t dtTicks, bool enLogic, uint32_t fixedTicks,
              int faultPin = -1, bool faultActiveHigh = false) {
        for (int i = 0; i < N; ++i)
            legs_[i].init(group, freq, pinHS[i], pinLS[i], dtTicks, enLogic, fixedTicks);
        pwmMax = legs_[0].pwmMax;

        if (faultPin >= 0) {
            fault_.initGpio(group, faultPin, faultActiveHigh);
            for (auto &l : legs_) fault_.bindLeg(l.oper(), l.genHS(), l.genLS());
        }

        if (N > 1) {                       // phase-shift legs 1..N-1 off leg 0
            mcpwm_timer_sync_src_config_t sc = {.timer_event = MCPWM_TIMER_EVENT_EMPTY, .flags = {}};
            ESP_ERROR_CHECK(mcpwm_new_timer_sync_src(legs_[0].timer(), &sc, &sync_));
            for (int i = 1; i < N; ++i) {
                mcpwm_timer_sync_phase_config_t pc = {
                    .sync_src = sync_,
                    .count_value = (uint32_t) ((uint32_t) pwmMax * i / N),
                    .direction = MCPWM_TIMER_DIRECTION_UP,
                };
                ESP_ERROR_CHECK(mcpwm_timer_set_phase_on_sync(legs_[i].timer(), &pc));
            }
        }
        for (auto &l : legs_) l.start();
    }

    void setHsOff(uint16_t c) { for (auto &l : legs_) l.setHsOff(c); }
    void setLsOff(uint16_t c) { for (auto &l : legs_) l.setLsOff(c); }
    void forceShutdown()      { for (auto &l : legs_) l.forceShutdown(); }
    void clearForce()         { for (auto &l : legs_) l.clearForce(); }
};
```

- [ ] **Step 2: Build to verify the template instantiates**

Run: `. ./idf-export.sh && WITH_MCPWM=1 idf.py build 2>&1 | tail -10`
Expected: build succeeds (the wrapper is header-only; instantiate `MCPWM_Converter<1>` in a temporary `static_assert`/local in `test/test_buck.cpp` to force compilation, then remove).

- [ ] **Step 3: Commit**

```bash
git add src/pwm/mcpwm.h
git commit -m "feat(mcpwm): MCPWM_Converter<N> interleaved legs, shared fault"
```

---

## Self-Review Notes

- **Spec coverage:** configurable freq/counts (Task 1 `bestTiming` + `fixedTicks`, Task 7 Step 5); best-resolution function (Task 1); dead-time (Task 3, config Task 6, wired Task 7 Step 6); GPIO zero-CPU brake (Task 4, wired Task 7 Step 5, verified Step 8); interleaving (Task 8); glitch-free updates (Task 2 `update_cmp_on_tez`, Task 7 Step 3). All covered.
- **Types consistent:** `PwmTiming{resolution_hz,period_ticks,actual_freq}`, `bestTiming(freq,src_clk)`, `MCPWM_SyncLeg::{init,setHsOff,setLsOff,start,recoverFault,forceShutdown,clearForce,oper,genHS,genLS,timer,pwmMax}`, `MCPWM_FaultBrake::{initGpio,bindLeg,handle}`, `MCPWM_Converter<N>` used uniformly.
- **Hardware caveat:** register-config tasks (2-5,7-8) cannot be host-unit-tested; verification is on-target build + scope (Task 7 Step 8), this project's canonical path. Only `bestTiming` is host-TDD'd.
- **Calibration:** `fixedTicks=2048` keeps `rect_offset`/`pwmRectMin` bit-identical to LEDC (spec risk note); `bestTiming()` opt-in requires rescaling those — out of scope here, flagged for a follow-up.
