#pragma once

#include <atomic>

#include "console.h"
#include "etc/coulomb_counter.h"
#include "etc/linear_glide.h"
#include "tele/mqtt.h"
#include "util.h"

struct BatChargerParams {
    float Vbat_max = NAN; // [V] max bat pack voltage = output voltage
    float Vbat_fallback = NAN; // [V] max bat pack voltage if bms data is n/a
    float Ibat_lim = NAN; // [A] Max bat charge current (Ibat = Iout - Iload)
    float Cbat = NAN; // [Ah] Effective pack capacity. For parallel packs use the summed Ah (e.g. 2P 280Ah → 560).

    float cv_min = NAN; // "float" where the termination line starts @Ibat=0 (LFP: 3.37V, LFP_longevity: 3.325V)
    float cv_eoc = NAN; // termination line ending (LFP_longevity: 3.5V @ Ibat = tail_c_rate * Cbat)
    float tail_c_rate = 0.05f; // [1/h] ratio of EOC tail current to capacity.
    // ^ LFP: 0.05. NCR (Sanyo NCR18650GA, 67mA on 3500mAh): ~0.02. EVE INR18650: 0.033. Higher = safer (terminates later).
    float recharge_dod = 0.20f; // DoD-since-EoC to release termination. LFP  ~0.20. See doc/Termination.md.

    void load(const ConfFile &chargerConf) {
        Vbat_max = chargerConf.getFloat("vout_max", NAN, true);
        cv_eoc = chargerConf.getFloat("cv_eoc", 3.5f);
        cv_min = chargerConf.getFloat("cv_float", 3.325);
        assert_throw(cv_eoc >= cv_min, "");
        auto n_cells = (uint8_t) floorf(Vbat_max / cv_eoc);
        float vout_fallback = n_cells * cv_min;
        Vbat_fallback = chargerConf.getFloat("vout_max_fallback", vout_fallback);
        ESP_LOGI("charger", "N_cells=%u (Vbat_max=%.2f / cv_eoc=%.3f), Vbat_fallback=%.3fV",
                 (unsigned) n_cells, Vbat_max, cv_eoc, Vbat_fallback);

        Ibat_lim = chargerConf.getFloat("ibat_max", 20.f, true); // note: iout = ibat + iload
        Cbat = chargerConf.getFloat("bat_c", NAN, true); // larger->"safer" value, doesn't overcharge small bats
        tail_c_rate = chargerConf.getFloat("tail_c_rate", 0.05f);
        assert_throw(tail_c_rate > 0.f, "tail_c_rate must be > 0");
        recharge_dod = chargerConf.getFloat("recharge_dod", 0.20f);
    }
};

struct BatteryState {
    static constexpr auto VCELL_EXPIRATION_TIME_SEC = 180;
    static constexpr uint16_t IBAT_MIN_SAMPLES = 8; // smoothing warm-up before ibat is published

    volatile float vcell_high = 0; // voltage of highest cell reported by BMS
    volatile unsigned long vcell_high_t = 0; // timestamp of highest cell voltage

    EWMA<volatile float, float> vout_avg{60}; // time-averaged pack voltage
    MeanAccumulator iout_mean{}; // out current (this charger); loop-task only, not shared
    CoulombCounter coulombCounter{}; // Ah-since-last-full tracker, used for recharge hysteresis

    void setVcellHigh(const float &vcell_high_) {
        vcell_high = vcell_high_;
        vcell_high_t = wallClockUs();
    }

    [[nodiscard]] bool haveValidCellVoltage() const {
        return vcell_high > 0 and wallClockUs() - vcell_high_t < (VCELL_EXPIRATION_TIME_SEC * 1000000);
    }

    // producer (MQTT task): smooth ibat here and publish a single lock-free
    // snapshot the loop-task consumer only loads. NAN until IBAT_MIN_SAMPLES seen.
    void updateBatCurrent(const float &ibat) {
        _ibatEwma.add(ibat);
        if (_ibatSamples < IBAT_MIN_SAMPLES) ++_ibatSamples;
        if (_ibatSamples >= IBAT_MIN_SAMPLES)
            _ibatSmoothed.store(_ibatEwma.get(), std::memory_order_relaxed);
        coulombCounter.updateBatCurrent(ibat);
    }

    // consumer (loop task): NAN until smoothing is warm
    [[nodiscard]] float ibatSmoothed() const { return _ibatSmoothed.load(std::memory_order_relaxed); }

    void update(float vout, float iout) {
        vout_avg.add(vout);
        // note: iout = ibat + iload
        //if (iout > params.Ibat_lim * 0.005f)
        if (isfinite(iout))iout_mean.add(iout);
        else iout_mean.clear();
    }

private:
    EWMA<float> _ibatEwma{IBAT_MIN_SAMPLES}; // producer-private smoothing
    uint16_t _ibatSamples = 0; // producer-private warm-up counter
    std::atomic<float> _ibatSmoothed{NAN}; // single writer: producer
};

class Li_ChgTerminationCondition {
    /**
     * Charge termination condition for LFP (LiFePo4, Lithium Iron Phosphate) and other (?) Lithium Batteries
     * as described in https://nordkyndesign.com/charging-marine-lithium-battery-banks/
     * also see discussion https://github.com/fl4p/fugu-mppt-firmware/issues/31
     *
     * when full charged, the battery voltage is pinned. todo regulate battery current towards 0?
     */

    static constexpr uint8_t VFLOOR_STREAK_REQ = 4; // consecutive sub-threshold BMS frames to release on voltage

    const BatChargerParams &p;
    bool terminated = false;
    float _v_term;
    uint8_t _vfloorStreak = 0; // sustained sub-threshold counter (transient I·R sag → 1-frame dips, ignore)

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
        _vfloorStreak = 0;
    }

    bool update(float vcell_high, float ibat, float ahSinceFull) {
        // Termination line: at ibat = tail_c_rate * Cbat the cell sits at cv_eoc; at ibat = 0 it sits at cv_min.
        // r models the apparent cell resistance implied by that line (for 280Ah / 0.05 → ~20mΩ).
        // See doc/Termination.md.
        float r = (p.cv_eoc - p.cv_min) / (p.tail_c_rate * p.Cbat);
        float vo = ibat * r;
        _v_term = fminf(p.cv_min + fmaxf(0.f, vo), p.cv_eoc); // don't go beyond cv_eoc to avoid BMS cut-off
        float iBatFloor = -p.tail_c_rate * p.Cbat * 0.02f;
        if (!terminated and ibat > iBatFloor and vcell_high > p.cv_min + vo) {
            terminated = true;
            _vfloorStreak = 0;
        } else if (terminated and shouldRelease(vcell_high, ahSinceFull)) {
            terminated = false;
        }
        ESP_LOGI("charger", "term %u (iBat=%.2f vcHigh=%.3f vcTerm=%.3f vcD=%.3f ahSF=%.2f)",
                 terminated, ibat, vcell_high, _v_term, _v_term - vcell_high, ahSinceFull);
        return terminated;
    }

private:
    bool shouldRelease(float vcell_high, float ahSinceFull) {
        // Voltage-floor backstop in case the coulomb counter is misbehaving.
        // Require a sustained streak so a single I·R sag (50 A load spike →
        // vcell drops a few hundred mV for one BMS frame) doesn't release a
        // still-near-full pack.
        constexpr float RECHARGE_VFLOOR_BAND = 0.1f;
        if (vcell_high < p.cv_min - RECHARGE_VFLOOR_BAND) {
            if (++_vfloorStreak >= VFLOOR_STREAK_REQ) {
                ESP_LOGW("charger", "Termination release due to vcell_high(%.3f)<%.3f - %.3f (sustained %u frames)",
                         vcell_high, p.cv_min, RECHARGE_VFLOOR_BAND, (unsigned) _vfloorStreak);
                _vfloorStreak = 0;
                return true;
            }
        } else {
            _vfloorStreak = 0;
        }
        if (std::isfinite(p.Cbat) && p.recharge_dod > 0.f && ahSinceFull > p.recharge_dod * p.Cbat) {
            ESP_LOGW("charger", "Termination release due to DoD: ahSinceFull(%.2f)>%.2f Ah (recharge_dod=%.2f)",
                     ahSinceFull, p.recharge_dod * p.Cbat, p.recharge_dod);
            return true;
        }
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

    // 5 s linear glide of vpack_pin on termination transitions. Rising edge:
    // current pin → Vbat_fallback (LFP "float" voltage ≈ N_cells × cv_min) so
    // the converter holds the pack at a fixed reference and supplies the load
    // (i_bat ≈ 0). Falling edge: Vbat_fallback → Vbat_max so the recharge ramp
    // doesn't step the converter setpoint.
    LinearGlide _floatGlide{5'000'000};
    bool _wasTerminated = false;

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

        // cheap gates first; iout_mean paces this (loop-task only). ibat is the
        // cross-task value but it's a plain lock-free atomic load now.
        if (batSt.iout_mean.num < MEAN_NUM) return;
        if (!batSt.haveValidCellVoltage()) return;

        float ibat = batSt.ibatSmoothed();
        if (!std::isfinite(ibat)) return; // smoothing not warm yet
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
        // Pack-voltage-pinning regimes (first match wins):
        //   1) EOC feedback — cells above v_eoc: closed loop on vcell_high → v_eoc.
        //      Doubles as the over-target corrector during terminated float; pulls
        //      vpack_pin down until vcell_high reaches v_eoc (≈ cv_min at ibat≈0),
        //      self-correcting any Vout-ADC offset. Suppressed when balancingMode
        //      is on, to let a passive balancer act at a fixed float voltage
        //      (unhealthy for an extended period of time, so only when explicitly
        //      requested).
        //   2) Terminated float — open-loop hold at Vbat_fallback.
        //   3) BMS-stale — glide to Vbat_fallback.
        //   4) Bulk — push to Vbat_max (or ride the falling-edge glide).
        // Termination transitions are glided to avoid stepping the setpoint.

        bool balancingMode = false;

        // never go beyond cv_eoc — a "dumb" BMS may cut us off at 3.65V and cause
        // a voltage transient
        float v_eoc = fmin(params.cv_eoc, termCond.v_term());

        bool batDataOk = batSt.haveValidCellVoltage() and std::isfinite(params.Cbat);
        bool nowTerm = bool(termCond);
        auto nowUs = wallClockUs();

        if (nowTerm != _wasTerminated) {
            float from = std::isfinite(vpack_pin) ? vpack_pin : params.Vbat_fallback;
            float to = nowTerm ? params.Vbat_fallback : params.Vbat_max;
            _floatGlide.start(from, to, nowUs);
            ESP_LOGI("charger", "Term %s, gliding vpPin %.3fV -> %.3fV over %ums",
                     nowTerm ? "latched" : "released", from, to,
                     (unsigned) (_floatGlide.durationUs() / 1000));
            _wasTerminated = nowTerm;
        }

        bool eocFeedback = batDataOk && batSt.vcell_high >= v_eoc && !(balancingMode && nowTerm);

        if (eocFeedback) {
            // battery is full
            _fallbackGlide.reset();
            _floatGlide.reset();
            constexpr auto OV_FEEDBACK_GAIN = 4;
            float vPin_raw = fmin(batSt.vout_avg.get(), vbat) - (batSt.vcell_high - v_eoc) * OV_FEEDBACK_GAIN;
            _vPinFilt.add(vPin_raw);
            float vPin = _vPinFilt.get();
            if (isnan(vpack_pin) or vPin < vpack_pin - 0.01f)
                ESP_LOGI("charger", "update vpPin:=%.3fV (raw=%.3f cvHigh=%.3f v_term=%.3f vbat_avg=%.3f)",
                         vPin, vPin_raw, batSt.vcell_high, v_eoc, batSt.vout_avg.get());
            vpack_pin = vPin;
        } else if (nowTerm && batDataOk) {
            // balancing mode / float
            // terminated float: hold an absolute target, ignore feedback/EWMA
            // this keep LFP float, with a small charge current, considered unhealthy for an extended period of time
            _vPinFilt.reset();
            _fallbackGlide.reset();
            vpack_pin = _floatGlide.value(nowUs);
        } else if (!batDataOk && params.Vbat_fallback >= 0) {
            // missing bat data, and we have a fallback -> glide there
            _vPinFilt.reset();
            _floatGlide.reset();
            if (!_fallbackGlide.active()) {
                // entering fallback — capture current pin as the glide origin
                float from = std::isfinite(vpack_pin) ? vpack_pin : params.Vbat_fallback;
                _fallbackGlide.start(from, params.Vbat_fallback, nowUs);
                auto what = !batSt.haveValidCellVoltage() ? "Cell Voltage" : "Pack Capacity";
                ESP_LOGW("charger", "%s n/a, gliding vpPin %.3fV -> %.3fV over %ums",
                         what, from, params.Vbat_fallback,
                         (unsigned) (_fallbackGlide.durationUs() / 1000));
            }
            vpack_pin = _fallbackGlide.value(nowUs);
        } else {
            // bulk charging or (missing batData and no fallback)
            _vPinFilt.reset();
            _fallbackGlide.reset();
            // ride the falling-edge glide if it's still ramping, otherwise jump
            vpack_pin = _floatGlide.active() ? _floatGlide.value(nowUs) : params.Vbat_max;
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
