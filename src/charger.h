#pragma once

#include "freertos/FreeRTOS.h" // portMUX_TYPE (for the _pinMux below)

#include "console.h"
#include "etc/coulomb_counter.h"
#include "etc/linear_glide.h"
#include "etc/mean_accumulator_sync.h"
#include "etc/portmux_guard.h"
#include "tele/mqtt.h"
#include "util.h"

struct BatChargerParams {
    float Vbat_max = NAN; // [V] max bat pack voltage = output voltage
    float Vbat_fallback = NAN; // [V] max bat pack voltage if bms data is n/a
    float Ibat_lim = NAN; // [A] Max bat charge current (Ibat = Iout - Iload)
    float Cbat = NAN; // [Ah] Effective pack capacity. For parallel packs use the summed Ah (e.g. 2P 280Ah → 560).

    float cv_min = NAN; // "float" where the termination line starts @Ibat=0 (LFP: 3.37V)
    float cv_eoc = NAN; // termination line ending (LFP: 3.65V @ Ibat = tail_c_rate * Cbat)
    float tail_c_rate = 0.05f; // [1/h] ratio of EOC tail current to capacity.
    // ^ LFP: 0.05. NCR (Sanyo NCR18650GA, 67mA on 3500mAh): ~0.02. EVE INR18650: 0.033. Higher = safer (terminates later).
    float recharge_dod = 0.20f; // DoD-since-EoC to release termination. LFP  ~0.20. See doc/Termination.md.

    void load(const ConfFile &chargerConf) {
        Vbat_max = chargerConf.getFloat("vout_max", NAN, true);
        cv_eoc = chargerConf.getFloat("cv_eoc", 3.6f);
        cv_min = chargerConf.getFloat("cv_float", 3.37f);
        assert_throw(cv_eoc >= cv_min, "");
        float vout_fallback = floorf(Vbat_max / cv_eoc) * cv_min;
        Vbat_fallback = chargerConf.getFloat("vout_max_fallback", vout_fallback);

        Ibat_lim = chargerConf.getFloat("ibat_max", 20.f, true); // note: iout = ibat + iload
        Cbat = chargerConf.getFloat("bat_c", NAN, true); // larger->"safer" value, doesn't overcharge small bats
        tail_c_rate = chargerConf.getFloat("tail_c_rate", 0.05f);
        recharge_dod = chargerConf.getFloat("recharge_dod", 0.20f);
    }
};

struct BatteryState {
    static constexpr auto VCELL_EXPIRATION_TIME_SEC = 180;

    volatile float vcell_high = 0; // voltage of highest cell reported by BMS
    volatile unsigned long vcell_high_t = 0; // timestamp of highest cell voltage

    EWMA<volatile float, float> vout_avg{60}; // time-averaged pack voltage
    MeanAccumulatorSync ibat_mean{}; // time-averaged battery current (from BMS, MQTT thread → RT consumer)
    MeanAccumulator iout_mean{}; // time-averaged out current (from this charger) TODO currently unused?
    CoulombCounter coulombCounter{}; // Ah-since-last-full tracker, used for recharge hysteresis

    void setVcellHigh(const float &vcell_high_) {
        vcell_high = vcell_high_;
        vcell_high_t = wallClockUs();
    }

    [[nodiscard]] bool haveValidCellVoltage() const {
        return vcell_high > 0 and wallClockUs() - vcell_high_t < (VCELL_EXPIRATION_TIME_SEC * 1000000);
    }

    void updateBatCurrent(const float &ibat) {
        ibat_mean.add(ibat);
        coulombCounter.updateBatCurrent(ibat);
    }

    void update(float vout, float iout) {
        vout_avg.add(vout);
        // note: iout = ibat + iload
        //if (iout > params.Ibat_lim * 0.005f)
        if (isfinite(iout))iout_mean.add(iout);
        else iout_mean.clear();
    }
};

class Li_ChgTerminationCondition {
    /**
     * Charge termination condition for LFP (LiFePo4, Lithium Iron Phosphate) and other (?) Lithium Batteries
     * as described in https://nordkyndesign.com/charging-marine-lithium-battery-banks/
     * also see discussion https://github.com/fl4p/fugu-mppt-firmware/issues/31
     */

    const BatChargerParams &p;
    bool terminated = false;
    float _v_term; // termination cell voltage; single writer (RT thread), atomic 32-bit float store/load

public:
    [[nodiscard]] float v_term() const { return _v_term; }

    explicit operator bool() const { return terminated; }


    explicit Li_ChgTerminationCondition(const BatChargerParams &params)
        : p(params),
          _v_term(params.cv_min) {
    }

    void reset() {
        terminated = false;
        _v_term = p.cv_min;
    }

    bool update(const volatile float &vcell_high, const float &ibat, float ahSinceFull) {
        // Termination line: at ibat = tail_c_rate * Cbat the cell sits at cv_eoc; at ibat = 0 it sits at cv_min.
        // r models the apparent cell resistance implied by that line (for 280Ah / 0.05 → ~20mΩ).
        // See doc/Termination.md.
        float r = (p.cv_eoc - p.cv_min) / (p.tail_c_rate * p.Cbat);
        float vo = ibat * r;
        _v_term = fminf(p.cv_min + fmaxf(0.f, vo), p.cv_eoc); // don't go beyond cv_eoc to avoid BMS cut-off
        if (!terminated and ibat > 0 and vcell_high > p.cv_min + vo) {
            terminated = true;
        } else if (terminated and shouldRelease(vcell_high, ahSinceFull)) {
            terminated = false;
        }
        ESP_LOGI("charger", "termination %hhu (iBat=%.2f, vcHigh=%.3f, vcTerm=%.3f, vcD=%.3f, ahSF=%.2f)",
                 terminated, ibat, vcell_high, _v_term, _v_term - vcell_high, ahSinceFull);
        return terminated;
    }

private:
    [[nodiscard]] bool shouldRelease(const volatile float &vcell_high, float ahSinceFull) const {
        // in case sth is wrong with our coulomb counter we release based on voltage
        constexpr float RECHARGE_VFLOOR_BAND = 0.05f;
        if (vcell_high < p.cv_min - RECHARGE_VFLOOR_BAND) return true;
        if (std::isfinite(p.Cbat) && p.recharge_dod > 0.f
            && ahSinceFull > p.recharge_dod * p.Cbat)
            return true;
        return false;
    }
};

class BatteryCharger {
    float vpack_pin = NAN;
    float ioutLim = NAN;

    // Smooths the OV-feedback pin against per-BMS-message noise. LFP's flat
    // discharge curve means mV-level cell-voltage noise gets multiplied by
    // OV_FEEDBACK_GAIN and would otherwise twitch the setpoint. Span chosen
    // to filter a single outlier without adding more than a few BMS-cycles
    // of latency.
    EWMA_N<4> _vPinFilt{};

    // 5 s linear glide of vpack_pin into Vbat_fallback when BMS data goes stale
    // (avoids a ~1 V step on the converter setpoint).
    LinearGlide _fallbackGlide{5'000'000};

    // Serializes _updatePackVoltagePinning across the MQTT callback (line ~240)
    // and the RT-side loopLF caller (via charger.update). Needed because the
    // EWMA and glide-state mutations are read-modify-write, not atomic.
    portMUX_TYPE _pinMux = portMUX_INITIALIZER_UNLOCKED;

public:
    BatChargerParams params{};
    Li_ChgTerminationCondition termCond{params};
    BatteryState batSt{};


    explicit BatteryCharger() = default;

    void begin(const ConfFile &chargerConf) {
        params.load(chargerConf);
        termCond.reset(); // propagate just-loaded cv_min into termCond's v_term (was NAN at ctor time)
    }


    void _updateTermination() {
        constexpr auto MEAN_NUM = 8;

        // cheap gates first; ibat_mean is the cross-thread one so its
        // gate+pop are folded into a single locked op via tryPop().
        if (batSt.iout_mean.num < MEAN_NUM) return;
        if (!batSt.haveValidCellVoltage()) return;

        float ibat;
        if (!batSt.ibat_mean.tryPop(MEAN_NUM, ibat)) return;
        (void) batSt.iout_mean.pop(); // unused, still pop to keep the accumulator bounded

        bool wasTerm = bool(termCond);
        termCond.update(batSt.vcell_high, ibat, batSt.coulombCounter.ahSinceFull());
        if (!wasTerm && bool(termCond)) {
            // Rising edge — pack is full, re-zero the coulomb counter so the
            // recharge_dod hysteresis measures against this full point.
            batSt.coulombCounter.markFull();
            ESP_LOGI("charger", "Termination latched: ahSinceFull reset to 0");
        }
        ioutLim = NAN;
    }

    void _updatePackVoltagePinning(float vbat = INFINITY) {
        // EOC voltage regulation "pack voltage pinning"
        // once a cell reaches termination voltage we capture pack voltage and set it as max output voltage

        PortMuxGuard _{_pinMux};

        float v_eoc = fmin(params.cv_eoc, termCond.v_term());
        //  ^ v_eoc: we could go beyond cv_eoc if ibat is sufficiently high. however a "dumb" BMS will
        //  cut us off at max 3.65V (or whatever voltage it is configured to), possibly causing a voltage transient
        //  which we like to avoid. so never go beyond cv_eoc

        bool batDataOk = batSt.haveValidCellVoltage() and std::isfinite(params.Cbat);

        if (batDataOk and batSt.vcell_high >= v_eoc) {
            _fallbackGlide.reset();
            constexpr auto OV_FEEDBACK_GAIN = 2; // 4
            float vPin_raw = fmin(batSt.vout_avg.get(), vbat) - (batSt.vcell_high - v_eoc) * OV_FEEDBACK_GAIN;
            _vPinFilt.add(vPin_raw);
            float vPin = _vPinFilt.get();
            if (isnan(vpack_pin) or vPin < vpack_pin - 0.01f)
                ESP_LOGI("charger", "vpPin:=%.3fV (raw=%.3f cvHigh=%.3f v_term=%.3f vbat_avg=%.3f)", vPin, vPin_raw,
                     batSt.vcell_high, v_eoc, batSt.vout_avg.get());
            vpack_pin = vPin;
        } else if (!batDataOk && params.Vbat_fallback >= 0) {
            _vPinFilt.reset();
            if (!_fallbackGlide.active()) {
                // entering fallback — capture current pin as the glide origin
                float from = std::isfinite(vpack_pin) ? vpack_pin : params.Vbat_fallback;
                _fallbackGlide.start(from, params.Vbat_fallback, wallClockUs());
                auto what = !batSt.haveValidCellVoltage() ? "Cell Voltage" : "Pack Capacity";
                ESP_LOGW("charger", "%s n/a, gliding vpPin %.3fV -> %.3fV over %ums",
                         what, from, params.Vbat_fallback,
                         (unsigned)(_fallbackGlide.durationUs() / 1000));
            }
            vpack_pin = _fallbackGlide.value(wallClockUs());
        } else {
            _vPinFilt.reset();
            _fallbackGlide.reset();
            vpack_pin = params.Vbat_max;
        }
    }


    void beginMqtt(const ConfFile &mqttConf) {
        auto topic = mqttConf.getString("cell_voltages_max_topic", "");
        if (!topic.empty()) {
            if (params.Vbat_fallback >= 0)
                vpack_pin = params.Vbat_fallback;
            MQTT.subscribeTopic(topic, [&](const char *dat, int len) {
                batSt.setVcellHigh(strntof(dat, len));
                ESP_LOGD("charger",
                         "avg(vbat)=%.3fV cv_max(mqtt)=%.3fV cv_term=%.3fV vbat_lim=%.3fV vbat_max=%.3fV",
                         batSt.vout_avg.get(),
                         batSt.vcell_high, termCond.v_term(), Vout_max(), params.Vbat_max);
                _updatePackVoltagePinning();
            });
        }

        topic = mqttConf.getString("ibat_topic", "");
        if (!topic.empty())
            MQTT.subscribeTopic(topic, [&](const char *dat, int len) {
                batSt.updateBatCurrent(strntof(dat, len));
            });

        topic = mqttConf.getString("ibat_lim_topic", "");
        if (!topic.empty())
            MQTT.subscribeTopic(topic, [&](const char *dat, int len) {
                float v = strntof(dat, len);
                if (!std::isfinite(v) || v < 0) {
                    LOG_VALUE_IGNORED("charger", "Ibat_lim", len, dat);
                    return;
                }
                auto prev = params.Ibat_lim;
                params.Ibat_lim = v;
                ESP_LOGI("charger", "Ibat_lim= %.3f A (was %.3f)", params.Ibat_lim, prev);
            });
    }

    void update(float vout, float iout) {
        batSt.update(vout, iout);
        _updateTermination();
        _updatePackVoltagePinning();
    }

    [[nodiscard]] float Vout_max() const {
        float v_max = params.Vbat_max;
        if (vpack_pin > 0 and vpack_pin < v_max) v_max = vpack_pin;
        return v_max;
    }

    [[nodiscard]] float Iout_max() const {
        // TODO this should take the acquired ibat - iout delta into account
        // TODO2: is this really necessary?
        // ibat != iout (generally)

        auto lim = params.Ibat_lim;

        // termination mode limit (keep Ibat~0 and supply loads)
        if (isfinite(ioutLim)) lim = min(lim, ioutLim);

        return lim;
    }
};
