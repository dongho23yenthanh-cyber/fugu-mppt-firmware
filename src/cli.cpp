#include "cli.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cmath>
#include <SimpleCLI.h>

#include "logging.h"
#include "conf.h"               // ConfFile
#include "util.h"               // scan_i2c, wallClockUs
#include "buck.h"               // SynchronousConverter
#include "mppt.h"               // MpptController
#include "adc/sampling.h"       // ADC_Sampler, Sensor, VIinVout
#include "service.h"            // g_services, stateStr/levelToStr/strToLevel
#include "viz/led.h"            // LedIndicator
#include "storage/key-value.h"  // KeyValueStorage
#include "tele/telemetry.h"     // connect_wifi_async, add_ap
#include "etc/ota.h"            // doOta

// Defined in main.cpp's TU via etc/perf.h (which defines, not just declares, it — so we can't
// include that header here without a duplicate definition). Forward-declare and let the linker
// resolve it.
void print_real_time_stats_1s_task(void *);

// Globals owned by main.cpp that the command handlers act on. Declared extern here (rather than in
// a shared header) because they're implementation coupling between main.cpp and this file, not a
// public interface. Types come from the includes above, so these match main.cpp's definitions.
extern bool manualPwm;
extern bool disableWifi;
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

// Defined in main.cpp (non-static so we can reach them from here).
void systemRestart();
void stopAndBackoff(uint32_t secondsDelay);
void loopLF(const unsigned long &nowUs);

#define NON_RT_CORE 0  // mirrors main.cpp: cmdRtStats spawns its task off the RT core

static SimpleCLI cli;

// --- Console command callbacks ---------------------------------------------------------------
// One handler per command, registered in setupCli(). SimpleCLI dispatches by the first token and
// hands us a `cmd*`; wrap it in Command to read arguments. Validation that used to live in the
// old if-chain conditions now lives in the handler body. The callbacks are void, so a handler
// can't return a bool — instead bad input goes through CMD_FAIL(), which logs a warning, sets
// s_cmdFailed, and returns. handleCommand() clears the flag before dispatch and reports ERR when
// it's set, so the console's OK/ERR marker matches the warning instead of contradicting it.
// (SimpleCLI's own errored() only catches parser-level errors like an unknown command.)
// Output still goes through the global ESP_LOG/UART_LOG path.
static bool s_cmdFailed = false;
#define CMD_FAIL(...) do { ESP_LOGW("main", __VA_ARGS__); s_cmdFailed = true; return; } while (0)

static void cmdSync(cmd *c) {
    if (!manualPwm) CMD_FAIL("sync: only in manual PWM (use 'dc N' first)");
    auto arg = Command(c).getArg(0).getValue();
    if (arg == "on" or arg == "1" or arg == "off" or arg == "0") {
        converter.forcedPwm_(false);
        converter.enableSyncRect(arg == "on" or arg == "1", true);
    } else if (arg == "forced") {
        converter.enableSyncRect(true);
        converter.forcedPwm_(true);
    } else {
        CMD_FAIL("sync: expected on|off|forced");
    }
}

static void cmdBflow(cmd *c) {
    if (!manualPwm) CMD_FAIL("bf: only in manual PWM (use 'dc N' first)");
    if (!mppt.bflow) CMD_FAIL("panel switch not configured");
    auto newState = Command(c).getArg(0).getValue().toInt();
    if (newState != 0 and newState != 1) CMD_FAIL("bf: expected 0|1");
    if (mppt.bflow.state() != newState)
        ESP_LOGI("main", "Set bflow state %i", (int) newState);
    mppt.bflow.enable(newState);
}

static void cmdRestart(cmd *) { systemRestart(); }

static void cmdMppt(cmd *) {
    if (!manualPwm) CMD_FAIL("MPPT already enabled");
    ESP_LOGI("main", "MPPT re-enabled");
    manualPwm = false;
}

static void cmdDc(cmd *c) {
    if (adcSampler.isCalibrating()) CMD_FAIL("dc: busy calibrating");
    auto v = Command(c).getArg(0).getValue();
    if (v.length() == 0) CMD_FAIL("dc: expected duty cycle");
    auto dc = v.toInt();
    if (dc < 0 || dc > converter.pwmCtrlMax) CMD_FAIL("dc: out of range [0,%i]", (int) converter.pwmCtrlMax);

    if (!manualPwm || converter.disabled()) {
        ESP_LOGI("main", "Switched to manual PWM");
        if (dc != 0 && !mppt.limits.reverse_current_paranoia) {
            converter.enableSyncRect(true);
            mppt.bflow.enable(true);
        }
    }
    manualPwm = true;
    mppt.setTargetDutyCycle(dc);
}

static void cmdShortLs(cmd *) {
    if (converter.boost() && abs(sensors.Vin->ewm.avg.get()) < 0.05) {
        manualPwm = true;
        converter.shortLs();
    } else {
        CMD_FAIL("short-ls: requires boost mode and Vin~0");
    }
}

static void cmdSpeed(cmd *c) {
    float speedScale = Command(c).getArg(0).getValue().toFloat();
    if (speedScale >= 0 && speedScale < 10) {
        mppt.speedScale = speedScale;
        ESP_LOGI("main", "Set tracker speed scale %.4f", speedScale);
    } else {
        CMD_FAIL("speed: out of range [0,10)");
    }
}

static void cmdFan(cmd *c) {
    if (!mppt.fan.fanSet(Command(c).getArg(0).getValue().toFloat() * 0.01f))
        CMD_FAIL("fan: set failed");
}

static void cmdLed(cmd *c) { led.setRGB(Command(c).getArg(0).getValue().c_str()); }

static void cmdSweep(cmd *) { mppt.startSweep(); }

static void cmdResetLag(cmd *) {
    maxLoopLag = 0;
#if CAPTURE_LOOP_DT
    maxLoopDT = 0;
#endif
    rtcount_print(true);
}

static void cmdWifi(cmd *c) {
    auto arg = Command(c).getArg(0).getValue();
    if (arg == "on") {
        disableWifi = false;
        connect_wifi_async();
    } else if (arg == "off") {
        WiFi.disconnect(true);
        disableWifi = true;
        nvs.open();
        if (!nvs.readString("wifi_ssid", "").empty())
            nvs.writeString("wifi_ssid", "");
        nvs.close();
    } else {
        CMD_FAIL("wifi: expected on|off");
    }
}

static void cmdWifiAdd(cmd *c) {
    auto ssidAndPw = Command(c).getArg(0).getValue();
    auto i = ssidAndPw.indexOf(':');
    if (i <= 0) CMD_FAIL("wifi-add: expected ssid:password");
    std::string ssid = ssidAndPw.substring(0, i).c_str();
    auto psk = ssidAndPw.substring(i + 1);
    ESP_LOGI("main", "adding wifi network %s (psk=%s)", ssid.c_str(), psk.c_str());
    add_ap(ssid, psk.c_str());
}

static void cmdScanI2c(cmd *) { scan_i2c(); }

static void cmdLs(cmd *) { ESP_LOGE("main", "not impl"); }

static void cmdOta(cmd *c) {
    auto url = Command(c).getArg(0).getValue();
    if (url.length() == 0) CMD_FAIL("ota: expected url");
    stopAndBackoff(10);
    adcSampler.halted = true; // disable ADC reading
    doOta(url.c_str());
    adcSampler.halted = false;
}

static void cmdRtStats(cmd *) {
    xTaskCreatePinnedToCore(print_real_time_stats_1s_task, "rtstats", 4096, NULL, 1, NULL, NON_RT_CORE /*core*/);
}

static void cmdMem(cmd *) {
    UART_LOG("Total heap:  %9ld", ESP.getHeapSize());
    UART_LOG("Free heap:   %9ld", ESP.getFreeHeap());
    UART_LOG("Total PSRAM: %9ld", ESP.getPsramSize());
    UART_LOG("Free PSRAM:  %9ld", ESP.getFreePsram());
}

static void cmdSensor(cmd *) {
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

static void cmdIp(cmd *) { UART_LOG("Local IP Address: %s", WiFi.localIP().toString().c_str()); }

static void cmdAdcRestart(cmd *) { adcSampler.reInitADCs(); }

static void cmdAdcReset(cmd *) { adcSampler.resetPeripherals(); }

// hostname [name]  — no arg prints the current hostname; an arg sets it.
static void cmdHostname(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 1 || cc.getArg(0).getValue().length() == 0) {
        UART_LOG("Hostname: %s", getHostname().c_str());
        return;
    }
    auto hn = cc.getArg(0).getValue();
    nvs.open();
    nvs.writeString("hostname", hn.c_str());
    nvs.close();
}

// set-config <file> <key> <value...>  — value may contain spaces, so join the trailing tokens.
//   set-config coil.conf L0 50            set-config mqtt.conf broker_uri mqtt://192.168.1.134:1882
//   set-config limits.conf iout_max 35    set-config charger.conf cell_voltage_eoc 3.53
static void cmdSetConfig(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 3) CMD_FAIL("set-config: expected <file> <key> <value>");
    auto fn = "/littlefs/conf/" + cc.getArg(0).getValue();
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
    if (cc.countArgs() < 2) CMD_FAIL("del-config: expected <file> <key>");
    auto fn = "/littlefs/conf/" + cc.getArg(0).getValue();
    auto key = cc.getArg(1).getValue();
    ConfFile conf{fn.c_str()};
    if (conf.remove(key.c_str()))
        ESP_LOGI("main", "Deleted conf '%s:%s'", fn.c_str(), key.c_str());
    else
        CMD_FAIL("del-config: key '%s' not found in %s", key.c_str(), fn.c_str());
}

// get-config <file> [key]  — print one key, or dump the whole file.
static void cmdGetConfig(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 1) CMD_FAIL("get-config: expected <file> [key]");
    auto fn = "/littlefs/conf/" + cc.getArg(0).getValue();
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
    else CMD_FAIL("vset: out of range [0,999]");
}

static void cmdIset(cmd *c) {
    float v = Command(c).getArg(0).getValue().toFloat();
    if (v >= 0 and v <= 999) mppt.charger.params.Ibat_lim = v;
    else CMD_FAIL("iset: out of range [0,999]");
}

// svc [list]                       svc start|stop|restart <name>
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
            const char *color = st == ServiceState::Running ? "\x1b[32m"   // green
                              : st == ServiceState::Failed  ? "\x1b[31m"   // red
                                                            : "\x1b[90m";  // gray
            char state[24];
            snprintf(state, sizeof(state), "%s%-8s\x1b[0m", color, stateStr(st));
            UART_LOG("%-10s %s %-6s %-8s %s", s->name(), state,
                     levelToStr(s->logLevel()), s->enabled() ? "yes" : "no", detail.c_str());
        }
        return;
    }

    if (n < 2) CMD_FAIL("svc: expected <name>");
    auto name = cc.getArg(1).getValue();
    auto *s = g_services.findByName(name.c_str());
    if (!s) CMD_FAIL("svc: unknown '%s'", name.c_str());

    if (sub == "start") {
        s->setEnabledPersist(true);
        if (!s->start()) UART_LOG("start failed (state=%s)", stateStr(s->state()));
    } else if (sub == "stop") {
        s->setEnabledPersist(false);
        s->stop();
    } else if (sub == "restart") {
        s->restart();
    } else if (sub == "log") {
        if (n < 3) CMD_FAIL("svc log: expected <error|warn|info>");
        s->setLogLevel(strToLevel(cc.getArg(2).getValue().c_str()), /*persist*/ true);
    } else {
        CMD_FAIL("svc: unknown subcommand '%s'", sub.c_str());
    }
}

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
    cli.addCommand("sensor", cmdSensor);
    cli.addCommand("ip", cmdIp);
    cli.addCommand("adc-restart", cmdAdcRestart);
    cli.addCommand("adc-reset", cmdAdcReset);

    cli.addSingleArgCmd("dc", cmdDc);
    cli.addSingleArgCmd("sync", cmdSync);
    cli.addSingleArgCmd("bf,panel", cmdBflow);
    cli.addSingleArgCmd("speed", cmdSpeed);
    cli.addSingleArgCmd("fan", cmdFan);
    cli.addSingleArgCmd("led", cmdLed);
    cli.addSingleArgCmd("wifi", cmdWifi);
    cli.addSingleArgCmd("wifi-add", cmdWifiAdd);
    cli.addSingleArgCmd("ota", cmdOta);
    cli.addSingleArgCmd("vset", cmdVset);
    cli.addSingleArgCmd("iset", cmdIset);

    cli.addBoundlessCmd("hostname", cmdHostname);
    cli.addBoundlessCmd("set-config", cmdSetConfig);
    cli.addBoundlessCmd("del-config", cmdDelConfig);
    cli.addBoundlessCmd("get-config", cmdGetConfig);
    cli.addBoundlessCmd("svc", cmdService);
}

bool handleCommand(const String &inp) {
    ESP_LOGI("main", "received serial command: '%s'", inp.c_str());

    // +N / -N PWM step is a signed numeric token, not a named command -> handle before SimpleCLI.
    if ((inp[0] == '+' or inp[0] == '-') && !adcSampler.isCalibrating() && inp.length() < 6 &&
        inp.toInt() != 0 && std::abs(inp.toInt()) < converter.pwmCtrlMax) {
        int pwmStep = inp.toInt();
        converter.pwmPerturb((int16_t) pwmStep);
        ESP_LOGI("main", "Manual PWM step %i -> %i", pwmStep, (int) converter.getCtrlOnPwmCnt());
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
