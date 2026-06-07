#include <unity.h>
#include <cmath>

#include "etc/rt.h"
#include "sim/vconv.h"
#include "adc/vconv.h"

// Coverage for the vconv catch-up fix (src/etc/rt.h::waitCount + src/adc/vconv.h::hasData):
// when the RT loop overruns an ADC tick, the periodic-timer notifications coalesce. The old
// hasData() stepped the plant by exactly one dt regardless of how many ticks piled up, so it
// silently dropped sim-time and the plant clock drifted from any wall-clock-driven control logic.
// The fix reads the coalesced count and advances the plant by that many ticks (capped).

static const float kDt = 1.0f / 3000.0f;   // adc_freq default

// Active buck operating point: coil current actually evolves cycle-to-cycle.
static void configActive(VirtualConverter &c) {
    c.setPv(13.0f, 76.0f, 0.85f);
    c.setBat(27.0f, 0.05f);
    c.setPassives(470e-6f, 470e-6f, 50e-6f);
    c.setVin(60.0f);
    c.setVout(27.0f);
    VirtualConverter::PwmState p{};
    p.pwmMax = 1000; p.pwmCtrl = 450; p.pwmRect = 450; p.pwmFreq = 39000;
    c.setPwm(p);
}

// Idle PV charging an open output: Vin ramps monotonically from 0, iLEnd is forced to 0 each cycle
// (no steady-state masking), so plant advance is a clean, deterministic function of cycle count.
// Huge C_in keeps Vin << Voc over the test window -> pvCurrent ~ Isc -> Vin grows ~linearly with
// cycles, so a capped advance stays distinguishable from an uncapped one (no Voc saturation).
static void configIdleLinear(VirtualConverter &c) {
    c.setPv(13.0f, 76.0f, 0.85f);
    c.setBat(0.0f, 1e9f);
    c.setPassives(1.0f, 470e-6f, 50e-6f);
    c.setVin(0.0f);
    c.setVout(0.0f);
    VirtualConverter::PwmState p{};
    p.pwmMax = 0; p.pwmFreq = 39000;   // converter off -> idle branch
    c.setPwm(p);
}

// rt.h: waitCount() must surface the *number* of coalesced notifications, not collapse to 1.
// (wait() still returns bool; the count is what lets hasData() catch up.)
void test_vconv_waitcount_returns_burst_count() {
    TaskNotification n;
    n.subscribe();
    n.waitCount(0);                                   // drain stale count from prior tests
    for (int i = 0; i < 7; ++i) n.notify();
    TEST_ASSERT_EQUAL_UINT32(7, n.waitCount(10));     // full burst, not 1
    TEST_ASSERT_EQUAL_UINT32(0, n.waitCount(1));      // drained in one read (clear-on-exit)
}

// Catch-up invariant: advancing by N*dt in one stepSeconds() lands on the exact same state as N
// separate dt steps. This is what makes collapsing a coalesced burst into one catch-up step lossless.
void test_vconv_catchup_equals_uncoalesced() {
    const int N = 5;
    VirtualConverter one, many;
    configActive(one); configActive(many);
    one.stepSeconds(kDt * (float) N, 39000);                      // single catch-up step
    for (int i = 0; i < N; ++i) many.stepSeconds(kDt, 39000);     // N un-coalesced steps
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, many.getVin(),  one.getVin());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, many.getVout(), one.getVout());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, many.getIL(),   one.getIL());
}

// Motivation: the OLD hasData() stepped one dt for the whole burst, dropping (N-1)*dt of sim-time.
// Prove that drop was material (not a rounding nit) -- single-step diverges hard from the N-step ref.
void test_vconv_single_step_drops_simtime() {
    const int N = 5;
    VirtualConverter dropped, ref;
    configActive(dropped); configActive(ref);
    dropped.stepSeconds(kDt, 39000);                             // old behavior: one step, burst lost
    for (int i = 0; i < N; ++i) ref.stepSeconds(kDt, 39000);
    TEST_ASSERT_TRUE(std::fabs(ref.getVin() - dropped.getVin()) > 0.5f);
}

// Integration: drive the real ADC_VConv path. Inject K timer-tick notifications the way the periodic
// ISR would (periodicTimerCallback), then a single hasData() must advance g_vconv by K ticks -- and
// cap a gross burst at kMaxCatchupTicks (8) so a stall can't become a multi-ms catch-up spike.
void test_vconv_hasdata_steps_capped_coalesced_count() {
    ADC_VConv adc;
    adc.startReading(0);     // subscribe this task; no init() -> no real periodic timer running
    adc.hasData();           // drain any stale notification

    // (a) small burst, below the cap -> advances exactly K ticks.
    const int K = 3;
    configIdleLinear(g_vconv);
    for (int i = 0; i < K; ++i) adc.periodicTimerCallback();
    TEST_ASSERT_TRUE(adc.hasData());
    VirtualConverter refK; configIdleLinear(refK); refK.stepSeconds(kDt * (float) K, 39000);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, refK.getVin(), g_vconv.getVin());

    // (b) gross burst -> capped at 8, NOT 20.
    configIdleLinear(g_vconv);
    for (int i = 0; i < 20; ++i) adc.periodicTimerCallback();
    TEST_ASSERT_TRUE(adc.hasData());
    VirtualConverter refCap;  configIdleLinear(refCap);  refCap.stepSeconds(kDt * 8.0f, 39000);
    VirtualConverter refFull; configIdleLinear(refFull); refFull.stepSeconds(kDt * 20.0f, 39000);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, refCap.getVin(), g_vconv.getVin());   // matches the 8-tick cap
    TEST_ASSERT_TRUE(g_vconv.getVin() < refFull.getVin() - 0.01f);        // well below uncapped 20
}

// ---- inverter-ripple disturbance (modelled from the fry 2 kW capture) -----------------------
// Settle a configured plant, sample V_out at adc_freq, and return the RMS ripple (% of mean) and
// the 2nd-harmonic / fundamental amplitude ratio. The shape assertions are the regression: a sine
// (inverter) stays near-tone (tiny 2nd harmonic) while |sin| (rectifier) carries ~20% — matching
// the capture, where the 2nd harmonic was 0.7% << 20%.
static void measureRipple(VirtualConverter &c, float freq, float &rmsPct, float &h2ratio) {
    const float fs = 1.0f / kDt;
    for (int i = 0; i < 6000; ++i) c.stepSeconds(kDt, 39000);   // settle transients
    const int N = 4096;
    static float buf[4096];
    double mean = 0;
    for (int i = 0; i < N; ++i) { c.stepSeconds(kDt, 39000); buf[i] = c.getVout(); mean += buf[i]; }
    mean /= N;
    auto goertzel = [&](float f) {
        double re = 0, im = 0;
        for (int i = 0; i < N; ++i) { double a = 2.0 * M_PI * (double) f * i / fs; re += (buf[i] - mean) * std::cos(a); im -= (buf[i] - mean) * std::sin(a); }
        return std::sqrt(re * re + im * im);
    };
    double rms = 0;
    for (int i = 0; i < N; ++i) { double d = buf[i] - mean; rms += d * d; }
    rms = std::sqrt(rms / N);
    rmsPct = (float) (rms / mean * 100.0);
    double f1 = goertzel(freq), f2 = goertzel(freq * 2.0f);
    h2ratio = (f1 > 0.0) ? (float) (f2 / f1) : 0.0f;
}

// sine ripple (inverter): ~0.6% RMS V_out at the tone, with a negligible 2nd harmonic.
void test_vconv_inverter_ripple_is_sine() {
    VirtualConverter c; configActive(c); c.setBatRipple(0.26f, 100.0f, 0);
    float rms, h2; measureRipple(c, 100.0f, rms, h2);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 0.6f, rms);   // ~0.6% RMS like the fry capture (allow 0.3-0.9)
    TEST_ASSERT_TRUE(h2 < 0.05f);                // pure sine -> 2nd harmonic <5% (capture 0.7%)
}

// |sin| ripple (rectifier load): must carry the ~20% 2nd harmonic that distinguishes it from sine.
void test_vconv_rectifier_ripple_has_2nd_harmonic() {
    VirtualConverter c; configActive(c); c.setBatRipple(0.26f, 100.0f, 1);
    float rms, h2; measureRipple(c, 100.0f, rms, h2);
    TEST_ASSERT_TRUE(h2 > 0.10f && h2 < 0.30f);  // full-wave |sin|: ~20% 2nd harmonic
}

// spiky ripple (cheap China inverter): a narrow per-cycle current pulse, harmonic-rich. Its 2nd
// harmonic dominates over |sin| and sine — the signature that its energy spreads up toward Nyquist
// and aliases when sampled (the case that corrupts the current estimate).
void test_vconv_spiky_ripple_is_harmonic_rich() {
    VirtualConverter c; configActive(c); c.setBatRipple(0.26f, 100.0f, 2);
    float rms, h2; measureRipple(c, 100.0f, rms, h2);
    TEST_ASSERT_TRUE(h2 > 0.5f);                  // ((1+cos)/2)^8 -> h2/h1 ~0.70, peakier than |sin|
}

// amp=0 disables the disturbance (clean, deterministic baseline).
void test_vconv_no_ripple_when_amp_zero() {
    VirtualConverter c; configActive(c); c.setBatRipple(0.0f, 100.0f, 0);
    float rms, h2; measureRipple(c, 100.0f, rms, h2);
    TEST_ASSERT_TRUE(rms < 0.02f);
}

// Pluggability: a custom ripple model (sin + 0.5*sin(2x) = a deliberate 50% 2nd harmonic) passed
// via setBatRippleShape() must flow through the plant — proving a new noise model is one free
// function, no edits to the plant core.
static float rippleHalfSecondHarmonic(float p) { return std::sin(p) + 0.5f * std::sin(2.0f * p); }
void test_vconv_pluggable_custom_ripple_shape() {
    VirtualConverter c; configActive(c);
    c.setBatRipple(0.26f, 100.0f, 0);              // amp + freq
    c.setBatRippleShape(rippleHalfSecondHarmonic); // override with the custom model
    TEST_ASSERT_EQUAL_INT(-1, c.getVbatAcShape()); // marked custom
    float rms, h2; measureRipple(c, 100.0f, rms, h2);
    TEST_ASSERT_TRUE(h2 > 0.35f && h2 < 0.65f);    // custom 50% 2nd harmonic reproduced at V_out
}
