#pragma once

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <algorithm>


#ifndef MOCK

#include <Arduino.h>
//#include "pinconfig.h"
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

#include "conf.h"
#include "util.h"
#include "logging.h"
#include "pwm/mcpwm_timing.h"   // rectOffset{Counts,Ns} helpers (no hardware deps)

/**
 * Synchronous buck or boost converter
 */
class SynchronousConverter {
    static constexpr uint8_t pwmCh_Ctrl = 0; // Ctrl FET; buck: IN, HI (HS signal)
    static constexpr uint8_t pwmCh_Rect = 1; // Rect FET; buck: EN or LO, depending on `pwmEnLogic`

    /**
     * instead of the %µi-curve we simply use a constant drop factor
     * this appears to be sufficient for CCM/DCM decision
     * notice that the more accurate we model coil current, the higher the efficiency in DCM and near-DCM condition
     */
    static constexpr float InductivityDcBias = 0.95f;

    // --- diode-emulation tuning (see doc/Diode Emulation.md) ---
    // DCM is entered when half the ripple exceeds the dc current (ΔI/2 > Io, i.e. ir > 2·Io).
    // Hysteresis: once in DCM, stay there until ir drops below 1.8·Io to avoid mode chatter.
    static constexpr float DcmEnterRippleRatio = 2.0f;
    static constexpr float DcmExitRippleRatio = 1.8f;
    // Below this dc current the converter is treated as DCM regardless of the ripple comparison.
    static constexpr float DcmForceCurrent = 0.1f; // A
    // In DCM, fully disable sync rectification below these: the body diode handles the small
    // discharge and this avoids reverse current at near-zero load / output.
    static constexpr float SyncRectOffCurrent = 0.01f; // A
    static constexpr float SyncRectOffVoltage = 1.0f; // V
    // Minimum side voltage for the M=vl/vh (buck) / vh/vl (boost) ratio to be trustworthy.
    static constexpr float MinRatioVoltage = 0.1f; // V
    // M clamp: floor keeps rectCtrlRatio() finite (no div-by-zero); UnityRatioMargin keeps M off
    // 1.0 so rectCtrlRatio() stays finite and positive on both sides.
    static constexpr float MinVoltageRatio = 1e-2f;
    static constexpr float MaxBoostRatio = 10.f;
    static constexpr float UnityRatioMargin = 1e-2f;


    PwmDriver pwmDriver;
#if WITH_MCPWM
    MCPWM_FaultBrake faultBrake;
#endif

    bool pwmEnLogic = false; // whether the driver has a IN/SD input logic (instead of HI/LO)
    uint8_t pinSd = 255;
    bool dcmHysteresis = false;
    bool syncRectEnabled = false;

    uint16_t pwmCtrl = 0; // buck: HS
    uint16_t pwmRect = 0; // buck: LS
    uint16_t pwmRectMax = 0;
    int32_t manualRect = -1; // >=0: hold LS at this count (bench), -1: auto diode emulation
    float pwmRectRatioDCM = 0; // t_onRect/t_onCtrl when in DCM
    float rectDitherErr = 0; // carried LS-count rounding remainder (DCM error-feedback dither)
    bool rectDither = true; // false: plain round() (fallback)
    int16_t rectOnOffset = 0; // DCM LS-count dead-time/gate-delay offset (counts; >0 = LS off later, toward zero crossing)

    float outInVoltageRatio = 0; // M
    float directionFloatBuffer = 0.0f; // fractional perturbation buffer

    bool isBoost = false;
    bool forcedPwm = false;

    float fL = NAN; // fsw * L
    float coilL0 = 0; // retained from coil.conf so logConfig() can re-emit the config line

public:
    SynchronousConverter() : pwmDriver() {
    }

    SynchronousConverter(const SynchronousConverter &) = delete;

    SynchronousConverter &operator=(SynchronousConverter const &) = delete;


    [[nodiscard]] bool boost() const { return isBoost; }

    [[nodiscard]] bool forcedPwm_() const { return forcedPwm; }

    void forcedPwm_(bool forced) { forcedPwm = forced; }

    [[nodiscard]] bool syncRectEnabled_() const { return syncRectEnabled; }

    [[nodiscard]] const uint16_t &pwmMaxDriver() const { return pwmDriver.pwmMax; };

    [[nodiscard]] uint16_t getDtTicks() const {
#if WITH_MCPWM
        return pwmDriver.getDtTicks();
#else
        return 0;
#endif
    }

    [[nodiscard]] bool isEnLogic() const { return pwmEnLogic; }

    [[nodiscard]] uint32_t getPwmFrequency() const { return pwmFrequency; }

    uint16_t pwmCtrlMax{}, pwmRectMin{}, pwmCtrlMin{};
    uint32_t pwmFrequency{};

    [[nodiscard]] bool disabled() const { return pwmCtrl == 0; }

    [[nodiscard]] float getDutyCycle() const { return (float) pwmCtrl / (float) pwmCtrlMax; }

    [[nodiscard]] uint16_t getCtrlOnPwmCnt() const { return pwmCtrl; }

    [[nodiscard]] uint16_t getCtrlOnPwmMin() const { return pwmCtrlMin; }

    [[nodiscard]] uint16_t getRectOnPwmCnt() const { return pwmRect; }

    [[nodiscard]] uint16_t getLowSideOnPwmCnt() const { return isBoost ? pwmCtrl : pwmRect; }

    [[nodiscard]] uint16_t getLowSideMinPwmCnt() const { return isBoost ? 0 : pwmRectMin; }

    [[nodiscard]] uint16_t getRectOnPwmMax() const { return pwmRectMax; }

    [[nodiscard]] uint16_t getRectOnPwmMin() const { return pwmRectMin; }

    [[nodiscard]] int16_t getRectOnOffset() const { return rectOnOffset; }

    void setRectOnOffset(int o) { rectOnOffset = (int16_t) o; } // DCM LS dead-time offset, counts

    // PWM counts per second = fsw * pwmMax (same tick-rate basis as boot_refresh_ns). Used to
    // convert the physical rect_offset_ns <-> comparator counts independent of driver resolution.
    [[nodiscard]] float getPwmTickRate() const { return (float) pwmFrequency * (float) pwmDriver.pwmMax; }

    // Emitted from init() (boot serial) and re-fired once MQTT is up (so it lands in the MQTT/telnet
    // log too — init() runs during setup(), before those sinks exist).
    void logConfig() const {
        ESP_LOGI("converter", "Coil L0=%.1f µH rect_offset=%.0f ns (%d ct)",
                 coilL0 * 1e6f, rectOffsetNsFromCounts(rectOnOffset, getPwmTickRate()), rectOnOffset);
    }

    [[nodiscard]] float voltageRatio() const { return outInVoltageRatio; } // M

    [[nodiscard]] bool manualRect_() const { return manualRect >= 0; }

    // Bench: pin the low-side on-count to `ls`, held against the auto diode-emulation logic
    // (lets you sweep LS timing by hand). `ls<0` restores automatic control. Clamped to
    // [pwmRectMin, complementary max]. NOTE: exceeding the natural zero-crossing draws reverse
    // current - same envelope as forced PWM; protections stay active.
    void setManualRect(int ls) {
        if (ls < 0) {
            manualRect = -1;
            return;
        }
        // -1: cmpLS == pwmMax would land on the timer-wrap, never firing the turn-off event;
        // LS would stay HIGH the whole period.
        manualRect = constrain(ls, (int) pwmRectMin, (int) (pwmDriver.pwmMax - pwmCtrl - 1));
        pwmRect = (uint16_t) manualRect;
#if WITH_MCPWM
        pwmDriver.setLsOff(pwmCtrl + pwmRect);
#else
        if (pwmEnLogic) pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl + pwmRect);
        else pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl, pwmRect);
#endif
    }

    void init(const ConfFile &converterConf, const ConfFile &boardConf, const ConfFile &coilConf) {
        auto topo = converterConf.getString("topo", "buck");
        assert_throw(topo == "buck" or topo == "boost", "");
        isBoost = topo == "boost";
        forcedPwm = converterConf.getByte("forced_pwm", 0);

        if (forcedPwm)
            ESP_LOGW("converter", "%s", "forced_pwm");

        coilL0 = coilConf.getFloat("L0");
        const float L0 = coilL0;
        // rectOnOffset is derived from coil.conf::rect_offset_ns after the driver is up (needs pwmMax
        // for the ns->counts conversion); see the end of init().

        pwmFrequency = boardConf.getLong("pwm_freq"); //39000; //  converter switching frequency
        assert_throw(pwmFrequency > 5e3 && pwmFrequency < 5e5, "");

        fL = (float) pwmFrequency * L0 * InductivityDcBias; // for ripple current computation
        assert_throw(fL < 20, "pwmFreq*L0 out-of-range");
        assert_throw(fL > 1, "pwmFreq*L0 out-of-range");
        // Lo: https://www.ti.com/lit/ds/symlink/lm5163.pdf#page=18

        auto drvInpLogic = boardConf.getString("pwm_driver_logic"); // driver input logic "in,en", "hi,li" and en
        uint8_t pinCtrl, pinRect;


        if (drvInpLogic == "InEn") {
            // e.g. Infineon ir2814

            pwmEnLogic = true;
            auto pnCtrl = isBoost ? "pwm_en" : "pwm_in";
            auto pnRect = isBoost ? "pwm_in" : "pwm_en";

            pinCtrl = boardConf.getByte(pnCtrl);
            pinRect = boardConf.getByte(pnRect);
            assert_throw(pinCtrl != pinRect, "");

            if (!boardConf.getByte("skip_assert", 0)) {
                // ti, infineon gate drivers: in pins pulled low, EN/SD pins pulled high
                assertPinState(pinCtrl, false, pnCtrl, true);
                assertPinState(pinRect, false, pnRect, true);
            }
        } else if (drvInpLogic == "HiLi") {
            // e.g. TI UCC21330x with optional DIS pin (SD pin)
            auto pnCtrl = isBoost ? "pwm_li" : "pwm_hi";
            auto pnRect = isBoost ? "pwm_hi" : "pwm_li";

            pinCtrl = boardConf.getByte(pnCtrl);
            pinRect = boardConf.getByte(pnRect);
            pinSd = boardConf.getByte("pwm_sd", 255); // DIS

            assert_throw(pinCtrl != pinRect, "");
            assert_throw(pinCtrl != pinSd, "");
            assert_throw(pinRect != pinSd, "");

            if (!boardConf.getByte("skip_assert", 0)) {
                assertPinState(pinCtrl, false, pnCtrl, false);
                assertPinState(pinRect, false, pnRect, false);
                if (pinSd != 255) assertPinState(pinSd, true, "pwm_sd", false);
            }
        } else {
            throw std::runtime_error("unrecognized pwm_driver_logic " + drvInpLogic);
        }

#if WITH_MCPWM
        // bestTiming picks the max period_ticks the hardware can give us at pwmFrequency
        // (highest src clock + integer prescaler, 16-bit period cap). See doc/mcpwm-sync-buck-driver.md.
        // rect_offset is stored as a time (rect_offset_ns) and converted to counts below, so it
        // survives the resolution change from LEDC without re-measuring.
        uint32_t resolutionHz = bestTiming(pwmFrequency).resolution_hz;
        // InEn drivers do dead-time in the gate-driver chip, so the MCPWM dt submodule
        // must stay off (pwm_deadtime_ns is documented as ignored for InEn).
        uint32_t dtTicks = pwmEnLogic ? 0u : (uint32_t) std::lround(
            boardConf.getFloat("pwm_deadtime_ns", 0.f) * 1e-9f * (float) resolutionHz);
        pwmDriver.init(0, pwmFrequency, pinCtrl, pinRect, dtTicks, pwmEnLogic);
        uint8_t faultPin = boardConf.getByte("pwm_fault_pin", 255);
        if (faultPin != 255) {
            faultBrake.initGpio(0, faultPin, boardConf.getByte("pwm_fault_active_high", 0));
            faultBrake.bindLeg(pwmDriver.oper(), pwmDriver.genHS(), pwmDriver.genLS());
        }
        pwmDriver.start();
        // Boot latched-low, consistent with the disabled() state (pwmCtrl==0). The first
        // protected pwmPerturb(+) clears the force. Without this, InEn boards (no pwm_sd)
        // would switch the half-bridge before MPPT/protection runs.
        pwmDriver.forceShutdown();
#else
        pwmDriver.init_pwm(pwmCh_Ctrl, pinCtrl, pwmFrequency);
        pwmDriver.init_pwm(pwmCh_Rect, pinRect, pwmFrequency);
#endif

        if (pinSd != 255) {
            pinMode(pinSd, OUTPUT);
            digitalWrite(pinSd, 1);
        }

        // Minimum LS on-time refreshing the HS gate-driver bootstrap cap when the switch node is
        // not otherwise pulled low (high duty, or DCM/zero coil current). Fixed time, not a duty
        // fraction: the recharge need is set by HS gate charge, independent of fsw. During normal
        // conversion the LS body diode refreshes the cap, so this only binds in those corners.
        auto bootRefreshNs = boardConf.getFloat("boot_refresh_ns", 500.f);
        pwmRectMin = isBoost ? 0 : (uint16_t) std::ceil(
                         bootRefreshNs * 1e-9f * (float) pwmFrequency * (float) pwmDriver.pwmMax);
        pwmCtrlMax = (uint16_t) (pwmDriver.pwmMax - pwmRectMin);
        pwmCtrlMin = 1; //isBoost ? 0 : 0;
        // note that mosfets have different Vg(th) and switching times worst case is Vi/o=80/12
        // ^ set pwmMinHS a bit lower than pwmMinLS (might cause no-load output over-voltage otherwise)

        ESP_LOGI("converter", "drv=%s f=%lu boost=%d pwmMax=%hu minLS=%hu minHS=%hu maxHS=%hu",
                 pwmDriver.name, pwmFrequency, isBoost, pwmDriver.pwmMax, pwmRectMin, pwmCtrlMin, pwmCtrlMax);

        // rect_offset_ns is the fixed gate-drive/MOSFET turn-off delay; convert to counts the same
        // way boot_refresh_ns is handled (ns -> counts via the tick rate), so the calibration is
        // invariant to PWM resolution and fsw. Default 0 (no offset) when unset.
        rectOnOffset = (int16_t) rectOffsetCountsFromNs(coilConf.getFloat("rect_offset_ns", 0.f), getPwmTickRate());
        logConfig();
    }

    void computePwmRectMax() {
        // update pwmRectMax for DCM or DCM case
        if (dcmHysteresis) {
            // DCM: the LS turn-off count is the fractional ideal pwmCtrl*ratio quantized to whole
            // PWM ticks. Plain round() stair-steps it, so as duty sweeps the turn-off lands a tick
            // before/after the inductor zero crossing, modulating delivered charge (the duty-pinned
            // SR reverse-current oscillation). First-order error feedback carries the rounding
            // remainder forward so the time-average turn-off tracks the ideal and the beat averages
            // out. Same conservative target (convRatioWCE), so no extra reverse-current bias.
            // rectOnOffset compensates a fixed dead-time/gate-delay between the commanded LS count
            // and the true zero crossing (measure_coil.py --ls-sweep). >0 turns LS off later,
            // recovering body-diode loss but eating reverse-current margin; default 0 = unchanged.
            float ideal = (float) pwmCtrl * pwmRectRatioDCM + (float) rectOnOffset;
            if (rectDither) {
                float v = ideal + rectDitherErr;
                long ls = std::lround(v);
                rectDitherErr = v - (float) ls;
                pwmRectMax = (uint16_t) std::max<long>(0, ls);
            } else {
                pwmRectMax = (uint16_t) std::round(ideal);
            }
        } else {
            // CCM
            pwmRectMax = pwmDriver.pwmMax - pwmCtrl - 1;   // -1: keep cmpLS < period (timer-wrap)
        }
        pwmRectMax = std::min<uint16_t>(std::max(pwmRectMin, pwmRectMax), pwmDriver.pwmMax - pwmCtrl - 1);
    }

    void pwmPerturb(int16_t direction) {
        bool enabling = unlikely(disabled() and direction > 0);
        if (enabling) {
            UART_LOG("Converter enabled");
            if (pinSd != 255) digitalWrite(pinSd, 0);
        }

        pwmCtrl = constrain(pwmCtrl + direction, pwmCtrlMin, pwmCtrlMax);


        bool largerDecrease = (-direction > pwmDriver.pwmMax / 50);

        // update pwmRect block
        if (manualRect >= 0) {
            // bench: hold LS at the requested count (clamped to the complementary max)
            pwmRect = (uint16_t) constrain(manualRect, (int) pwmRectMin,
                                           (int) (pwmDriver.pwmMax - pwmCtrl - 1));
        } else {
            if (largerDecrease && !forcedPwm)
                // assume DCM as it will always give equal or less pwmRectMax
                dcmHysteresis = true;

            computePwmRectMax();

            if (largerDecrease && !forcedPwm) {
                // we don't know if we end up in CCM/DCM, so set rect duty cycle to min
                // let the converter and sensors converge
                if (pwmRect > pwmRectMin + (pwmDriver.pwmMax / 64))
                    UART_LOG("Set pwmLS %hu -> pwmRectMin=%hu\n", pwmRect, pwmRectMin);
                pwmRect = pwmRectMin;
            } else {
                if (pwmRect - pwmRectMax > (pwmDriver.pwmMax / 16)) {
                    // report if rect duty cycle is significantly above upper limit
                    UART_LOG("Set pwmLS %hu -> pwmMaxLS=%hu\n", pwmRect, pwmRectMax);
                }

                // "fade-in" the low-side duty cycle
                // start slowly, then quickly step towards pwmRectMax
                auto step = 1 + ((pwmRect > pwmRectMin + 32) ? (pwmRectMax - pwmRect) / 64 : 0);
                pwmRect = syncRectEnabled
                              ? constrain(pwmRect + step, pwmRectMin, pwmRectMax)
                              : pwmRectMin;
            }
        }


#if WITH_MCPWM
        // Both comparators latch together on TEZ -> order-independent, glitch-free.
        // The largerDecrease/direction ordering dance is only needed for LEDC's two-write update.
        pwmDriver.setHsOff(pwmCtrl);
        pwmDriver.setLsOff(pwmCtrl + pwmRect);
        if (enabling) {
            // Comparators now hold the new (low) duty; release the force latch so the
            // gens resume. Ungated by pinSd: InEn/no-SD boards must clear force too, else
            // they stay latched LOW after the first disable() and can never re-enable.
            pwmDriver.clearForce();
        }
        (void) largerDecrease;
#else
        if (largerDecrease) {
            /*
             * with larger decreases of duty cycle do a "safe" update
             */
            if (pwmEnLogic) {
                pwmDriver.update_pwm(pwmCh_Rect, 0);
                pwmDriver.update_pwm(pwmCh_Ctrl, pwmCtrl);
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl + pwmRect);
            } else {
                if (pinSd != 255) digitalWrite(pinSd, 1);
                else pwmDriver.update_pwm(pwmCh_Rect, 0);
                pwmDriver.update_pwm(pwmCh_Ctrl, pwmCtrl);
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl, pwmRect);
                if (pinSd != 255) digitalWrite(pinSd, 0);
            }
        } else if (direction < 0) {
            /*
             * update EN before IN
             * pwmHS will be smaller than before. after the EN update (we don't know the direction) pwmHS stays at the
             * old value (larger) causing the effective pwmLS to be less, which is ok
             */
            if (pwmEnLogic) {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl + pwmRect);
            } else {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl, pwmRect);
            }
            pwmDriver.update_pwm(pwmCh_Ctrl, pwmCtrl);
        } else {
            /* update IN before EN
             * we increase pwmHS here, so update it first
             */
            pwmDriver.update_pwm(pwmCh_Ctrl, pwmCtrl);
            if (pwmEnLogic) {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl + pwmRect);
            } else {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl, pwmRect);
            }
        }
#endif
    }


    /**
     * Perturb by a fractional step.
     * Instantly perturbs the integer part and accumulates the remainder in a buffer
     *
     * @param directionFloat
     */
    void pwmPerturbFractional(float directionFloat) {
        if (!(std::abs(directionFloat) <= pwmDriver.pwmMax)) {
            ESP_LOGE("buck", "perturbation out of range %f", directionFloat);
            //return;
            throw std::out_of_range("pwmPerturbFractional");
        }
        //assert(std::abs(directionFloat) <= pwmDriver.pwmMax);

        directionFloat += directionFloatBuffer;
        directionFloatBuffer = 0;
        auto directionInt = (int16_t) (directionFloat);
        if (directionInt != 0)
            pwmPerturb(directionInt);
        directionFloatBuffer += directionFloat - (float) directionInt;
    }


    void disable() {
        if (pinSd != 255)digitalWrite(pinSd, 1);
#if WITH_MCPWM
        pwmDriver.forceShutdown();
        // Park the comparators at 0 too: when the gens are later un-forced, the latched
        // pre-disable duty cannot resume for one period before pwmPerturb rewrites them.
        pwmDriver.setHsOff(0);
        pwmDriver.setLsOff(0);
#else
        pwmDriver.stop(pwmCh_Ctrl, 0);
        pwmDriver.stop(pwmCh_Rect, 0);
#endif

        if (pwmCtrl > pwmCtrlMin)
            UART_LOG("PWM disabled (duty cycle was %d)\n", (int) pwmCtrl);

        pwmCtrl = 0;
        pwmRect = 0;
        syncRectEnabled = false;
    }


    [[nodiscard]] inline float rippleCurrent(float hv, float lv) const {
        // this does not consider dc bias, just a constant 0.95 inductivity factor
        return lv / fL * (1.0f - lv / hv);
    }

    /**
     *
     * @param vh higher-voltage side (buck: Vin)
     * @param vl lower-voltage side (buck: Vout)
     * @param il inductor dc current
     */
    [[nodiscard]] bool computeDCM(float vh, float vl, float il) {
        auto ir = rippleCurrent(vh, vl);
        auto dcm = ir > il * (dcmHysteresis ? DcmExitRippleRatio : DcmEnterRippleRatio)
                   || il < DcmForceCurrent;
        if (forcedPwm) dcm = false;
        if (dcm != dcmHysteresis) {
            dcmHysteresis = dcm;
            UART_LOG("converter: %s -> %s (M=%.2f, I=%.2f, ∆I/2=%.2f, pwm=%hu)",
                     dcm ? "CCM" : "DCM", dcm ? "DCM" : "CCM",
                     isBoost ? (vh / vl) : (vl / vh), il, ir * .5f, pwmCtrl);
        }
        return dcm;
    }

    /*!
      * @brief  Compute t_rect/t_ctrl ratio for DCM
      *
      * Used in DCM to limit t_on for the rect switch to prevent reverse coil current (forced PWM).
      * For the buck converter it is t_onLS/t_onHS. (HS=Ctrl)
      * For the boost converter t_onHS/t_onLS. (LS=Ctrl)
      * See doc/Diode Emulation.rst.
      *
      * @param m converter voltage ratio
      * @return pwmRect/pwmCtrl ratio
      */
    [[nodiscard]] float rectCtrlRatio(float m) const {
        return isBoost ? (1.f / (m - 1.f)) : (1.f / m - 1.f);
    }

    [[nodiscard]] bool inDCM() const {
        return dcmHysteresis;
    }

    /**
     * Compute low-side switch duty cycle to emulate a diode for synchronous buck (sensor-less)
     * Prevents reverse current through the LS switch, which would lead to voltage boost and reverse current
     * and can eventually destroy the LS switch.
     * High-voltages at the input can destroy anything connected (including the board itself!)
     *
     * The function does some error estimation and decides whether we are in DCM or CCM mode and limits the LS duty cycle accordingly.
     * See https://www.ti.com/seclit/ug/slyu036/slyu036.pdf#page=19 for more info about sync buck modes and timings
     *
     * by Fabian S. (fl4p) https://github.com/fl4p/fugu-mppt-firmware/
     *
     * @param pwmCtrl Duty cycle of the ctrl switch (buck:HS, boost:LS)
     * @param pwmMax The maximum duty cycle value
     * @param voltageRatio Vout/Vin ratio, D (the greater, the safer but less efficient)
     * @return
     */
    void computeSyncRectRatio(float vh, float vl, float il) {
        // computation of t_onCtrl and t_onRect ratio is quite error sensitive
        // e.g. at VR=0.64 a -5% error causes a 13% deviation of pwmMaxLs !
        // so we do some proper error computation here
        constexpr float voltageMaxErr = 0.01f; // inc -> safer, less efficient

        // WCEF = worst case error factor
        // compute the worst case error (Vout estimated too low, Vin too high => VR estimated too low)
        // this is true for buck and boost
        constexpr float voltageRatioWCEF = (1.f - voltageMaxErr) / (1.f + voltageMaxErr); // < 1.0


        float convRatioWCE =
        (isBoost
             ? ((vh > vl && vl > MinRatioVoltage) ? constrain(vh / vl, MinVoltageRatio, MaxBoostRatio) : MaxBoostRatio) // boost
             : ((vh > vl && vh > MinRatioVoltage) ? constrain(vl / vh, MinVoltageRatio, 1.f - UnityRatioMargin) : 1.0f) //buck
        ) / voltageRatioWCEF;

        // the WCEF division can push M past its physical bound (buck: <1, boost: >1),
        // which would make rectCtrlRatio() negative; clamp back
        convRatioWCE = isBoost ? std::max(convRatioWCE, 1.f + UnityRatioMargin)
                               : std::min(convRatioWCE, 1.f - UnityRatioMargin);

        outInVoltageRatio = convRatioWCE;

        if (computeDCM(vh, vl, il)) {
            if (il < SyncRectOffCurrent || vl < SyncRectOffVoltage) {
                if (pwmRectRatioDCM > 0.2f && pwmRect > pwmRectMin)
                    ESP_LOGI("converter", "Disable sync rect, low I(%.2f)/V(%.2f) pwm=%hu|%hu", il,
                         vl, pwmCtrl, pwmRect);
                pwmRectRatioDCM = 0.0f;
            } else {
                pwmRectRatioDCM = rectCtrlRatio(convRatioWCE);
                //if(dcmHysteresis)pwmRectRatioDCM = min(pwmRectRatioDCM, 1.5f); // TODO
            }
        }
    }
    
    const float &updateSyncRectMaxDuty(float vin, float vout, float il) {
        auto &vh(isBoost ? vout : vin);
        auto &vl(isBoost ? vin : vout);

        computeSyncRectRatio(vh, vl, il);
        if (manualRect >= 0) return outInVoltageRatio; // bench: hold manual LS, skip clamp
        computePwmRectMax();

        if (pwmRect > pwmRectMax) {
            if (pwmRect - pwmRectMax > (pwmDriver.pwmMax / 40)) {
                UART_LOG("Set pwmLS %hu -> pwmMaxLS=%hu (VR=%.3f)", pwmRect, pwmRectMax, outInVoltageRatio);
            }
            pwmRect = pwmRectMax;
#if WITH_MCPWM
            pwmDriver.setLsOff(pwmCtrl + pwmRect); // instantly commit if limit decreases
#else
            if (pwmEnLogic) {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl + pwmRect); // instantly commit if limit decreases
            } else {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl, pwmRect);
            }
#endif
        }

        return outInVoltageRatio;
    }


    /**
     * Permanently set enable/disable low-side switch
     * @param enable
     */
    void enableSyncRect(bool enable, bool overwriteFPWM = false) {
        if (forcedPwm && !overwriteFPWM) enable = true;
        if (enable != syncRectEnabled) {
            UART_LOG("Sync rect %s", enable ? "enabled" : "disabled");
        }
        syncRectEnabled = enable;
        if (!enable)
            syncRectMinDuty();
    }

    /**
     * Set rect sync duty cycle to minimum (buck: disable power conduction, just keep the bootstrapping powered for HS drive)
     * Notice that this is only needed if no inductor is connected to the half-bridge, as the inductor will push the
     * switch node voltage until LS diode conducts.
     * Boost converter has a min duty cycle of 0 (HS)
     */
    void syncRectMinDuty() {
        if (pwmRect > pwmRectMin) {
            if (pwmRect > pwmRectMin + pwmRectMin / 2) {
                ESP_LOGW("dcdc", "set sync-rect PWM to minimum %hu -> %hu (vRatio=%.3f, pwmMaxLS=%hu)", pwmRect,
                         pwmRectMin, outInVoltageRatio, pwmRectMax);
            }
            pwmRect = pwmRectMin;
#if WITH_MCPWM
            pwmDriver.setLsOff(pwmCtrl + pwmRect);
#else
            if (pwmEnLogic) {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl + pwmRect);
            } else {
                pwmDriver.update_pwm(pwmCh_Rect, pwmCtrl, pwmRect);
            }
#endif
        }
    }

    void shortLs() {
        disable();

#if WITH_MCPWM
        // Hold-LS-high diagnostic mode not supported by force_level shutdown.
        // Drop SD if available; otherwise just stays in forceShutdown state.
        ESP_LOGW("buck", "shortLs() is a LEDC-only diagnostic; ignored under WITH_MCPWM");
#else
        auto pwmChLS = boost() ? pwmCh_Ctrl : pwmCh_Rect;
        auto pwmChHS = !boost() ? pwmCh_Ctrl : pwmCh_Rect;
        pwmDriver.stop(pwmChLS, 1);
        pwmDriver.stop(pwmChHS, 0);
#endif
        if (pinSd != 255)digitalWrite(pinSd, 0);
    }

    [[nodiscard]] const uint16_t &pwmCounts()  const { return pwmDriver.pwmMax; }
};
