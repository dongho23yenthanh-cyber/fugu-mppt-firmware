*this document is an LLM generated placeholder*

# VirtualConverter (vconv) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Embed a pure-software model of a synchronous buck converter in firmware so the existing control code (`SynchronousConverter`, MPPT, PD loops, charger) runs on real ESP32 hardware without a physical power stage. Replaces `ADC_Fake` in mock board configs.

**Architecture:** Three units. `VirtualConverter` (`src/sim/vconv.*`) holds state and solves coil current per cycle in closed form. `PWM_VConv` (`src/pwm/vconv.h`) is a `PwmDriver` impl that forwards `update_pwm` events into the singleton `g_vconv`. `ADC_VConv` (`src/adc/vconv.h`) is an `AsyncADC<float>` backend that steps the model on its sample timer and returns V_in / I_in / I_out / V_out / NTC. Build-time `WITH_VCONV=1` swaps the PWM driver; runtime `*_adc=vconv` in `sensor.conf` selects the ADC backend.

**Tech Stack:** C++20, ESP-IDF v5.5, FreeRTOS, the existing `AsyncADC<float>` / `PwmDriver` abstractions. Host-stub tests with `clang++ -std=gnu++17` (existing pattern, see `test/host-stub/integrator-test.cpp`).

**Design spec:** [`docs/superpowers/specs/2026-05-23-virtual-converter-design.md`](../specs/2026-05-23-virtual-converter-design.md)

---

## Conventions for all tasks

- After every passing test, run a full host-stub rebuild before committing to make sure no other host-stub test broke.
- All new `.md` / docs files start with `*this document is an LLM generated placeholder*` per `CLAUDE.md`.
- Commit messages: short, lower-case, present tense, with `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`. See recent commits for tone.
- Stage only files relevant to the task — leave `sdkconfig`, `etc/fugu` submodule pointer, and other dirty files alone.
- Host-stub test command for vconv-test (used in many tasks):
  ```bash
  clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
      -I test/host-stub -I src \
      -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
      && /tmp/vconv-test
  ```
  Expected: exits 0, prints `vconv-test: all asserts passed`.
- `pwmCtrl` / `pwmRect` / `pwmMax` / `pwmFreq` semantics mirror `src/pwm/mock.h` + the buck.h selector — see spec §"PWM input".

---

## Task 1: Skeleton — `VirtualConverter` class, PV exponential model

**Files:**
- Create: `src/sim/vconv.h`
- Create: `src/sim/vconv.cpp`
- Create: `test/host-stub/vconv-test.cpp`

- [ ] **Step 1: Write the failing test for `pvCurrent()` IV-curve shape**

Create `test/host-stub/vconv-test.cpp` with this content:

```cpp
// Host-side unit test for VirtualConverter. Run with the clang++ command
// in docs/superpowers/plans/2026-05-23-virtual-converter.md (top of file).
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../../src/sim/vconv.h"

static bool approxEq(float a, float b, float rtol = 1e-3f, float atol = 1e-6f) {
    return std::abs(a - b) <= atol + rtol * std::abs(b);
}

static void test_pv_iv_curve() {
    VirtualConverter vc;
    vc.setPv(/*isc=*/8.0f, /*voc=*/40.0f, /*k=*/2.0f);

    assert(approxEq(vc.pvCurrent(0.0f), 8.0f, /*rtol*/0.01f));
    assert(vc.pvCurrent(40.0f) <= 0.001f);
    assert(vc.pvCurrent(50.0f) == 0.0f);            // clamp above voc
    assert(vc.pvCurrent(-1.0f) >= 0.0f && vc.pvCurrent(-1.0f) <= 8.0f);

    float prev = vc.pvCurrent(0.0f);
    for (float v = 1.0f; v <= 39.0f; v += 1.0f) {
        float i = vc.pvCurrent(v);
        assert(i <= prev + 1e-4f);                  // monotone non-increasing
        prev = i;
    }
}

int main() {
    test_pv_iv_curve();
    std::printf("vconv-test: all asserts passed\n");
    return 0;
}
```

- [ ] **Step 2: Run the test, verify it fails (no source files yet)**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp \
    && /tmp/vconv-test
```
Expected: compile error like `src/sim/vconv.h: No such file or directory`.

- [ ] **Step 3: Create `src/sim/vconv.h`**

```cpp
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

// Pure-C++ model of a synchronous buck converter. No Arduino, no IDF, no FreeRTOS.
// Wire-up (PWM input, ADC output, conf parsing) lives in src/pwm/vconv.h,
// src/adc/vconv.h, and src/main.cpp. Spec:
// docs/superpowers/specs/2026-05-23-virtual-converter-design.md
class VirtualConverter {
public:
    struct PwmState {
        uint16_t pwmMax = 0;
        uint16_t pwmCtrl = 0;   // HS on-count (buck)
        uint16_t pwmRect = 0;   // LS on-count (buck)
        uint32_t pwmFreq = 0;   // Hz
    };

    void setPv(float isc, float voc, float k) {
        isc_ = isc; voc_ = voc; pvK_ = k;
    }

    // Single-diode-ish exponential. Clamped to [0, isc].
    [[nodiscard]] float pvCurrent(float v) const {
        if (v >= voc_) return 0.0f;
        float i = isc_ * (1.0f - std::exp((v - voc_) / pvK_));
        if (i < 0.0f) i = 0.0f;
        if (i > isc_) i = isc_;
        return i;
    }

private:
    // PV
    float isc_ = 8.0f;
    float voc_ = 40.0f;
    float pvK_ = 2.0f;
};
```

- [ ] **Step 4: Create `src/sim/vconv.cpp`**

```cpp
#include "vconv.h"

// Out-of-line definitions land here as the class grows. Keep empty for v1
// (everything inline in the header so far).
```

- [ ] **Step 5: Run the test, verify it passes**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
    && /tmp/vconv-test
```
Expected: `vconv-test: all asserts passed` and exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/sim/vconv.h src/sim/vconv.cpp test/host-stub/vconv-test.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): VirtualConverter skeleton with PV exponential model

Closed-form IV curve I(V) = Isc * (1 - exp((V - Voc)/k)) clamped to
[0, Isc]. Monotone non-increasing in V, zero above Voc. Tested via the
host-stub vconv-test.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Battery sink + cap dynamics (no PWM yet)

**Files:**
- Modify: `src/sim/vconv.h`
- Modify: `src/sim/vconv.cpp`
- Modify: `test/host-stub/vconv-test.cpp`

- [ ] **Step 1: Append the failing test**

In `test/host-stub/vconv-test.cpp`, add this function before `main()`:

```cpp
static void test_battery_cap_dynamics() {
    VirtualConverter vc;
    vc.setPv(8.0f, 40.0f, 2.0f);
    vc.setBat(/*vbat=*/28.0f, /*rbat=*/0.05f);
    vc.setPassives(/*c_in=*/470e-6f, /*c_out=*/470e-6f, /*L=*/50e-6f);

    // No PWM applied yet -> caps integrate net source/sink current.
    vc.setVin(20.0f);
    vc.setVout(28.0f);
    // step 1 ms = ~39 PWM cycles at 39 kHz. Without PWM events, Iin/Iout=0,
    // so Vin should rise toward Voc (PV charges Cin), Vout should stay ~Vbat.
    const float vin0 = vc.getVin();
    const float vout0 = vc.getVout();
    vc.stepSeconds(/*dt=*/1e-3f, /*pwmFreqFallback=*/39000);

    assert(vc.getVin() > vin0);
    assert(vc.getVin() <= 40.5f);
    assert(approxEq(vc.getVout(), vout0, 0.05f));
}
```

Add `test_battery_cap_dynamics();` to `main()` above the success print.

- [ ] **Step 2: Run the test, verify it fails to compile**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
    && /tmp/vconv-test
```
Expected: compile errors like `'setBat' is not a member of 'VirtualConverter'`.

- [ ] **Step 3: Extend `src/sim/vconv.h`**

Add public methods after `pvCurrent()`:

```cpp
    void setBat(float vbat, float rbat) { vbat_ = vbat; rbat_ = rbat; }
    void setPassives(float c_in, float c_out, float l) { cIn_ = c_in; cOut_ = c_out; l_ = l; }
    void setVin(float v) { vIn_ = v; }
    void setVout(float v) { vOut_ = v; }
    [[nodiscard]] float getVin() const  { return vIn_; }
    [[nodiscard]] float getVout() const { return vOut_; }
    [[nodiscard]] float getIL() const   { return iLEnd_; }

    // Advance the model by dt_s seconds. Without PwmState updates, uses last
    // latched pwm (zero by default -> converter idle, no I_L, I_in=I_out=0,
    // caps drift toward source/sink).
    void stepSeconds(float dt_s, uint32_t pwmFreqFallback);
```

Add private fields below the PV fields:

```cpp
    // Battery + passives
    float vbat_ = 28.0f;
    float rbat_ = 0.05f;
    float cIn_  = 470e-6f;
    float cOut_ = 470e-6f;
    float l_    = 50e-6f;

    // State
    float vIn_   = 0.0f;
    float vOut_  = 0.0f;
    float iLEnd_ = 0.0f;   // coil current at end of last PWM cycle
    PwmState pwm_{};
```

- [ ] **Step 4: Implement `stepSeconds` in `src/sim/vconv.cpp`**

```cpp
#include "vconv.h"

void VirtualConverter::stepSeconds(float dt_s, uint32_t pwmFreqFallback) {
    if (dt_s <= 0.0f) return;
    uint32_t freq = pwm_.pwmFreq ? pwm_.pwmFreq : pwmFreqFallback;
    if (freq < 1000) freq = 39000;   // sane default if all else fails
    const float T = 1.0f / static_cast<float>(freq);

    // Number of PWM cycles to simulate. Round to nearest, min 1.
    long n = std::lround(dt_s / T);
    if (n < 1) n = 1;

    for (long i = 0; i < n; ++i) {
        // No coil action until PWM is wired (Task 3). For now, Iin = Iout = 0.
        const float Iin = 0.0f;
        const float Iout = 0.0f;

        // Source/sink + cap dynamics (forward-Euler per PWM cycle).
        const float Ipv = pvCurrent(vIn_);
        // I_bat: backflow clamp only when converter disabled.
        float Ibat = (vOut_ - vbat_) / rbat_;
        if (pwm_.pwmCtrl == 0 && Ibat < 0.0f) Ibat = 0.0f;

        vIn_  += (Ipv  - Iin)  * T / cIn_;
        vOut_ += (Iout - Ibat) * T / cOut_;

        // Numerical guards (see spec §Numerical guards).
        if (vIn_ < 0.0f) vIn_ = 0.0f;
        if (vIn_ > voc_ * 1.05f) vIn_ = voc_ * 1.05f;
        if (vOut_ < 0.0f) vOut_ = 0.0f;
        if (vOut_ > vbat_ * 2.0f) vOut_ = vbat_ * 2.0f;
    }
}
```

- [ ] **Step 5: Run the test, verify it passes**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
    && /tmp/vconv-test
```
Expected: `vconv-test: all asserts passed`.

- [ ] **Step 6: Commit**

```bash
git add src/sim/vconv.h src/sim/vconv.cpp test/host-stub/vconv-test.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): battery sink + cap forward-Euler step

Cin/Cout integrate PV current vs (placeholder zero) coil current.
Backflow clamp engages only when pwmCtrl==0. Numerical guards on
Vin/Vout keep the exponential PV term from running away.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Per-cycle analytic coil solver (CCM)

**Files:**
- Modify: `src/sim/vconv.h`
- Modify: `src/sim/vconv.cpp`
- Modify: `test/host-stub/vconv-test.cpp`

- [ ] **Step 1: Append the failing test (CCM steady-state)**

In `test/host-stub/vconv-test.cpp`, add before `main()`:

```cpp
static void test_ccm_steady_state() {
    VirtualConverter vc;
    vc.setPv(8.0f, 60.0f, 2.0f);
    vc.setBat(28.0f, 0.05f);
    vc.setPassives(470e-6f, 470e-6f, 50e-6f);
    vc.setVin(60.0f);
    vc.setVout(28.0f);

    // Pin a duty that gives clearly-CCM operation: D = Vout/Vin ~= 0.467.
    // pwmMax=1023 (typical LEDC at ~39 kHz on ESP32). pwmCtrl=478, pwmRect=pwmMax-pwmCtrl-1.
    VirtualConverter::PwmState p{
        .pwmMax = 1023,
        .pwmCtrl = 478,
        .pwmRect = static_cast<uint16_t>(1023 - 478 - 1),
        .pwmFreq = 39000,
    };
    vc.setPwm(p);

    // Run 5 ms (~195 cycles) to let I_L settle. PV provides plenty of current.
    for (int i = 0; i < 200; ++i) vc.stepSeconds(1.0f / 39000.0f, 39000);

    // After settling, I_L_end should be positive (CCM, not zero).
    assert(vc.getIL() > 0.5f);

    // Iout average reported by the model.
    const float iout = vc.getIoutAvg();
    assert(iout > 1.0f && iout < 12.0f);

    // Vout should not have collapsed.
    assert(vc.getVout() > 20.0f && vc.getVout() < 35.0f);
}
```

Register `test_ccm_steady_state();` in `main()` before the success print.

- [ ] **Step 2: Verify the test fails to compile**

Run the host-stub command above. Expected: `'setPwm' is not a member of 'VirtualConverter'`.

- [ ] **Step 3: Extend `src/sim/vconv.h`**

Add new public methods (before the private section):

```cpp
    void setPwm(const PwmState &s) { pwm_ = s; }
    [[nodiscard]] float getIinAvg() const  { return iInAvg_; }
    [[nodiscard]] float getIoutAvg() const { return iOutAvg_; }
    [[nodiscard]] bool  inDcm() const      { return dcm_; }
```

Add new private fields next to the existing state:

```cpp
    float iInAvg_  = 0.0f;   // cycle-average input current
    float iOutAvg_ = 0.0f;   // cycle-average output current (Iout sensor)
    bool  dcm_     = false;
```

Add a `stepOneCycle` declaration in the private section:

```cpp
    void stepOneCycle(float T);
```

- [ ] **Step 4: Rewrite `stepSeconds` to call a per-cycle analytic solver**

Replace `src/sim/vconv.cpp` contents:

```cpp
#include "vconv.h"

namespace {
// Trapezoid area under a piecewise-linear segment from y0 to y1 over duration t.
inline float trapArea(float y0, float y1, float t) {
    return 0.5f * (y0 + y1) * t;
}
}

void VirtualConverter::stepOneCycle(float T) {
    const float L = l_;
    const float dHS = pwm_.pwmMax ? float(pwm_.pwmCtrl) / float(pwm_.pwmMax) : 0.0f;
    const float dLS = pwm_.pwmMax ? float(pwm_.pwmRect) / float(pwm_.pwmMax) : 0.0f;
    const float tHS = dHS * T;
    const float tLS = dLS * T;
    const float tOff = std::max(0.0f, T - tHS - tLS);

    // Phase 1: HS on.
    const float a = iLEnd_;
    const float b = (tHS > 0.0f && L > 0.0f)
                        ? a + (vIn_ - vOut_) / L * tHS
                        : a;
    const float areaHS = trapArea(a, b, tHS);

    // Phase 2: LS on. Allow signed I_L (reverse current in forced PWM).
    float c;
    float areaLS;
    if (tLS > 0.0f && L > 0.0f) {
        c = b - vOut_ / L * tLS;
        if (b > 0.0f && c < 0.0f) {
            // Natural zero-crossing inside phase 2 -> firmware/diode holds I_L=0.
            const float tZc = b * L / vOut_;
            areaLS = trapArea(b, 0.0f, tZc);
            c = 0.0f;
        } else {
            areaLS = trapArea(b, c, tLS);
        }
    } else {
        c = b;
        areaLS = 0.0f;
    }

    // Phase 3: both switches off, body diodes conduct toward zero.
    float cEnd = c;
    float areaOff = 0.0f;
    if (tOff > 0.0f && L > 0.0f) {
        if (c > 0.0f) {
            const float tToZero = c * L / vOut_;
            if (tToZero < tOff) {
                areaOff = trapArea(c, 0.0f, tToZero);
                cEnd = 0.0f;
            } else {
                const float cAtEnd = c - vOut_ / L * tOff;
                areaOff = trapArea(c, cAtEnd, tOff);
                cEnd = cAtEnd;
            }
        } else if (c < 0.0f) {
            const float tToZero = -c * L / vIn_;
            if (tToZero < tOff) {
                areaOff = trapArea(c, 0.0f, tToZero);
                cEnd = 0.0f;
            } else {
                const float cAtEnd = c + vIn_ / L * tOff;
                areaOff = trapArea(c, cAtEnd, tOff);
                cEnd = cAtEnd;
            }
        }
    }

    iInAvg_  = (T > 0.0f) ? (areaHS / T) : 0.0f;
    iOutAvg_ = (T > 0.0f) ? ((areaHS + areaLS + areaOff) / T) : 0.0f;
    iLEnd_ = cEnd;

    // Source/sink + cap dynamics.
    const float Ipv = pvCurrent(vIn_);
    float Ibat = (vOut_ - vbat_) / rbat_;
    if (pwm_.pwmCtrl == 0 && Ibat < 0.0f) Ibat = 0.0f;

    vIn_  += (Ipv      - iInAvg_)  * T / cIn_;
    vOut_ += (iOutAvg_ - Ibat)     * T / cOut_;

    // Numerical guards.
    if (vIn_ < 0.0f) vIn_ = 0.0f;
    if (vIn_ > voc_ * 1.05f) vIn_ = voc_ * 1.05f;
    if (vOut_ < 0.0f) vOut_ = 0.0f;
    if (vOut_ > vbat_ * 2.0f) vOut_ = vbat_ * 2.0f;
}

void VirtualConverter::stepSeconds(float dt_s, uint32_t pwmFreqFallback) {
    if (dt_s <= 0.0f) return;
    uint32_t freq = pwm_.pwmFreq ? pwm_.pwmFreq : pwmFreqFallback;
    if (freq < 1000) freq = 39000;
    const float T = 1.0f / static_cast<float>(freq);

    long n = std::lround(dt_s / T);
    if (n < 1) n = 1;
    for (long i = 0; i < n; ++i) stepOneCycle(T);
}
```

- [ ] **Step 5: Run the test, verify it passes**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
    && /tmp/vconv-test
```
Expected: `vconv-test: all asserts passed`.

- [ ] **Step 6: Commit**

```bash
git add src/sim/vconv.h src/sim/vconv.cpp test/host-stub/vconv-test.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): analytic per-cycle coil solver (CCM + diode emulation)

Closed-form piecewise-linear I_L across HS / LS / off phases. Computes
Iin_avg + Iout_avg via trapezoid areas. Handles natural DCM zero-
crossing (clamp I_L=0 mid-LS or mid-off) and signed reverse current.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: DCM/CCM mode flag + boundary test

**Files:**
- Modify: `src/sim/vconv.cpp`
- Modify: `test/host-stub/vconv-test.cpp`

- [ ] **Step 1: Add failing DCM-boundary test**

Append to `vconv-test.cpp`:

```cpp
static void test_dcm_ccm_boundary() {
    VirtualConverter vc;
    vc.setPv(2.0f, 60.0f, 2.0f);          // low Isc -> low Iout -> DCM
    vc.setBat(28.0f, 0.05f);
    vc.setPassives(470e-6f, 470e-6f, 50e-6f);
    vc.setVin(60.0f);
    vc.setVout(28.0f);

    VirtualConverter::PwmState p{
        .pwmMax = 1023, .pwmCtrl = 478,
        .pwmRect = static_cast<uint16_t>(1023 - 478 - 1),
        .pwmFreq = 39000,
    };
    vc.setPwm(p);
    for (int i = 0; i < 200; ++i) vc.stepSeconds(1.0f / 39000.0f, 39000);

    assert(vc.inDcm() == true);

    // Crank Isc up -> should leave DCM.
    vc.setPv(8.0f, 60.0f, 2.0f);
    for (int i = 0; i < 400; ++i) vc.stepSeconds(1.0f / 39000.0f, 39000);
    assert(vc.inDcm() == false);
}
```

Register the test in `main()`.

- [ ] **Step 2: Run, verify failure**

Expected: `inDcm()` returns false at the first assert (`dcm_` is never set).

- [ ] **Step 3: Set the DCM flag inside `stepOneCycle`**

In `src/sim/vconv.cpp::stepOneCycle`, after computing `iOutAvg_`, add hysteresis matching `buck.h::DcmEnterRippleRatio` / `DcmExitRippleRatio`:

```cpp
    // DCM/CCM detection. Mirror buck.h's hysteresis (Enter=2.0, Exit=1.8).
    if (L > 0.0f && pwm_.pwmFreq > 0) {
        const float ripple = (vOut_ > 0.0f && vIn_ > vOut_)
            ? vOut_ / (float(pwm_.pwmFreq) * L) * (1.0f - vOut_ / vIn_)
            : 0.0f;
        const float ratio = dcm_ ? 1.8f : 2.0f;
        if (iOutAvg_ < 0.1f || ripple > ratio * iOutAvg_) dcm_ = true;
        else dcm_ = false;
    }
```

Place this block after the `iOutAvg_ = ...` assignment and before the cap-dynamics block.

- [ ] **Step 4: Run, verify passes**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
    && /tmp/vconv-test
```
Expected: `vconv-test: all asserts passed`.

- [ ] **Step 5: Commit**

```bash
git add src/sim/vconv.cpp test/host-stub/vconv-test.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): DCM/CCM mode detection with buck.h-matched hysteresis

inDcm() flips on Enter=2.0, off on Exit=1.8 ripple/iOut ratio so the
model boundary aligns with SynchronousConverter::computeDCM.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Reverse-current regime + backflow clamp tests

**Files:**
- Modify: `test/host-stub/vconv-test.cpp`

- [ ] **Step 1: Add the failing tests**

Append to `vconv-test.cpp`:

```cpp
static void test_reverse_current_vin_pump() {
    VirtualConverter vc;
    vc.setPv(0.5f, 60.0f, 2.0f);          // weak PV
    vc.setBat(28.0f, 0.05f);
    vc.setPassives(470e-6f, 470e-6f, 50e-6f);
    vc.setVin(40.0f);                     // headroom under Voc
    vc.setVout(28.0f);

    // Small HS pulse, long LS pulse -> I_L_after_LS << 0 -> HS body diode pump.
    VirtualConverter::PwmState p{
        .pwmMax = 1023,
        .pwmCtrl = 200,
        .pwmRect = static_cast<uint16_t>(1023 - 200 - 1),
        .pwmFreq = 39000,
    };
    vc.setPwm(p);

    const float vinStart = vc.getVin();
    for (int i = 0; i < 800; ++i) vc.stepSeconds(1.0f / 39000.0f, 39000);

    assert(vc.getVin() > vinStart + 0.5f);
}

static void test_backflow_clamps_when_disabled() {
    VirtualConverter vc;
    vc.setPv(8.0f, 60.0f, 2.0f);
    vc.setBat(28.0f, 0.05f);
    vc.setPassives(470e-6f, 470e-6f, 50e-6f);
    vc.setVin(40.0f);
    vc.setVout(20.0f);                    // below Vbat

    VirtualConverter::PwmState p{ .pwmMax = 1023, .pwmCtrl = 0, .pwmRect = 0, .pwmFreq = 39000 };
    vc.setPwm(p);
    const float voutStart = vc.getVout();
    for (int i = 0; i < 200; ++i) vc.stepSeconds(1.0f / 39000.0f, 39000);
    // pwmCtrl==0 + Vout<Vbat -> Ibat=0, no source path, Vout unchanged.
    assert(approxEq(vc.getVout(), voutStart, 0.01f));
}
```

Register both in `main()`.

- [ ] **Step 2: Run, verify passes**

The reverse-current path and the backflow clamp were both wired in Task 2/3. This test asserts that behavior.

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
    && /tmp/vconv-test
```
Expected: passes. If it fails, the most likely cause is the phase-3 HS-body-diode branch in `stepOneCycle` — verify the `else if (c < 0.0f)` block contributes negative area to `iInAvg_` (which feeds back into `vIn_ += (Ipv - iInAvg_) ...` — negative `iInAvg_` makes Vin rise).

- [ ] **Step 3: Commit**

```bash
git add test/host-stub/vconv-test.cpp
git commit -m "$(cat <<'EOF'
test(vconv): reverse-current Vin pump + backflow clamp on disable

Two asserts: (a) holding LS past the natural zero crossing causes Vin
to rise as the HS body diode pumps coil energy back into Cin; (b) with
pwmCtrl==0 and Vout<Vbat the battery does not source current into Vout.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `PWM_VConv` shim

**Files:**
- Modify: `src/sim/vconv.h` (add `pwm()` getter)
- Modify: `src/sim/vconv.cpp` (define `g_vconv`)
- Create: `src/pwm/vconv.h`

- [ ] **Step 1: Expose `pwm()` getter**

In `src/sim/vconv.h`, add to the public section (next to `setPwm`):

```cpp
    [[nodiscard]] const PwmState &pwm() const { return pwm_; }
```

- [ ] **Step 2: Define the `g_vconv` singleton**

In `src/sim/vconv.cpp`, after the `#include`, add:

```cpp
// Singleton accessed by PWM_VConv (writer) and ADC_VConv (reader). See
// src/pwm/vconv.h and src/adc/vconv.h.
VirtualConverter g_vconv;
```

- [ ] **Step 3: Create `src/pwm/vconv.h`**

```cpp
#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

#include "sim/vconv.h"

// Global instance defined in src/sim/vconv.cpp. PWM_VConv writes, ADC_VConv
// reads. Single producer (RT loop) and single consumer (ADC sample timer),
// both on core 1, so no synchronisation required.
extern VirtualConverter g_vconv;

// LEDC-style PWM driver shim that forwards update_pwm() into g_vconv. Selected
// at compile-time by buck.h when WITH_VCONV=1 (see CMakeLists). Mirrors the
// surface of src/pwm/mock.h.
class PWM_VConv {
public:
    static constexpr const char *name = "vconv";

    uint16_t pwmMax = 0;

    void init_pwm(int channel, int pin, uint32_t freq) {
        (void) channel; (void) pin;
        // Match LEDC's auto duty resolution (see PWM_Mock::init_pwm).
        uint32_t div = 80000000U / freq;
        int resolution = std::min((int) std::log2(double(div)), 14);
        uint16_t pm = uint16_t((2 << (resolution - 1)) - 1);
        if (pwmMax == 0) pwmMax = pm;
        VirtualConverter::PwmState s = g_vconv.pwm();
        s.pwmMax = pwmMax;
        s.pwmFreq = freq;
        g_vconv.setPwm(s);
    }

    // EnLogic / HS path: single-arg LEDC update. For HS the value is pwmCtrl
    // (HS on-count). For LS-EnLogic the firmware passes (pwmCtrl + pwmRect)
    // because the SD pin separates the regions — so we must subtract.
    // See buck.h::pwmPerturb (EnLogic branch).
    void update_pwm(int channel, uint32_t duty) {
        VirtualConverter::PwmState s = g_vconv.pwm();
        if (channel == 0) {
            s.pwmCtrl = uint16_t(duty);
        } else {
            uint16_t off = uint16_t(duty);
            s.pwmRect = (off > s.pwmCtrl) ? uint16_t(off - s.pwmCtrl) : uint16_t(0);
        }
        g_vconv.setPwm(s);
    }

    // HiLi / LEDC two-arg: pulse goes HIGH at `hpoint`, LOW at `hpoint+duty`,
    // so `duty` IS the on-count for the addressed channel. Channel 0 = HS,
    // channel 1 = LS. See buck.h::pwmPerturb (HiLi branch, !pwmEnLogic).
    void update_pwm(int channel, uint32_t hpoint, uint32_t duty) {
        (void) hpoint;
        VirtualConverter::PwmState s = g_vconv.pwm();
        if (channel == 0) s.pwmCtrl = uint16_t(duty);
        else              s.pwmRect = uint16_t(duty);
        g_vconv.setPwm(s);
    }

    void stop(int channel, int /*level*/) {
        VirtualConverter::PwmState s = g_vconv.pwm();
        if (channel == 0) s.pwmCtrl = 0;
        else              s.pwmRect = 0;
        g_vconv.setPwm(s);
    }
};
```

- [ ] **Step 4: Re-run host-stub vconv-test (no behavior change expected)**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall \
    -I test/host-stub -I src \
    -o /tmp/vconv-test test/host-stub/vconv-test.cpp src/sim/vconv.cpp \
    && /tmp/vconv-test
```
Expected: `vconv-test: all asserts passed`. Adding `g_vconv` definition must not break the existing test (it doesn't include `pwm/vconv.h`).

- [ ] **Step 5: Compile-only check of `src/pwm/vconv.h`**

```bash
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall -fsyntax-only \
    -I test/host-stub -I src \
    -x c++ src/pwm/vconv.h
```
Expected: clean output. If clang refuses to fsyntax-only a header, wrap the check in a 3-line `.cpp`:

```bash
printf '#include "pwm/vconv.h"\nVirtualConverter g_vconv;\nint main(){}\n' > /tmp/vconv_pwm_syntax.cpp
clang++ -std=gnu++17 -fexceptions -O0 -g -Wall -I test/host-stub -I src \
    -o /tmp/vconv_pwm_syntax /tmp/vconv_pwm_syntax.cpp src/sim/vconv.cpp
# Expected: no warnings, no errors. (We don't run it — symbol collision with
# the second g_vconv would link-fail, which is itself the check.)
```
The above will actually fail to link due to duplicate `g_vconv` — that's fine; the compile step proves the header is well-formed. Skip the link by adding `-c -o /tmp/vconv_pwm_syntax.o` instead.

- [ ] **Step 6: Commit**

```bash
git add src/pwm/vconv.h src/sim/vconv.h src/sim/vconv.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): PWM_VConv PwmDriver shim

LEDC-style update_pwm() calls forward into the g_vconv singleton.
Mirrors src/pwm/mock.h's surface so SynchronousConverter sees a normal
PwmDriver. Selected at compile time by buck.h when WITH_VCONV=1.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: `ADC_VConv` backend

**Files:**
- Create: `src/adc/vconv.h`

- [ ] **Step 1: Write `src/adc/vconv.h`**

Model the existing `src/adc/mock.h::ADC_Fake`. Channel-to-quantity table fixed in spec §Architecture.

```cpp
#pragma once

#include "adc.h"
#include "etc/rt.h"
#include "sim/vconv.h"

const unsigned long &wallClockUs();

extern VirtualConverter g_vconv;

static bool adc_vconv_periodic_timer_callback(void *arg);

// AsyncADC backend driven by the VirtualConverter model. Steps the model on
// every sample tick and returns the requested channel's state.
//
// Channel map (must match config/lab/vconv_mock/conf/sensor.conf):
//   0 -> Vin (V)        1 -> Iin_avg (A)    2 -> Iout_avg (A)
//   3 -> Vout (V)       4 -> NTC (constant)
class ADC_VConv : public AsyncADC<float> {
public:
    uint8_t readingChannel = 0;

    [[nodiscard]] SampleReadScheme scheme() const override {
        return SampleReadScheme::all;
    }

    bool init(const ConfFile &boardConf) override {
        auto f = boardConf.getLong("adc_vconv_freq",
                                   boardConf.getLong("adc_fake_freq", 1000 * 3));
        sampleFreqHz_ = (uint32_t) f;
        periodic_timer_.begin(f, &adc_vconv_periodic_timer_callback, this);
        periodic_timer_.start();
        taskNotification_.subscribe(true);
        return true;
    }

    void deinit() override { periodic_timer_.destroy(); }

    float getSamplingRate(uint8_t) override {
        return static_cast<float>(periodic_timer_.freq());
    }

    void startReading(uint8_t channel) override {
        readingChannel = channel;
        taskNotification_.subscribe();
    }

    bool hasData() override { return taskNotification_.wait(10); }

    void setMaxExpectedVoltage(uint8_t, float) override {}

    float getSample() override {
        // Advance the model by 1/sampleFreq seconds on the *first* channel
        // read of each round so all five channels see a consistent state.
        if (readingChannel == 0 && sampleFreqHz_ > 0) {
            g_vconv.stepSeconds(1.0f / float(sampleFreqHz_), g_vconv.pwm().pwmFreq);
        }
        switch (readingChannel) {
            case 0: return g_vconv.getVin();
            case 1: return g_vconv.getIinAvg();
            case 2: return g_vconv.getIoutAvg();
            case 3: return g_vconv.getVout();
            case 4: return 0.5f;        // NTC stub (mid-rail)
            default: return 0.0f;
        }
    }

    float getInputImpedance(uint8_t) override { return 100e3f; }

    void reset(const uint8_t ch) override { (void) ch; }

    bool periodicTimerCallback() {
        return taskNotification_.notifyFromIsr();
    }

private:
    TaskNotification taskNotification_{};
    PeriodicTimer periodic_timer_{};
    uint32_t sampleFreqHz_ = 0;
};

static IRAM_ATTR bool adc_vconv_periodic_timer_callback(void *arg) {
    auto adc = static_cast<ADC_VConv *>(arg);
    return adc->periodicTimerCallback();
}
```

- [ ] **Step 2: No standalone compile check (depends on `etc/rt.h` from the firmware build)**

This file is exercised by the firmware build in Task 9.

- [ ] **Step 3: Commit**

```bash
git add src/adc/vconv.h
git commit -m "$(cat <<'EOF'
feat(vconv): ADC_VConv AsyncADC backend

Periodic-timer driven sampler that steps g_vconv once per ADC period
and returns Vin/Iin/Iout/Vout/NTC by channel index. Mirrors
ADC_Fake's TaskNotification + PeriodicTimer pattern.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: `buck.h` selector + CMakeLists gate

**Files:**
- Modify: `src/buck.h`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Add the `WITH_VCONV` branch in `src/buck.h`**

Replace the existing block (around lines 9-26):

```cpp
#ifndef MOCK

#include <Arduino.h>
#if WITH_MCPWM
#include "pwm/mcpwm.h"
using PwmDriver = MCPWM_SyncLeg;
#else
#include "pwm/ledc.h"
using PwmDriver = PWM_ESP32_ledc;
#endif

#else

#include "pwm/mock.h"
using PwmDriver = PWM_Mock;

#endif
```

with:

```cpp
#ifndef MOCK

#include <Arduino.h>
#if WITH_VCONV
#include "pwm/vconv.h"
using PwmDriver = PWM_VConv;
#elif WITH_MCPWM
#include "pwm/mcpwm.h"
using PwmDriver = MCPWM_SyncLeg;
#else
#include "pwm/ledc.h"
using PwmDriver = PWM_ESP32_ledc;
#endif

#else

#include "pwm/mock.h"
using PwmDriver = PWM_Mock;

#endif
```

- [ ] **Step 2: Add `vconv.cpp` to the build and gate the define**

In `main/CMakeLists.txt`, locate the `idf_component_register(SRCS ...)` block (around line 64). Add `"../src/sim/vconv.cpp"` next to the other component sources, e.g. after `"../src/math/float16.cpp"`:

```cmake
        "../src/math/float16.cpp"
        "../src/sim/vconv.cpp"
        ${NETW_SRC}
```

After the existing `WITH_MCPWM` block (around line 103-107), add:

```cmake
# WITH_VCONV=1 replaces the LEDC gate driver with PWM_VConv and lets sensor.conf
# select the ADC_VConv backend (*_adc=vconv). Mutually exclusive with WITH_MCPWM
# because PWM_VConv mirrors only the LEDC-style API.
if ($ENV{WITH_VCONV})
    if ($ENV{WITH_MCPWM})
        message(FATAL_ERROR "WITH_VCONV and WITH_MCPWM are mutually exclusive")
    endif ()
    component_compile_definitions("WITH_VCONV=1")
endif ()
```

- [ ] **Step 3: Verify the default build still works without `WITH_VCONV`**

```bash
. ./idf-export.sh
idf.py build 2>&1 | tail -30
```
Expected: build succeeds.

- [ ] **Step 4: Verify `WITH_VCONV=1` builds**

```bash
WITH_VCONV=1 idf.py build 2>&1 | tail -30
```
Expected: build succeeds. ADC_VConv is unreferenced at this stage (it's only pulled in by `sensor_setup.cpp` in Task 9) — so the link should still succeed.

- [ ] **Step 5: Commit**

```bash
git add src/buck.h main/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(vconv): buck.h selector + CMake gate for WITH_VCONV

WITH_VCONV=1 swaps PwmDriver to PWM_VConv; mutually exclusive with
WITH_MCPWM. Adds vconv.cpp to component SRCS.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: `setupSensors()` recognises `*_adc=vconv`

**Files:**
- Modify: `src/sensor_setup.cpp`

- [ ] **Step 1: Add the `vconv` branch in `createAdcInstance`**

In `src/sensor_setup.cpp`, near the top (after `#include "adc/mock.h"`):

```cpp
#if WITH_VCONV
#include "adc/vconv.h"
#endif
```

Inside `createAdcInstance` (the if/else ladder around lines 29-41), after the `"fake"` branch, before the trailing `else { throw ... }`, insert:

```cpp
#if WITH_VCONV
    } else if (adcName == "vconv") {
        adc = new ADC_VConv();
#endif
```

- [ ] **Step 2: Build with WITH_VCONV**

```bash
WITH_VCONV=1 idf.py build 2>&1 | tail -30
```
Expected: build succeeds.

- [ ] **Step 3: Build without WITH_VCONV (sanity)**

```bash
idf.py build 2>&1 | tail -30
```
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/sensor_setup.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): sensor_setup recognises *_adc=vconv

Gated by WITH_VCONV so non-vconv builds are unaffected.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Load `vconv.conf` in `main.cpp::setup()`

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Find the right insertion point**

In `src/main.cpp::setup()`, locate the call to `setupSensors(boardConf, lim);` (currently around line 253). The vconv init must run BEFORE this call, because `setupSensors` constructs `ADC_VConv` which immediately starts a periodic timer that reads `g_vconv` state. The project's main `coilConf` is only created later (around line 260), so we read coil.conf locally here.

- [ ] **Step 2: Add vconv.conf load + include**

Near the other adc-related includes at the top of `src/main.cpp`:

```cpp
#if WITH_VCONV
#include "sim/vconv.h"
#endif
```

Immediately before the `setupSensors(boardConf, lim);` call, insert:

```cpp
#if WITH_VCONV
    {
        ConfFile vconvConf{"/littlefs/conf/vconv.conf"};
        ConfFile vconvCoilConf{"/littlefs/conf/coil.conf"};
        const float l0 = vconvCoilConf ? vconvCoilConf.getFloat("L0", 50e-6f) : 50e-6f;
        if (vconvConf) {
            g_vconv.setPv(
                vconvConf.getFloat("isc", 8.0f),
                vconvConf.getFloat("voc", 40.0f),
                vconvConf.getFloat("pv_k", 2.0f));
            g_vconv.setBat(
                vconvConf.getFloat("v_bat", 28.0f),
                vconvConf.getFloat("r_bat", 0.05f));
            g_vconv.setPassives(
                vconvConf.getFloat("c_in", 470e-6f),
                vconvConf.getFloat("c_out", 470e-6f),
                l0);
            // Initial cap voltages so MPPT has somewhere reasonable to start.
            g_vconv.setVin(vconvConf.getFloat("voc", 40.0f) * 0.9f);
            g_vconv.setVout(vconvConf.getFloat("v_bat", 28.0f));
        } else {
            g_vconv.setPassives(470e-6f, 470e-6f, l0);
            ESP_LOGW("vconv", "no vconv.conf — using built-in defaults");
        }
    }
#endif
```

- [ ] **Step 3: Build both configurations**

```bash
WITH_VCONV=1 idf.py build 2>&1 | tail -30
```
Expected: build succeeds.

```bash
idf.py build 2>&1 | tail -30
```
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): wire vconv.conf into setup()

Boot-time defaults for PV (isc/voc/pv_k), battery (v_bat/r_bat), and
caps (c_in/c_out). L0 still comes from coil.conf so vconv mirrors what
buck.h sees.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: `vconv` console command

**Files:**
- Modify: `src/cli.cpp`

- [ ] **Step 1: Add the `cmdVconv` handler**

In `src/cli.cpp`, near the other diagnostic commands (after `cmdMeasureCoil`), add:

```cpp
#if WITH_VCONV
#include "sim/vconv.h"
extern VirtualConverter g_vconv;

static void cmdVconv(cmd *c) {
    Command cc(c);
    auto n = cc.countArgs();
    if (n == 0) {
        UART_LOG("vconv: Vin=%.2f Vout=%.2f IL=%.3f Iin=%.3f Iout=%.3f dcm=%d",
                 g_vconv.getVin(), g_vconv.getVout(), g_vconv.getIL(),
                 g_vconv.getIinAvg(), g_vconv.getIoutAvg(),
                 g_vconv.inDcm() ? 1 : 0);
        return;
    }
    auto sub = cc.getArg(0).getValue();
    if (sub == "pv") {
        if (n < 3) CMD_FAIL_RETURN("vconv pv <isc> <voc> [k]");
        float isc = cc.getArg(1).getValue().toFloat();
        float voc = cc.getArg(2).getValue().toFloat();
        float k   = (n >= 4) ? cc.getArg(3).getValue().toFloat() : 2.0f;
        g_vconv.setPv(isc, voc, k);
        UART_LOG("vconv pv: isc=%.2f voc=%.2f k=%.2f", isc, voc, k);
    } else if (sub == "bat") {
        if (n < 2) CMD_FAIL_RETURN("vconv bat <v>");
        float v = cc.getArg(1).getValue().toFloat();
        g_vconv.setBat(v, 0.05f);
        UART_LOG("vconv bat: v=%.2f", v);
    } else {
        CMD_FAIL_RETURN("vconv: expected pv|bat or no args");
    }
}
#endif
```

The SimpleCLI helpers `Command::countArgs()` / `Command::getArg(i).getValue()` / `.toFloat()` / `.toInt()` are the names used throughout `src/cli.cpp` (see `cmdSync`, `cmdDc`, etc.). The `boundless` cmd variant returns the whole tail string as one positional arg when the user types `vconv pv 8 40` — `cc.getArg(0)` will then yield "pv 8 40" instead of "pv". If that's the case for this SimpleCLI version (verify by adding `ESP_LOGI("vconv","dbg n=%d a0=%s", n, cc.getArg(0).getValue().c_str());` at the top of `cmdVconv` and observing one boot), switch to `cli.addCmd("vconv", cmdVconv)` with `cc.getArg("...")` named-arg APIs as used by `cmdSync` patterns elsewhere in this file.

- [ ] **Step 2: Register the command**

Find the `cli.addBoundlessCmd("measure-coil", cmdMeasureCoil);` line and add immediately after:

```cpp
#if WITH_VCONV
    cli.addBoundlessCmd("vconv", cmdVconv);
#endif
```

- [ ] **Step 3: Build both configurations**

```bash
WITH_VCONV=1 idf.py build 2>&1 | tail -30
idf.py build 2>&1 | tail -30
```
Expected: both succeed.

- [ ] **Step 4: Commit**

```bash
git add src/cli.cpp
git commit -m "$(cat <<'EOF'
feat(vconv): console command for runtime PV/battery overrides

  vconv             dump model state
  vconv pv I V [k]  override PV (isc, voc, k)
  vconv bat V       override battery voltage

Non-persistent test-driving surface; set-config vconv.conf is the
persistent path.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: `config/lab/vconv_mock/` board config

**Files:**
- Create: `config/lab/vconv_mock/conf/*.conf` (whole directory, seeded from wokwi_mock)

- [ ] **Step 1: Seed conf files from `config/lab/wokwi_mock/conf/`**

```bash
mkdir -p config/lab/vconv_mock/conf
cp config/lab/wokwi_mock/conf/*.conf config/lab/vconv_mock/conf/
```

- [ ] **Step 2: Edit `config/lab/vconv_mock/conf/sensor.conf`**

Replace the entire file content with:

```
adc=vconv
expected_hz=390
power_conversion_eff=0.97

vin_adc=vconv
vin_ch=0
vin_rh=0
vin_rl=1

iin_adc=vconv
iin_ch=1

iout_adc=vconv
iout_ch=2

vout_adc=vconv
vout_ch=3
vout_rh=0
vout_rl=1

ntc_adc=vconv
ntc_ch=4
ntc_filt_len=60
```

`(Rh=0, Rl=1)` makes the voltage-divider formula `(Rh+Rl)/Rl = 1.0`, so `ADC_VConv` can return raw physical V without any scaling fight.

- [ ] **Step 3: Create `config/lab/vconv_mock/conf/vconv.conf`**

```
# PV source
isc=8.0
voc=40.0
pv_k=2.0

# Battery sink
v_bat=28.0
r_bat=0.05

# Passives (L is read from coil.conf::L0)
c_in=470e-6
c_out=470e-6

# Sampling tick
adc_vconv_freq=3000
```

- [ ] **Step 4: Verify board.conf has the bits vconv needs**

Open `config/lab/vconv_mock/conf/board.conf` and confirm:
- `pwm_freq=39000` (or similar — vconv reads it via PWM_VConv::init_pwm).
- `skip_assert=1` (no real pins to assert on a mock).
- `pwm_driver_logic=HiLi` (matches PWM_VConv's two-edge interpretation).

If any are missing, add them.

- [ ] **Step 5: Commit**

```bash
git add config/lab/vconv_mock/
git commit -m "$(cat <<'EOF'
feat(vconv): vconv_mock board config

PV (isc=8/voc=40/k=2) -> stiff 28 V battery, 50 µH coil, 470 µF caps.
sensor.conf routes all channels through *_adc=vconv with Rh=0,Rl=1 so
the divider factor collapses to 1.0 and ADC_VConv returns physical
units directly.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: Update `doc/Configuration.md` and conf-editor

**Files:**
- Modify: `doc/Configuration.md`
- Modify: `etc/config-tool/conf-editor.html`

- [ ] **Step 1: Add `vconv.conf` section to `doc/Configuration.md`**

After the `## coil.conf — inductor` block, insert:

```markdown
## vconv.conf — virtual converter simulator (`WITH_VCONV` builds)

Embedded software model of a synchronous buck converter that replaces real hardware
sensing. Only consulted when the firmware was built with `WITH_VCONV=1`.

| key              | unit | default  | description                                                  |
|------------------|------|----------|--------------------------------------------------------------|
| `isc`            | A    | 8.0      | PV short-circuit current                                     |
| `voc`            | V    | 40.0     | PV open-circuit voltage                                      |
| `pv_k`           | V    | 2.0      | PV exponential sharpness (smaller = sharper MPP)             |
| `v_bat`          | V    | 28.0     | stiff battery voltage                                        |
| `r_bat`          | Ω    | 0.05     | small series R for stiffness (prevents clamp δ-functions)    |
| `c_in`           | F    | 470e-6   | input capacitance                                            |
| `c_out`          | F    | 470e-6   | output capacitance                                           |
| `adc_vconv_freq` | Hz   | 3000     | ADC_VConv sample tick                                        |

Coil L comes from `coil.conf::L0` so vconv mirrors what `buck.h` uses for ripple
math.

See `docs/superpowers/specs/2026-05-23-virtual-converter-design.md`.
```

- [ ] **Step 2: Add `vconv.conf` keys to `etc/config-tool/conf-editor.html`**

Open `etc/config-tool/conf-editor.html`. Inside the `const META = { ... }` block (search for `// sensor.conf`), insert a new section before the `// coil.conf` block:

```js
  // vconv.conf
  isc:{unit:"A",desc:"PV short-circuit current",type:"float"},
  voc:{unit:"V",desc:"PV open-circuit voltage",type:"float"},
  pv_k:{unit:"V",desc:"PV exponential sharpness (smaller = sharper MPP)",type:"float"},
  v_bat:{unit:"V",desc:"Stiff battery voltage",type:"float"},
  r_bat:{unit:"Ω",desc:"Small series R for battery stiffness",type:"float"},
  c_in:{unit:"F",desc:"Input capacitance",type:"float"},
  c_out:{unit:"F",desc:"Output capacitance",type:"float"},
  adc_vconv_freq:{unit:"Hz",desc:"ADC_VConv sample tick",type:"int"},
```

Inside `const FILE_KEYS = { ... }`, near `"coil.conf":[...]`, add:

```js
  "vconv.conf":["isc","voc","pv_k","v_bat","r_bat","c_in","c_out","adc_vconv_freq"],
```

- [ ] **Step 3: Sanity check by inspection**

Grep that all the new keys made it in and that the JS still parses as far as a simple regex check can tell:

```bash
grep -n "vconv.conf\|adc_vconv_freq" etc/config-tool/conf-editor.html
```
Expected: at least two hits — one in `FILE_KEYS`, one in `META`.

Open the file in a browser to confirm the conf-editor still renders. If a syntax error breaks the page, the browser console will pinpoint the line.

- [ ] **Step 4: Commit**

```bash
git add doc/Configuration.md etc/config-tool/conf-editor.html
git commit -m "$(cat <<'EOF'
doc(vconv): vconv.conf reference + conf-editor metadata

Per the CLAUDE.md three-source rule (doc + conf-editor) for new .conf
keys.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 14: On-target smoke test (manual procedure)

**Files:** none committed

- [ ] **Step 1: Point the littlefs image at vconv_mock for this developer run**

In top-level `CMakeLists.txt`, find the `littlefs_create_partition_image(... config/lab/wokwi_mock ...)` call and temporarily change the path to `config/lab/vconv_mock`. Do not commit.

- [ ] **Step 2: Build, flash, monitor**

```bash
. ./idf-export.sh
WITH_VCONV=1 idf.py set-target esp32s3
WITH_VCONV=1 idf.py build
WITH_VCONV=1 idf.py -p $ESPPORT flash monitor
```
Expected: device boots, no protection trip. Logs include:
- `converter: ... pwmMax=... minLS=...` (normal init line).
- MPPT lines as the global sweep + fast P&O run.

- [ ] **Step 3: Verify MPPT lands near `0.8 * Voc`**

Wait ~60 s for the sweep + tracker to converge, then in the monitor console:

```
vconv
```
Expected (approximate): `Vin ≈ 32` (`0.8 * Voc=40`), `Vout ≈ 28`, `Iout` > 0, `dcm` is 0 or 1 depending on the operating point.

`sensor` should print the same Vin/Vout numbers from the regular sensor path.

- [ ] **Step 4: Smoke-test PV override**

```
vconv pv 4.0 50.0
```
Wait ~15 s, then `sensor` and `vconv`. Expected: `Vin` rises toward ~40 V (new `0.8 * Voc`), `Iout` drops.

- [ ] **Step 5: Smoke-test the reverse-current regime (optional)**

```
dc 200
sync on
```
After ~5 s of running, `vconv` should show `Vin` above its previous value (HS body diode pumping). If the protection layer trips first (Vin OV), that's also a pass — the model is producing physically meaningful behavior, just outside the protection envelope.

- [ ] **Step 6: Restore `CMakeLists.txt`**

Revert the temporary littlefs path change. Do not commit.

---

## Self-review notes

Spec coverage matrix (each spec section → task that implements it):

| Spec section | Task(s) |
|---|---|
| §Architecture (PWM_VConv, VirtualConverter, ADC_VConv split) | 1, 6, 7 |
| §Compile-time selector (`WITH_VCONV`) | 8 |
| §Runtime selector (`*_adc=vconv` in sensor.conf) | 9, 12 |
| §State (`V_in`, `V_out`, `I_L_end`) | 2, 3 |
| §PWM input + LEDC API reconciliation | 6 |
| §Per-cycle solution (HS / LS / off phases) | 3 |
| §Cap dynamics + numerical guards | 2, 3 |
| §Sources/sinks (PV exponential, stiff battery, conditional backflow) | 1, 2, 5 |
| §N-cycle stepping in ADC backend | 7 |
| §Configuration (`vconv.conf`) | 10, 12 |
| §Console command | 11 |
| §Tests (host-stub: CCM, DCM, reverse current, backflow) | 1, 3, 4, 5 |
| §On-target integration test | 14 |

All host-stub tests live in a single `test/host-stub/vconv-test.cpp` so each task can grow it without touching CMakeLists.

Type consistency:
- `VirtualConverter::PwmState` — same struct used in `PWM_VConv::update_pwm` (Task 6) and (indirectly via `g_vconv.pwm()`) in `ADC_VConv::getSample` (Task 7).
- `g_vconv` is the single global, **defined** in `src/sim/vconv.cpp` (Task 6), **declared `extern`** in `src/pwm/vconv.h`, `src/adc/vconv.h`, `src/cli.cpp`, `src/main.cpp`.
- Public method names — `getVin / getVout / getIinAvg / getIoutAvg / getIL / inDcm / pwm()` — used identically across `cli.cpp`, `adc/vconv.h`, and the host-stub tests.
- Setters — `setPv(isc,voc,k) / setBat(v,r) / setPassives(cin,cout,L) / setVin / setVout / setPwm` — defined in Task 1-3, exercised in Task 10/11/host-stub tests.
