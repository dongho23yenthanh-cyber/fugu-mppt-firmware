#include "cli.h"

#include <Arduino.h>
#ifdef WITH_NETW
#include <WiFi.h>
#endif
#include <cmath>
#include <esp_timer.h>
#include <SimpleCLI.h>

#include "logging.h"
#include "conf.h"
#include "util.h"
#include "buck.h"
#include "mppt.h"
#include "measure_coil.h"
#include "etc/version.h"
#include "adc/sampling.h"
#include "service.h"
#include "viz/led.h"
#include "storage/key-value.h"
#ifdef WITH_NETW
#include "tele/telemetry.h"     // connect_wifi_async, add_ap
#include "etc/ota.h"            // doOta
#endif
#include "etc/ota_ble.h"        // otaBleBegin/End/Abort (OTA push over BLE)
#if WITH_VCONV
#include "sim/vconv.h"
#endif

void print_real_time_stats_1s_task(void *);

// Globals owned by main.cpp that the command handlers act on. Declared extern here (rather than in
// a shared header) because they're implementation coupling between main.cpp and this file, not a
// public interface. Types come from the includes above, so these match main.cpp's definitions.
extern bool manualPwm;
#ifdef WITH_NETW
extern bool disableWifi;
extern uint32_t wifiReenableMs;
#endif
extern SynchronousConverter converter;
extern MpptController mppt;
extern ADC_Sampler adcSampler;
extern VIinVout<const Sensor *> sensors;
extern LedIndicator led;
extern KeyValueStorage nvs;
extern unsigned long maxLoopLag;
#if CAPTURE_LOOP_DT
extern unsigned long maxLoopDT;
#endif

#define WITH_PWM_DIAGNOSTICS 1

// Defined in main.cpp (non-static so we can reach them from here).
void systemRestart();

void stopAndBackoff(uint32_t secondsDelay);

void loopLF(const unsigned long &nowUs);

static SimpleCLI cli;

// --- Console command callbacks ---------------------------------------------------------------
// use CMD_FAIL() in command dispatchers to flag that command as failed
// SimpleCLI's dispatch API has no return value

static bool s_cmdFailed = false;
#define CMD_FAIL_RETURN(...) do { ESP_LOGW("main", __VA_ARGS__); s_cmdFailed = true; return; } while (0)

static void cmdSync(cmd *c) {
    if (!manualPwm)
        CMD_FAIL_RETURN("sync: only in manual PWM (use 'dc N' first)");
    auto arg = Command(c).getArg(0).getValue();
    auto on = arg == "on" or arg == "1";
    if (on or arg == "off" or arg == "0") {
        converter.forcedPwm_(false);
        converter.enableSyncRect(on, true);
    } else if (arg == "forced") {
        converter.enableSyncRect(true);
        converter.forcedPwm_(true);
    } else {
        CMD_FAIL_RETURN("sync: expected on|off|forced");
    }
}

static void cmdBflow(cmd *c) {
    if (!manualPwm)
        CMD_FAIL_RETURN("bf: only in manual PWM (use 'dc N' first)");
    if (!mppt.bflow)
        CMD_FAIL_RETURN("panel switch not configured");
    auto newState = Command(c).getArg(0).getValue().toInt();
    if (newState != 0 and newState != 1)
        CMD_FAIL_RETURN("bf: expected 0|1");
    if (mppt.bflow.state() != newState)
        ESP_LOGI("main", "Set bflow state %i", (int) newState);
    mppt.bflow.enable(newState);
}

static void cmdRestart(cmd *) { systemRestart(); }

static void cmdMppt(cmd *) {
    if (!manualPwm)
        CMD_FAIL_RETURN("MPPT already enabled");
    ESP_LOGI("main", "MPPT re-enabled");
    converter.setManualRect(-1); // drop any bench LS hold
    manualPwm = false;
}

// dc <hs> [ls]  — manual PWM. With no [ls] the low side is automatic (diode emulation); with
// [ls] the low-side on-count is pinned to that value (bench LS-timing sweep). ls<0 -> auto.

static void cmdDc(cmd *c) {
    if (adcSampler.isCalibrating() || isMeasuring())
        CMD_FAIL_RETURN("dc: busy calibrating");
    if (isMeasuring())
        CMD_FAIL_RETURN("dc: busy measuring");
    Command cc(c);
    auto v = cc.getArg(0).getValue();
    if (v.length() == 0)
        CMD_FAIL_RETURN("dc: expected <hs> [ls]");
    auto dc = v.toInt();
    if (dc < 0 || dc > converter.pwmCtrlMax || v.indexOf(',') != -1)
        CMD_FAIL_RETURN("dc: out of range [0,%i]", (int) converter.pwmCtrlMax);

    if (!manualPwm || converter.disabled()) {
        ESP_LOGI("main", "Switched to manual PWM");
        if (dc != 0 && !mppt.limits.reverse_current_paranoia) {
            converter.enableSyncRect(true);
            mppt.bflow.enable(true);
        }
    }
    manualPwm = true;
    mppt.setTargetDutyCycle(dc);

    if (cc.countArgs() >= 2 && dc > 0) {
        int ls = cc.getArg(1).getValue().toInt();
        converter.setManualRect(ls);
        if (ls >= 0)
            UART_LOG("Manual LS=%i held (HS=%i); reverse-current risk, bench only", ls, (int) dc);
    } else {
        converter.setManualRect(-1); // auto LS
    }
}

static void cmdShortLs(cmd *) {
    if (converter.boost() && abs(sensors.Vin->ewm.avg.get()) < 0.05) {
        manualPwm = true;
        converter.shortLs();
    } else {
        CMD_FAIL_RETURN("short-ls: requires boost mode and Vin~0");
    }
}

static void cmdSpeed(cmd *c) {
    float speedScale = Command(c).getArg(0).getValue().toFloat();
    if (speedScale >= 0 && speedScale < 10) {
        mppt.speedScale = speedScale;
        ESP_LOGI("main", "Set tracker speed scale %.4f", speedScale);
    } else {
        CMD_FAIL_RETURN("speed: out of range [0,10)");
    }
}

static void cmdFan(cmd *c) {
    if (!mppt.fan.fanSet(Command(c).getArg(0).getValue().toFloat() * 0.01f))
        CMD_FAIL_RETURN("fan: set failed");
}

static void cmdLed(cmd *c) { led.setRGB(Command(c).getArg(0).getValue().c_str()); }

// Bench-only: `gpio <pin> <0|1>` -- direct digitalWrite test. Bypasses MCPWM/LEDC.
static void cmdGpio(cmd *c) {
    Command cc(c);
    auto pin = (uint8_t) cc.getArg(0).getValue().toInt();
    auto val = (uint8_t) cc.getArg(1).getValue().toInt();
    pinMode(pin, OUTPUT);
    digitalWrite(pin, val);
    UART_LOG("gpio %u -> %u", (unsigned) pin, (unsigned) val);
}

// Bench-only: `mcpwmtest <pin>` -- minimal IDF MCPWM example pattern on a single pin.
// 1 kHz 50% duty, no dead-time, no fault. Verifies the MCPWM peripheral itself.
#include "driver/mcpwm_prelude.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/mcpwm_reg.h"

static void cmdMcpwmTest(cmd *c) {
    int pin = Command(c).getArg(0).getValue().toInt();
    static mcpwm_timer_handle_t timer = nullptr;
    static mcpwm_oper_handle_t oper = nullptr;
    static mcpwm_cmpr_handle_t cmp = nullptr;
    static mcpwm_gen_handle_t gen = nullptr;
    if (timer) {
        CMD_FAIL_RETURN("already created; reboot to re-test");
        return;
    }
    mcpwm_timer_config_t tc = {
        .group_id = 0, .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = 80'000'000, .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = 2048, .intr_priority = 0, .flags = {}
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&tc, &timer));
    mcpwm_operator_config_t oc = {.group_id = 0, .intr_priority = 0, .flags = {}};
    ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &oper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper, timer));
    mcpwm_comparator_config_t cc2 = {.intr_priority = 0, .flags = {}};
    ESP_ERROR_CHECK(mcpwm_new_comparator(oper, &cc2, &cmp));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmp, 1024));
    mcpwm_generator_config_t gc = {.gen_gpio_num = pin, .flags = {}};
    ESP_ERROR_CHECK(mcpwm_new_generator(oper, &gc, &gen));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmp, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
    UART_LOG("mcpwmtest: 1kHz 50%% on GPIO %d (group 1, isolated from buck driver)", pin);
}

static void cmdSweep(cmd *) { mppt.startSweep(); }

static void cmdResetLag(cmd *) {
    maxLoopLag = 0;
#if CAPTURE_LOOP_DT
    maxLoopDT = 0;
#endif
    rtcount_print(true);
}

#ifdef WITH_NETW
static void cmdWifi(cmd *c) {
    Command cc(c);
    auto arg = cc.getArg(0).getValue();
    if (arg == "on") {
        wifiReenableMs = 0;
        disableWifi = false;
        connect_wifi_async();
    } else if (arg == "off") {
        // "off <minutes>" disables temporarily and keeps the saved ssid for reconnect;
        // bare "off" disables for good and forgets the sticky ssid.
        long mins = cc.countArgs() >= 2 ? cc.getArg(1).getValue().toInt() : 0;
        disconnect_wifi(true);
        disableWifi = true;
        if (mins > 0) {
            wifiReenableMs = wallClockMs() + (uint32_t) mins * 60000;
            UART_LOG("WiFi off for %ld min", mins);
        } else {
            wifiReenableMs = 0;
            nvs.open();
            if (!nvs.readString("wifi_ssid", "").empty())
                nvs.writeString("wifi_ssid", "");
            nvs.close();
        }
    } else {
        CMD_FAIL_RETURN("wifi: expected on|off [minutes]");
    }
}

static void cmdWifiAdd(cmd *c) {
    auto ssidAndPw = Command(c).getArg(0).getValue();
    auto i = ssidAndPw.indexOf(':');
    if (i <= 0)
        CMD_FAIL_RETURN("wifi-add: expected ssid:password");
    std::string ssid = ssidAndPw.substring(0, i).c_str();
    auto psk = ssidAndPw.substring(i + 1);
    ESP_LOGI("main", "adding wifi network %s (psk=%s)", ssid.c_str(), psk.c_str());
    add_ap(ssid, psk.c_str());
}
#endif

static void cmdScanI2c(cmd *) { scan_i2c(); }

static void cmdLs(cmd *) { ESP_LOGE("main", "not impl"); }

#ifdef WITH_NETW
static void cmdOta(cmd *c) {
    auto url = Command(c).getArg(0).getValue();
    if (url.length() == 0)
        CMD_FAIL_RETURN("ota: expected url");
    stopAndBackoff(10);
    adcSampler.halted = true; // disable ADC reading
    doOta(url.c_str());
    adcSampler.halted = false;
}
#endif


#ifdef WITH_BLE
// otab begin <size> <sha256hex> | end | abort  — OTA firmware push over BLE (no WiFi). `begin` arms the
// receiver (halts the converter, erases the passive partition); the host then streams the image to the
// NUS FW characteristic; `end` verifies the SHA-256 and reboots. ADC halt/restore lives inside otaBle*.
static void cmdOtaBle(cmd *c) {
    Command cc(c);
    auto sub = cc.getArg(0).getValue();
    if (sub == "begin") {
        if (cc.countArgs() < 3)
            CMD_FAIL_RETURN("otab: begin <size> <sha256hex>");
        long size = cc.getArg(1).getValue().toInt();
        auto sha = cc.getArg(2).getValue();
        if (size <= 0 || sha.length() != 64)
            CMD_FAIL_RETURN("otab: bad size/sha");
        if (!otaBleBegin((uint32_t) size, sha.c_str()))
            CMD_FAIL_RETURN("otab: begin rejected");
    } else if (sub == "end") {
        if (!otaBleEnd())
            CMD_FAIL_RETURN("otab: end failed"); // on success this reboots and never returns
    } else if (sub == "abort") {
        otaBleAbort();
    } else {
        CMD_FAIL_RETURN("otab: expected begin|end|abort");
    }
}
#endif

static void cmdRtStats(cmd *) {
    xTaskCreatePinnedToCore(print_real_time_stats_1s_task, "rtstats", 4096, NULL, 1, NULL, NON_RT_CORE /*core*/);
}

// monotonic seconds since boot; resets only on reboot (unlike status N, which zeroes on each sweep)
static void cmdUptime(cmd *) {
    UART_LOG("Uptime: %lu s", (uint32_t) (esp_timer_get_time() / 1000000));
    UART_LOG("App: %s", format_version());
}

static void cmdMem(cmd *) {
    UART_LOG("Total heap:  %9ld", ESP.getHeapSize());
    UART_LOG("Free heap:   %9ld", ESP.getFreeHeap());
    UART_LOG("Total PSRAM: %9ld", ESP.getPsramSize());
    UART_LOG("Free PSRAM:  %9ld", ESP.getFreePsram());
}

static void cmdSensor(cmd *c) {
    if (Command(c).countArgs() >= 1) {
        // `sensor avg`: one compact line of EWM averages, for fast polling
        char line[160];
        int n = 0;
        for (auto s: adcSampler.sensors) {
            if (n < 0 || n >= (int) sizeof(line)) break;
            n += snprintf(line + n, sizeof(line) - n, "%s=%.4f ", s->params.teleName.c_str(), s->ewm.avg.get());
        }
        UART_LOG("sens: %s", line);
        return;
    }
    for (auto s: adcSampler.sensors) {
        auto u = s->params.unit;
        UART_LOG("\nSensor `%s` (ch%d, %s):", s->params.teleName.c_str(), s->params.adcCh,
                 s->isVirtual ? "virtual" : "physical");
        UART_LOG("  num=%6lu  last=%7.3f %c   prev=%7.3f %c  raw=%8.4f", s->numSamples, s->last, u, s->previous, u,
                 s->lastRaw);
        UART_LOG("  EWM(%4lu):  avg= %7.3f %c   std*=%7.4f %c  std%%=%7.3f %%", s->ewm.span(),
                 s->ewm.avg.get(), u,
                 sqrt(s->ewm.std.get()) * abs(s->ewm.avg.get()), u,
                 sqrt(s->ewm.std.get()) * 100.f);
        UART_LOG("  ANF(span=%4.0f):  Nstd= %7.3f   Sstd=%7.3f   NSR=%7.3f", s->anf.span,
                 sqrt(s->anf.ewmN.nvar()) * 100.0f, sqrt(s->anf.ewmS.nvar()) * 100.0f,
                 sqrt(s->anf.ewmN.nvar() / s->anf.ewmS.nvar()));
    }
}

#ifdef WITH_NETW
static void cmdIp(cmd *) { UART_LOG("Local IP Address: %s", WiFi.localIP().toString().c_str()); }
#endif

static void cmdAdcRestart(cmd *) { adcSampler.reInitADCs(); }

static void cmdAdcReset(cmd *) { adcSampler.resetPeripherals(); }

// hostname [name]  — no arg prints the current hostname; an arg sets it.
static void cmdHostname(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 1 || cc.getArg(0).getValue().length() == 0) {
#ifdef WITH_NETW
        UART_LOG("Hostname: %s", getHostname().c_str());
#else
        nvs.open();
        UART_LOG("Hostname: %s", nvs.readString("hostname", "fugu").c_str());
        nvs.close();
#endif
        return;
    }
    auto hn = cc.getArg(0).getValue();
    nvs.open();
    nvs.writeString("hostname", hn.c_str());
    nvs.close();
}

std::string confFile(const std::string &c) {
    auto path = "/littlefs/conf/" + c;
    if (!c.ends_with(".conf")) path += ".conf";
    return path;
}

std::string confFile(const String &c) {
    return confFile(std::string(c.c_str()));
}


// set-config <file> <key> <value...>  — value may contain spaces, so join the trailing tokens.
//   set-config coil.conf L0 50            set-config mqtt.conf broker_uri mqtt://192.168.1.134:1882
//   set-config limits.conf iout_max 35    set-config charger.conf cell_voltage_eoc 3.53
static void cmdSetConfig(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 3)
        CMD_FAIL_RETURN("set-config: expected <file> <key> <value>");
    auto fn = confFile(cc.getArg(0).getValue());
    auto key = cc.getArg(1).getValue();
    String val = cc.getArg(2).getValue();
    for (int i = 3; i < cc.countArgs(); ++i) val += " " + cc.getArg(i).getValue();
    ConfFile conf{fn.c_str()};
    auto oldVal = conf.getString(key.c_str(), "");
    ESP_LOGI("main", "Setting conf '%s:%s' = '%s' (was %s)", fn.c_str(), key.c_str(), val.c_str(), oldVal.c_str());
    if (oldVal != val.c_str())
        conf.add({{key.c_str(), val.c_str()}}, true);
}


// del-config <file> <key>  — remove a key; the whole line (incl. inline comment) is deleted.
static void cmdDelConfig(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 2)
        CMD_FAIL_RETURN("del-config: expected <file> <key>");
    auto fn = confFile(cc.getArg(0).getValue());
    auto key = cc.getArg(1).getValue();
    ConfFile conf{fn.c_str()};
    if (conf.remove(key.c_str()))
        ESP_LOGI("main", "Deleted conf '%s:%s'", fn.c_str(), key.c_str());
    else
        CMD_FAIL_RETURN("del-config: key '%s' not found in %s", key.c_str(), fn.c_str());
}

// get-config <file> [key]  — print one key, or dump the whole file.
static void cmdGetConfig(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 1)
        CMD_FAIL_RETURN("get-config: expected <file> [key]");
    auto fn = confFile(cc.getArg(0).getValue());
    ConfFile conf{fn.c_str()};
    if (cc.countArgs() >= 2) {
        auto key = cc.getArg(1).getValue();
        auto val = conf.getString(key.c_str(), "");
        UART_LOG("Conf '%s:%s' = '%s'", fn.c_str(), key.c_str(), val.c_str());
    } else {
        for (const auto &key: conf.keys())
            ESP_LOGI("main", "Conf '%s:%s' = '%s'", fn.c_str(), key.c_str(), conf.getString(key).c_str());
    }
}

static void cmdVset(cmd *c) {
    float v = Command(c).getArg(0).getValue().toFloat();
    if (v >= 0 and v <= 999) mppt.charger.params.Vbat_max = v;
    else
        CMD_FAIL_RETURN("vset: out of range [0,999]");
}

static void cmdIset(cmd *c) {
    float v = Command(c).getArg(0).getValue().toFloat();
    if (v >= 0 and v <= 999) mppt.charger.params.Ibat_lim = v;
    else
        CMD_FAIL_RETURN("iset: out of range [0,999]");
}

// svc [list]                       svc on|off|restart|rs <name>
// svc log <name> <error|warn|info>
static void cmdService(cmd *c) {
    Command cc(c);
    int n = cc.countArgs();
    String sub = n >= 1 ? cc.getArg(0).getValue() : String("list");

    if (sub == "list") {
        UART_LOG("%-10s %-8s %-6s %-8s %s", "NAME", "STATE", "LOG", "ENABLED", "DETAIL");
        for (auto *s: g_services.all()) {
            auto detail = s->statusDetail();
            auto st = s->state();
            const char *color = st == ServiceState::Running
                                    ? "\x1b[32m" // green
                                    : st == ServiceState::Failed
                                          ? "\x1b[31m" // red
                                          : "\x1b[90m"; // gray
            char state[24];
            snprintf(state, sizeof(state), "%s%-8s\x1b[0m", color, stateStr(st));
            UART_LOG("%-10s %s %-6s %-8s %s", s->name(), state,
                     levelToStr(s->logLevel()), s->enabled() ? "yes" : "no", detail.c_str());
        }
        return;
    }

    if (n < 2)
        CMD_FAIL_RETURN("svc: expected <name>");
    auto name = cc.getArg(1).getValue();
    auto *s = g_services.findByName(name.c_str());
    if (!s)
        CMD_FAIL_RETURN("svc: unknown '%s'", name.c_str());

    if (sub == "on") {
        s->setEnabledPersist(true);
        if (!s->start()) UART_LOG("start failed (state=%s)", stateStr(s->state()));
    } else if (sub == "off") {
        s->setEnabledPersist(false);
        s->stop();
    } else if (sub == "restart" || sub == "rs") {
        s->restart();
    } else if (sub == "log") {
        if (n < 3)
            CMD_FAIL_RETURN("svc log: expected <error|warn|info>");
        s->setLogLevel(strToLevel(cc.getArg(2).getValue().c_str()), /*persist*/ true);
    } else {
        CMD_FAIL_RETURN("svc: unknown subcommand '%s'", sub.c_str());
    }
}

// measure-coil l0|ls [steps|hs] [dwell_ms] [apply]  — the sweep logic lives in measure_coil.cpp.
static void cmdMeasureCoil(cmd *c) {
    if (adcSampler.isCalibrating())
        CMD_FAIL_RETURN("measure-coil: busy calibrating");
    Command cc(c);
    auto mode = cc.getArg(0).getValue();
    bool ls;
    if (mode == "l0") ls = false;
    else if (mode == "ls") ls = true;
    else
        CMD_FAIL_RETURN("measure-coil: expected l0|ls [args]");

    int nArgs = cc.countArgs();
    bool apply = nArgs >= 2 && cc.getArg(nArgs - 1).getValue() == "apply";
    int numArgs = apply ? nArgs - 1 : nArgs; // positional count excluding trailing 'apply'
    int arg1 = numArgs >= 2 ? cc.getArg(1).getValue().toInt() : 0;
    uint32_t dwellMs = numArgs >= 3 ? (uint32_t) cc.getArg(2).getValue().toInt() : 3000;
    if (dwellMs < 200) dwellMs = 200;
    if (!measureCoilStart(ls, apply, arg1, dwellMs))
        CMD_FAIL_RETURN("measure-coil: already running");
}

#if WITH_VCONV
// vconv                           dump state
// vconv pv <isc> <voc> [k]        update PV params
// vconv bat <vbat>                update battery voltage
// vconv set <key> <value>         in-memory setter (c_in, c_out, r_bat, l, vbat_ac_amp, vbat_ac_freq)
static void cmdVconv(cmd *c) {
    Command cc(c);
    int n = cc.countArgs();
    if (n == 0 || cc.getArg(0).getValue().length() == 0) {
        const auto &p = g_vconv.getPwm();
        UART_LOG("vconv: Vin=%.3fV Vout=%.3fV IL=%.3fA Iin=%.3fA Iout=%.3fA %s pwm[ctrl=%u rect=%u max=%u f=%uHz]%s",
                 g_vconv.getVin(), g_vconv.getVout(), g_vconv.getIL(),
                 g_vconv.getIinAvg(), g_vconv.getIoutAvg(),
                 g_vconv.inDcm() ? "DCM" : "CCM",
                 (unsigned) p.pwmCtrl, (unsigned) p.pwmRect, (unsigned) p.pwmMax,
                 (unsigned) p.pwmFreq,
                 g_vconv.errored() ? " ERR" : "");
        return;
    }
    auto sub = cc.getArg(0).getValue();
    if (sub == "pv") {
        if (n < 3) CMD_FAIL_RETURN("vconv pv: expected <isc> <voc> [k]");
        float isc = cc.getArg(1).getValue().toFloat();
        float voc = cc.getArg(2).getValue().toFloat();
        float k   = n >= 4 ? cc.getArg(3).getValue().toFloat() : g_vconv.getPvK();
        if (isc <= 0 || voc <= 0 || k <= 0 || k >= 1)
            CMD_FAIL_RETURN("vconv pv: bad args (isc>0, voc>0, 0<k<1)");
        g_vconv.setPv(isc, voc, k);
        UART_LOG("vconv: PV set Isc=%.2f Voc=%.2f k=%.2f", isc, voc, k);
    } else if (sub == "bat") {
        if (n < 2) CMD_FAIL_RETURN("vconv bat: expected <v|open|short>");
        auto arg1 = cc.getArg(1).getValue();
        if (arg1 == "open") {
            // Open-circuit output: huge series R, V_bat=0. I_bat≈0 so V_out
            // integrates whatever the converter delivers (model caps at 2·Voc;
            // firmware OVP should trip first).
            g_vconv.setBat(0.0f, 1e9f);
            UART_LOG("vconv: bat open-circuit (v_bat=0, r_bat=1e9)");
        } else if (arg1 == "short") {
            // Hard short on the output: V_bat=0, tiny R_bat. Backward-Euler on the
            // V_out node makes this stable at arbitrary r_bat — V_out collapses to
            // ~iOutAvg·rbat, the firmware should see Iout climb and trip OCP.
            g_vconv.setBat(0.0f, 1e-3f);
            UART_LOG("vconv: bat short-circuit (v_bat=0, r_bat=1e-3)");
        } else {
            float v = arg1.toFloat();
            if (v <= 0 || v > 200) CMD_FAIL_RETURN("vconv bat: out of range");
            g_vconv.setBat(v, g_vconv.getRbat());
            UART_LOG("vconv: Vbat=%.2f", v);
        }
    } else if (sub == "set") {
        if (n < 3) CMD_FAIL_RETURN("vconv set: expected <key> <value>");
        auto key = cc.getArg(1).getValue();
        float v  = cc.getArg(2).getValue().toFloat();
        if (key == "c_in")          g_vconv.setPassives(v, g_vconv.getCout(), g_vconv.getL());
        else if (key == "c_out")    g_vconv.setPassives(g_vconv.getCin(), v, g_vconv.getL());
        else if (key == "l")        g_vconv.setPassives(g_vconv.getCin(), g_vconv.getCout(), v);
        else if (key == "r_bat")    g_vconv.setBat(g_vconv.getVbat(), v);
        else if (key == "vbat_ac_amp")  g_vconv.setBatRipple(v, g_vconv.getVbatAcFreq());
        else if (key == "vbat_ac_freq") g_vconv.setBatRipple(g_vconv.getVbatAcAmp(), v);
        else CMD_FAIL_RETURN("vconv set: unknown key '%s'", key.c_str());
        UART_LOG("vconv: %s=%.6g", key.c_str(), v);
    } else {
        CMD_FAIL_RETURN("vconv: expected pv|bat|set");
    }
}
#endif

static void cmdHelp(cmd *) { UART_LOG("%s", cli.toString().c_str()); }

void setupCli() {
    cli.addCommand("help,?", cmdHelp);
    cli.addCommand("restart,reset,reboot", cmdRestart);
    cli.addCommand("mppt", cmdMppt);
    cli.addCommand("short-ls", cmdShortLs);
    cli.addCommand("sweep", cmdSweep);
    cli.addCommand("reset-lag", cmdResetLag);
    cli.addCommand("scan-i2c", cmdScanI2c);
    cli.addCommand("ls", cmdLs);
    cli.addCommand("rt-stats", cmdRtStats);
    cli.addCommand("mem", cmdMem);
    cli.addCommand("uptime", cmdUptime);
    cli.addBoundlessCmd("sensor", cmdSensor); // `sensor` full dump; `sensor avg` compact EWM line
#ifdef WITH_NETW
    cli.addCommand("ip", cmdIp);
#endif
    cli.addCommand("adc-restart", cmdAdcRestart);
    cli.addCommand("adc-reset", cmdAdcReset);

    cli.addBoundlessCmd("dc", cmdDc); // dc <hs> [ls]
    cli.addSingleArgCmd("sync", cmdSync);
    cli.addSingleArgCmd("bf,panel", cmdBflow);
    cli.addSingleArgCmd("speed", cmdSpeed);
    cli.addSingleArgCmd("fan", cmdFan);
    cli.addSingleArgCmd("led", cmdLed);
    cli.addBoundlessCmd("gpio", cmdGpio);

#if WITH_PWM_DIAGNOSTICS
    cli.addSingleArgCmd("mcpwmtest", cmdMcpwmTest);
    cli.addSingleArgCmd("gpiodump", [](cmd *c) {
        int pin = Command(c).getArg(0).getValue().toInt();
        // GPIO_ENABLE_REG (or _ENABLE1 for pin>=32) bit, GPIO_OUT_SEL signal, IO_MUX function
        uint32_t en = (pin < 32) ? REG_READ(GPIO_ENABLE_REG) : REG_READ(GPIO_ENABLE1_REG);
        uint32_t out_sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (pin * 4));
        uint32_t io_mux = REG_READ(IO_MUX_GPIO0_REG + (pin * 4));
        uint32_t enable_bit = (en >> (pin & 31)) & 1;
        uint32_t signal_idx = out_sel & 0x1FF;
        uint32_t oen_sel = (out_sel >> 10) & 1;
        UART_LOG("gpio %d: enable=%u sig=%u oen_sel=%u io_mux=0x%lx",
                 pin, (unsigned) enable_bit, (unsigned) signal_idx, (unsigned) oen_sel, (unsigned long) io_mux);
    });
    cli.addSingleArgCmd("mcpwmdump", [](cmd *c) {
        int grp = Command(c).getArg(0).getValue().toInt();
        uint32_t cfg0 = REG_READ(MCPWM_TIMER0_CFG0_REG(grp));
        uint32_t cfg1 = REG_READ(MCPWM_TIMER0_CFG1_REG(grp));
        uint32_t status = REG_READ(MCPWM_TIMER0_STATUS_REG(grp));
        UART_LOG("mcpwm grp%d T0: cfg0=0x%lx cfg1=0x%lx status=0x%lx (count=%lu dir=%lu)",
                 grp, (unsigned long) cfg0, (unsigned long) cfg1, (unsigned long) status,
                 (unsigned long) (status & 0xFFFF), (unsigned long) ((status >> 16) & 1));
    });
    cli.addBoundlessCmd("pwm-dump", [](cmd *) {
        uint32_t freq = converter.getPwmFrequency();
        uint16_t pwmMax = converter.pwmMaxDriver();
        uint16_t pwmCtrl = converter.getCtrlOnPwmCnt();
        uint16_t pwmRect = converter.getRectOnPwmCnt();
        uint16_t dtTicks = converter.getDtTicks();
        uint16_t hs_off = pwmCtrl;
        // enLogic: LS rises at TEZ (tick 0); HiLi: LS rises at cmpHS_. DT module delays the posedge.
        uint16_t ls_on_base = converter.isEnLogic() ? 0 : pwmCtrl;
        uint16_t ls_on = (pwmRect == 0) ? 0 : (uint16_t) (ls_on_base + dtTicks);
        uint16_t ls_off = (pwmRect == 0) ? 0 : (uint16_t) (pwmCtrl + pwmRect);
        UART_LOG("freq=%u pwmMax=%u hs_off=%u ls_on=%u ls_off=%u fault=0 brake=0",
                 (unsigned) freq, (unsigned) pwmMax,
                 (unsigned) hs_off, (unsigned) ls_on, (unsigned) ls_off);
    });
    cli.addBoundlessCmd("anaw", [](cmd *c) {
        Command cc(c);
        int pin = cc.getArg(0).getValue().toInt();
        int val = cc.getArg(1).getValue().toInt(); // 0..255
        analogWrite(pin, val);
        UART_LOG("anaw %d -> %d (Arduino LEDC)", pin, val);
    });
#endif
#ifdef WITH_NETW
    cli.addBoundlessCmd("wifi", cmdWifi); // wifi on | off [minutes]
    cli.addSingleArgCmd("wifi-add", cmdWifiAdd);
    cli.addSingleArgCmd("ota", cmdOta);
#endif
    cli.addSingleArgCmd("vset", cmdVset);
    cli.addSingleArgCmd("iset", cmdIset);

    cli.addBoundlessCmd("hostname,hn", cmdHostname);
    cli.addBoundlessCmd("set-config,setc", cmdSetConfig);
    cli.addBoundlessCmd("del-config,delc", cmdDelConfig);
    cli.addBoundlessCmd("get-config,getc", cmdGetConfig);
    cli.addBoundlessCmd("service,svc", cmdService);
#ifdef WITH_BLE
    cli.addBoundlessCmd("ota-ble", cmdOtaBle);
#endif
    cli.addBoundlessCmd("measure-coil", cmdMeasureCoil); // measure-coil l0|ls [steps|hs] [dwell_ms] [apply]
#if WITH_VCONV
    cli.addBoundlessCmd("vconv", cmdVconv); // vconv [pv|bat|set ...]
#endif
}

bool handleCommand(const String &inp) {
    ESP_LOGI("main", "received serial command: '%s'", inp.c_str());

    // +N / -N PWM step is a signed numeric token, not a named command -> handle before SimpleCLI.
    // does not enable manual pwm!
    if ((inp[0] == '+' or inp[0] == '-') && !adcSampler.isCalibrating() && inp.length() < 6 &&
        inp.toInt() != 0 && std::abs(inp.toInt()) < converter.pwmCtrlMax) {
        int pwmStep = inp.toInt();
        int target = (int) converter.getCtrlOnPwmCnt() + pwmStep;
        if (target < 0) target = 0;
        mppt.setTargetDutyCycle((uint16_t) target);
        ESP_LOGI("main", "Manual PWM step %i -> target %i", pwmStep, target);
        loopLF(wallClockUs());
        return true;
    }

    s_cmdFailed = false; // a handler may set this via CMD_FAIL() to report ERR despite a clean parse
    try {
        cli.parse(inp); // runs the matched command callback inline
    } catch (const std::exception &e) {
        // a throwing command must not take down the console/network task
        ESP_LOGE("main", "command '%s' failed: %s", inp.c_str(), e.what());
        return false;
    } catch (...) {
        ESP_LOGE("main", "command '%s' failed: unknown exception", inp.c_str());
        return false;
    }
    if (cli.errored()) {
        ESP_LOGE("main", "%s", cli.getError().toString().c_str()); // unknown command / parse error
        return false;
    }
    if (s_cmdFailed) return false; // handler rejected its input (CMD_FAIL)

    loopLF(wallClockUs());
    return true;
}
