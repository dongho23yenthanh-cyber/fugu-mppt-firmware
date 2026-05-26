
#include "logging.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <Wire.h>
#include <USB.h>


#include "adc/sampling.h"

#include "console.h"
#include "tele/console_ble.h"
#include "cli.h"
#include "buck.h"
#include "mppt.h"
#include "service.h"
#include "adc/sensor_setup.h"
#include "util.h"
#include "viz/lcd.h"
#include "viz/lcd_service.h"
#include "viz/led.h"
#include "tele/console_ble_service.h"
#include "etc/network_shim.h"
#ifdef WITH_NETW
#include "tele/telemetry.h"
#include "tele/ftp_service.h"
#include "tele/telnet_service.h"
#include "tele/telemetry_service.h"
#include "tele/scope_service.h"
#include "tele/home_assistant.h"
#include "etc/ota.h"
#endif
#ifdef WITH_MEASURE_COIL
#include "measure_coil.h"
#endif

#include "etc/version.h"

#include "etc/perf.h"
#ifdef WITH_SPROFILER
#include <sprofiler.h>
#endif

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED

#include <hal/usb_serial_jtag_ll.h>

#endif

#include "storage/key-value.h"
#include "app_state.h"

#include <esp_task_wdt.h>
#include <esp_pm.h>
#include <filesystem>

#include "etc/rt_core_check.h"


ADC_Sampler adcSampler{}; // schedules async ADC reading
VIinVout<const Sensor *> sensors{nullptr, nullptr, nullptr, nullptr};
SynchronousConverter converter; // buck or boost
LedIndicator led;
MpptController mppt{adcSampler, sensors, converter, lcdService.lcd}; // lcd owned by lcdService

unsigned long loopWallClockUs_ = 0;

unsigned long lastLoopTime = 0;

unsigned long timeLastSampler = 0;

unsigned long delayStartUntil = 0;

const auto lfPeriod = 3000000; //(mppt.tracker.avgPower.get() < 1) ? 3000000 : 3000000;

unsigned long lastTimeOutUs = 0;
uint32_t lastNSamples = 0;
unsigned long lastMpptUpdateNumSamples = 0;

float conversionEfficiency;

AppState g_app{}; // mode flags / per-iteration max-lag bookkeeping (see app_state.h)

KeyValueStorage nvs{};

void systemRestart();

static void loopNetwork_task(void *arg);

static void loopRT(void *arg); // this is the critical one

static void loopRTNewData(unsigned long nowMs);

#ifdef WITH_NETW
// arduino-esp32's WiFiGeneric.cpp:298 calls esp_netif_create_default_wifi_ap() unconditionally,
// but esp_wifi defines it only when CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y. We turn SOFTAP off in
// sdkconfig.defaults (saves a few KB) and never act as an AP, so stub it inline here so the
// link succeeds. Inline because a separate TU was getting --gc-sections'd out of libmain.a
// before the linker had a chance to satisfy arduino's undefined ref.
extern "C" void *esp_netif_create_default_wifi_ap(void) { return nullptr; }
#endif


// Single reused log literal for the repeated "failed to read <conf>.conf" catch blocks in setup().
static void logConfErr(const char *name, const std::exception &e) {
    ESP_LOGE("main", "conf %s: %s", name, e.what());
}

// Try-load a conf file. On parse error, log and (if fatal) set g_app.setupErr; return an empty ConfFile
// so the caller can keep going with defaults instead of throwing out of setup() → reboot loop.
// noWarnIfMissing forwards to ConfFile's flag (some confs are optional even when present).
static ConfFile loadConfSafe(const char *path, bool fatal = true, bool noWarnIfMissing = false) {
    try {
        return ConfFile{path, noWarnIfMissing};
    } catch (const std::exception &e) {
        logConfErr(path, e);
        if (fatal) g_app.setupErr = true;
        return ConfFile{};
    }
}

// I2C + LED + LCD bring-up from board.conf. SDA=255 means "no I2C wired" (LED only). The
// skip_assert escape exists for bench boards where the pre-init bus assertion is too strict.
static void setupI2C(const ConfFile &boardConf, bool &noI2C) {
    if (!boardConf) return;
    try {
        auto i2c_freq = boardConf.getLong("i2c_freq", 100000);
        auto i2c_sda = boardConf.getByte("i2c_sda", 255);
        noI2C = (i2c_sda == 255);
        if (!noI2C) {
            if (!boardConf.getByte("skip_assert", 0))
                try {
                    auto i2c_scl = boardConf.getLong("i2c_scl");
                    assertPinState(i2c_sda, true, "i2c_sda");
                    assertPinState(i2c_scl, true, "i2c_scl");
                    pinMode(i2c_scl, OUTPUT);
                    digitalWrite(i2c_scl, LOW);
                    usleep(1);
                    assertPinState(i2c_sda, true, "i2c_sda(scl_short)");
                } catch (const std::exception &e) {
                    ESP_LOGE("main", "error %s", e.what());
                    g_app.setupErr = true;
                }
            ESP_LOGI("main", "i2c pins SDA=%hi SCL=%hi freq=%lu", i2c_sda, boardConf.getByte("i2c_scl"), i2c_freq);
            if (!Wire.begin(i2c_sda, (uint8_t) boardConf.getLong("i2c_scl"), i2c_freq)) {
                ESP_LOGE("main", "Failed to initialize Wire");
                g_app.setupErr = true;
            }
        } else {
            ESP_LOGI("main", "no i2c_sda pin set");
        }

        led.begin(boardConf);
        led.setHexShort(0x111);

        if (!noI2C) {
            if (!lcdService.initLcd()) {
                ESP_LOGE("main", "Failed to init LCD");
            } else {
                lcdService.displayMessage("Fugu FW v" FIRMWARE_VERSION "\n" __DATE__ " " __TIME__, 2000);
            }
        }
    } catch (const std::exception &ex) {
        ESP_LOGE("main", "error %s", ex.what());
        g_app.setupErr = true;
    }
}

// Bring up WiFi at boot (when enabled) and load tele.conf. Returns a default-constructed TeleConf
// when WITH_NETW=0 or NO_WIFI is set; mppt.begin tolerates that.
static TeleConf setupNetworkAtBoot() {
    TeleConf teleConf{};
#ifdef WITH_NETW
#ifdef NO_WIFI
    g_app.disableWifi = true;
#endif
    if (!g_app.disableWifi) {
        connect_wifi_async();
        bool res = wait_for_wifi();
        led.setHexShort(res ? 0x565 : 0x200);
        lcdService.displayMessage(
            res ? ("WiFi connected.\n" + wifiLocalIp()) : "WiFi timeout.", 2000);

        try {
            teleConf = ConfFile{"/littlefs/conf/tele.conf"};
        } catch (const std::exception &e) {
            // telemetry host is optional — a malformed tele.conf must not brick the device
            logConfErr("tele.conf", e);
        }
    }
#endif
    return teleConf;
}

// Sensors → converter → MPPT/tracker bring-up. Sets g_app.setupErr on any failure. Kept as one unit
// because these initializations fail-together: without sensors the converter can't run safely,
// without coil/converter conf the PWM driver can't init.
static void setupConverterAndMppt(const ConfFile &boardConf, const Limits &lim, const TeleConf &teleConf, bool noI2C) {
    try {
        setupSensors(boardConf, lim);
        mppt.initSensors(boardConf);
#ifdef WITH_NETW
        scope->addChannel(&mppt, 0, 'u', 12, "vout_filt"); // scope owned by scopeService (scope -> &scopeObj)
#endif

        if (!g_app.setupErr) {
            ConfFile coilConf{"/littlefs/conf/coil.conf"};
            ConfFile converterConf{"/littlefs/conf/converter.conf"};
            ConfFile chargerConf{"/littlefs/conf/charger.conf"};

            mppt.charger.begin(chargerConf);
            converter.init(converterConf, boardConf, coilConf);
        }

        if (!g_app.setupErr && !adcSampler.adcStates.empty()) {
            ConfFile trackerConf{"/littlefs/conf/tracker.conf", true};
            mppt.begin(trackerConf, boardConf, lim, teleConf);
        }
    } catch (const std::runtime_error &er) {
        ESP_LOGE("main", "error during sensor/converter/tracker setup: %s", er.what());
        adcSampler.adcStates.clear();
        g_app.setupErr = true;
        if (!noI2C) scan_i2c();
    }
}

// Register the optional non-RT subsystems as services and start the enabled ones. MQTT keeps its
// mppt/home-assistant wiring here (out of mqtt.cpp) via preStart, re-run on every start. Network
// services that aren't Running at boot self-heal on the WiFi-up edge in loopNetwork_task.
static void registerServices() {
#ifdef WITH_NETW
    MQTT.preStart = [](const ConfFile &mqttConf) {
        mppt.charger.beginMqtt(mqttConf);
        MQTT.onConnected = haMqttSendDiscovery;
    };
    // Periodic HA power publish, throttled inside MqttService::onTick (only ticks while Running).
    MQTT.tickHook = [] {
        if (!mppt.sensorPhysicalI || !mppt.sensorPhysicalU) return; // sensor setup failed, mppt.begin() skipped
        float pow = mppt.sensorPhysicalI->ewm.avg.get() * mppt.sensorPhysicalU->ewm.avg.get();
        haMqttUpdate({.power = mppt.isSweeping() ? NAN : pow});
    };
    g_services.registerService(&MQTT);
    g_services.registerService(&telemetryService);
    g_services.registerService(&ftpService);
    g_services.registerService(&telnetService);
#endif
    g_services.registerService(&lcdService);
#ifdef WITH_NETW
    g_services.registerService(&scopeService);
#endif
#ifdef WITH_BLE
    g_services.registerService(&bleConsoleService);
#endif
    g_services.startEnabledAtBoot(wifiIsConnected());
}

void setup() {
    consoleInit();
    setupCli();
    ESP_LOGI("main", "*** %s", format_version());

    rtcount_test_cycle_counter();

    nvs.init();

    if (!mountLFS()) {
        ESP_LOGE("main", "Error mounting LittleFS partition!");
        g_app.setupErr = true;
    }

#ifdef WITH_SPROFILER
    try {
        ConfFile pprofConf{"/littlefs/conf/pprof.conf", true};

        auto sprofHz = (uint32_t) pprofConf.getLong("sprofiler_hz", 0); // 100~300
        if (sprofHz && esp_cpu_dbgr_is_attached()) {
            // only start the profiler with OpenOCD attached?
            ESP_LOGI("main", "starting sprofiler with freq %lu (samples/bank=%i)", sprofHz, PROFILING_ITEMS_PER_BANK);
            sprofiler_initialize(sprofHz);
        } else if (sprofHz) {
            ESP_LOGW("main", "sprofiler configured but not debugger attached");
        }
    } catch (const std::exception &e) {
        // a malformed pprof.conf must not brick the device — the profiler is purely diagnostic
        ESP_LOGE("main", "error reading pprof.conf, skipping profiler: %s", e.what());
    }
#endif // WITH_SPROFILER

    // A malformed board.conf must not abort setup() (which would reboot and re-read the same bad
    // file → boot loop). Fall back to an empty config and run the safe-idle path via g_app.setupErr.
    ConfFile boardConf = loadConfSafe("/littlefs/conf/board.conf");

    if (!boardConf && std::filesystem::exists("/littlefs/conf")) {
        for (const auto &entry: std::filesystem::directory_iterator("/littlefs/conf")) {
            ESP_LOGI("main", "file: %s", entry.path().c_str());
        }
    }

    auto mcuStr = boardConf.getString("mcu", "");
    if (mcuStr != CONFIG_IDF_TARGET) {
        ESP_LOGE("main", "board.conf expects MCU '%s', but target is '%s'", mcuStr.c_str(), CONFIG_IDF_TARGET);
        g_app.setupErr = true;
    }

    bool noI2C = true;
    setupI2C(boardConf, noI2C);

    TeleConf teleConf = setupNetworkAtBoot();

    Limits lim{};
    try {
        lim = Limits{ConfFile{"/littlefs/conf/limits.conf"}};
    } catch (const std::runtime_error &er) {
        logConfErr("limits.conf", er);
        g_app.setupErr = true;
    }

    setupConverterAndMppt(boardConf, lim, teleConf, noI2C);

    if (!g_app.setupErr) {
        xTaskCreatePinnedToCore(loopRT, "loopRt", 4096 * 4, NULL, RT_PRIO, NULL, 1);
    } else {
        led.setHexShort(0x200);
    }

    registerServices();

    // this will defer all logs, if abort() is called during setup we might never see relevant messages
    // so calls this after everything else has been set up
#ifdef WITH_NETW
    enable_esp_log_to_telnet();
#endif

#if CONFIG_HEAP_POISONING_COMPREHENSIVE
    ESP_LOGW("main", "HEAP_POISONING=COMPREHENSIVE: every alloc has head/tail canaries + fill, expect 5-10%% perf hit and ~16B/alloc RAM cost. Debug-only.");
#elif CONFIG_HEAP_POISONING_LIGHT
    ESP_LOGW("main", "HEAP_POISONING=LIGHT: every alloc has tail canary, free() asserts on overrun. Debug-only — revert sdkconfig when done.");
#endif

    ESP_LOGI("main", "setup() done.");

    /*g_app.manualPwm = true;
    adcSampler.cancelCalibration();
    pwm.pwmPerturb(1327); // this can destroy the BF switch
    pwm.enableLowSide(true);
    mppt.bflow.enable(true); */
}


void loop() {
    // use arduino loop for networking (non-critical stuff)
    loopNetwork_task(nullptr);
}

static esp_err_t disable_cpu_power_saving(void) {
    esp_err_t ret = ESP_OK;

#ifdef CONFIG_PM_ENABLE
    static esp_pm_lock_handle_t s_cli_pm_lock = nullptr;

    ret = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "noPwrSave", &s_cli_pm_lock);
    if (ret == ESP_OK) {
        ESP_ERROR_CHECK(esp_pm_lock_acquire(s_cli_pm_lock));
        ESP_LOGI("main", "Successfully created CLI pm lock");
    } else {
        if (s_cli_pm_lock != NULL) {
            esp_pm_lock_delete(s_cli_pm_lock);
            s_cli_pm_lock = NULL;
        }
        ESP_LOGW("main", "Failed to create CLI pm lock: %s", esp_err_to_name(ret));
    }
#endif
    return ret;
}

void stopAndBackoff(uint32_t secondsDelay) {
    mppt.shutdownDcdc("stopAndBackoff");
    delayStartUntil = wallClockUs() + secondsDelay * 1000000;
}

static void loopRT(void *arg) {
    // low-latency control loop task

    try {
        adcSampler.begin();
    } catch (const std::runtime_error &er) {
        ESP_LOGE("main", "error starting ADC sampler: %s", er.what());
        while (true) {
            loopWallClockUs_ = micros();
            vTaskDelay(10);
        }
    }

    disable_cpu_power_saving();

    assert(xPortGetCoreID() == RT_CORE);
    vTaskPrioritySet(nullptr, 20); // highest priority (24)
    // ^ see https://docs.espressif.com/projects/esp-idf/en/stable/esp32h2/api-guides/performance/speed.html#choosing-task-priorities-of-the-application

    ESP_LOGD("main", "Loop running on core %i", (int) xPortGetCoreID());

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    ESP_LOGW("main", "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS enabled!");
    delay(1000);
#endif

    // enable log defer just before starting the RT loop
    // this way we instantly know about things going wrong during start-up
    loggingEnableDefer();

    while (true) {
        rtcount("start");
        loopWallClockUs_ = micros();
        rtcount("micros");
        auto &nowUs(loopWallClockUs_);

        auto nowMs = (unsigned long) (nowUs / 1000ULL);

        rtcount("adc.update.pre");
        auto samplerRet = adcSampler.update();
        rtcount("adc.update");

        if (adcSampler.halted) continue;

        if (samplerRet == ADC_Sampler::UpdateRet::AdcError) {
            ESP_LOGE("main", "ADC error");
            stopAndBackoff(16);
        }

        if (samplerRet == ADC_Sampler::UpdateRet::CalibFailure) {
            stopAndBackoff(4);
        }

        if (samplerRet != ADC_Sampler::UpdateRet::NewData) {
            if (adcSampler.isCalibrating() && mppt.boardPowerSupplyUnderVoltage()) {
                ESP_LOGW("main", "Board power supply UV %.2f!", mppt.boardPowerSupplyVoltage());
                adcSampler.cancelCalibration();
            }

            if (timeLastSampler && nowUs - timeLastSampler > 200000) {
                if (!converter.disabled()) {
                    stopAndBackoff(4);
                    ESP_LOGE("main", "Timeout new ADC sample, shutdown! nSamples=%lu dt=%lu ms",
                             lastMpptUpdateNumSamples, (nowUs - timeLastSampler) / 1000);
                    if (adcSampler.resetPeripherals()) {
                        ESP_LOGI("main", "ADC peripherals reset");
                    } else {
                        ESP_LOGE("main", "Failed to reset ADC peripherals");
                    }
                }

                if (timeLastSampler && nowUs - timeLastSampler > 60000000) {
                    systemRestart();
                }
            }

            if (!timeLastSampler and nowMs > 20000) {
                converter.disable();
                ESP_LOGE("main", "Never got a sample! Please check ADC");
                if (nowMs > (1000 * 60 * 15)) {
                    systemRestart();
                }
                delay(5000);
            }
        } else {
            loopRTNewData(nowMs);
            rtcount("loopRTNewData");
        }


        auto lag = nowUs - lastLoopTime;
        if (lastLoopTime && lag > g_app.maxLoopLag && !converter.disabled()) g_app.maxLoopLag = lag;
        lastLoopTime = nowUs;

#if CAPTURE_LOOP_DT
        auto now2 = micros();
        auto loopDT = now2 - nowUs;
        if (loopDT > g_app.maxLoopDT && !pwm.disabled())
            g_app.maxLoopDT = loopDT;
#endif

        // Not need to yield or call vTaskDelay here
        // waiting for adc values does the necessary blocking

        //vTaskDelay(0); // this resets the Watchdog Timer (WDT) for some reason
        //vTaskDelay(1);
        //yield();
    }
}

static std::string mpptStateStr() {
    auto st = mppt.getState();
    std::string arrow;
    if (st == MpptControlMode::MPPT) {
        if (mppt.tracker.slowMode) {
            arrow = mppt.tracker._direction ? "⇡" : "⇣";
        } else {
            arrow = mppt.tracker._direction ? "↑" : "↓";
        }
    }

    mppt.limIdxSampled.reset();

    return arrow + MpptState2String[(uint8_t) st];
}

// Shut down WiFi when chip temperature gets dangerous. BLE on ESP32-S3 ran the chip hot enough
// to need this even before the modem-sleep fix; the temperature ceiling here is a last resort.
static void wifiShutdownIfHot(float chipTempC) {
#ifdef WITH_NETW
    if (chipTempC > 95 && wifiIsConnected()) {
        ESP_LOGW("main", "High chip temperature, shut-down WiFi");
        flush_async_uart_log();
        vTaskDelay(pdMS_TO_TICKS(200));
        wifiHardOff();
    }
#endif
}

// Latency watchdog. Fires when the RT loop is producing fewer samples per second than required
// (slow ADC, blocked path, etc.) for ~0.9 lfPeriod and trips a backoff to recover.
static void lfWatchdog(unsigned long nowUs, uint32_t dt, uint32_t sps, uint32_t nSamples) {
    if (!((dt > (lfPeriod * 0.9f)) && sps < g_app.loopRateMin && !converter.disabled() &&
          nSamples > max(g_app.loopRateMin * 5, 200) &&
          !g_app.manualPwm && lastTimeOutUs && (nowUs - adcSampler.getTimeLastCalibrationUs()) > 2000000))
        return;
    auto loopRunTime = (nowUs - adcSampler.getTimeLastCalibrationUs());
    ESP_LOGE("main", "Loop latency high (%lu<%hu Hz), shutdown! (nSamples=%lu D=%u rt=%.1fs)",
             sps, g_app.loopRateMin, nSamples, converter.getCtrlOnPwmCnt(), loopRunTime * 1e-6f);
    stopAndBackoff(4);
}

// Low-frequency control reads (RT-adjacent, not in the ADC fast path): NTC + chip temp,
// charger termination state, and the thermal-cap WiFi cutoff.
static void lfControl() {
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    g_app.usbConnected = usb_serial_jtag_is_connected();
#endif
    mppt.ntc.read();
    mppt.ucTemp.read();
    if (sensors.Vout)
        mppt.charger.update(sensors.Vout->ewm.avg.get(), sensors.Iout->ewm.avg.get());
    wifiShutdownIfHot(mppt.ucTemp.last());
}

// One-line UART/MQTT/telnet status line. WITH_MEASURE_COIL skips it during the coil sweep so the
// measurement output isn't intermixed with the status print.
static void lfStatusLine(uint32_t nSamples, uint32_t sps, uint32_t dt) {
#ifdef WITH_MEASURE_COIL
    if (!sensors.Vin || isMeasuring()) return;
#else
    if (!sensors.Vin) return;
#endif
    UART_LOG(
        "V=%4.*f/%5.*f I=%4.*f/%5.*fA %5.1fW %.0f℃%.0f℃ %2lusps %2lu㎅/s %s(H|L|Lm)=%4hu|%4hu|%4hu"
        " st=%5s,%i lag=%lu㎲ N=%lu rssi=%hi",
        sensors.Vin->last >= 9.55f ? 1 : 2, sensors.Vin->last,
        sensors.Vout->last >= 9.55f ? 2 : 2, sensors.Vout->last,
        sensors.Iin->ewm.avg.get() >= 9.55f ? 1 : 2, sensors.Iin->ewm.avg.get(),
        sensors.Iout->ewm.avg.get() >= 9.55f ? 2 : 2, sensors.Iout->ewm.avg.get(),
        sensors.Vin->ewm.avg.get() * sensors.Iin->ewm.avg.get(),
        mppt.ntc.last(), mppt.ucTemp.last(),
        sps,
        dt ? (uint32_t) (bytesSent * 1000llu / dt) : 0,
        converter.inDCM() ? "DCM" : "CCM",
        converter.getCtrlOnPwmCnt(), converter.getRectOnPwmCnt(), converter.getRectOnPwmMax(),
        g_app.manualPwm
            ? "MANU"
            : (mppt.converter.disabled() && !mppt.startCondition()
                   ? (mppt.boardPowerSupplyUnderVoltage() ? "UV" : "START")
                   : mpptStateStr().c_str()),
        (int) mppt.active(),
        g_app.maxLoopLag,
        nSamples,
        (int16_t) wifiRssi()
    );
}

// RGB LED color from current converter state (manual / idle / sweep / MPPT / CV / topping).
static void lfUpdateLed(unsigned long nowUs) {
    if (g_app.manualPwm) {
        uint8_t i = constrain((sensors.Vout->last * sensors.Iout->last) / mppt.limits.P_max * 255, 1, 255);
        led.setRGB(0, i, i);
        return;
    }
    if (mppt.converter.disabled()) {
        if (g_app.setupErr) { led.setHexShort(0x600); return; }
        if (!sensors.Vin) return;
        if (mppt.boardPowerSupplyUnderVoltage(true)) { led.setHexShort(0x100); return; }
        if (nowUs > 60000000 * 15) led.setHexShort(0x000); // turn off light at night (protect insects)
        else led.setHexShort(sensors.Vout->last > sensors.Vin->last ? 0x100 : 0x300);
        return;
    }
    switch (mppt.getState()) {
        case MpptControlMode::Sweep: led.setHexShort(0x303); break; // purple
        case MpptControlMode::MPPT:
            led.setHexShort(sensors.Iout->ewm.avg.get() > 0.2f ? 0x230 : 0x111);
            break;
        case MpptControlMode::CV: led.setHexShort(0x033); break;
        default: led.setHexShort(0x310); break; // CC/CP/topping
    }
}

void loopLF(const unsigned long &nowUs) {
    auto &nSamples(sensors.Vout ? sensors.Vout->numSamples : lastNSamples);
    auto dt = nowUs - lastTimeOutUs;
    uint32_t sps = (dt > 20000) ? (uint64_t) (nSamples - lastNSamples) * 1000000llu / dt : 0;

    lfWatchdog(nowUs, dt, sps, nSamples);
    lfControl();
    lfStatusLine(nSamples, sps, dt);

    lastNSamples = nSamples;
    bytesSent = 0;

    if (mppt.converter.disabled())
        mppt.meter.update(); // always update the meter

    lfUpdateLed(nowUs);
}

static void loopRTNewData(unsigned long nowMs) {
    // cap control update rate to sensor sampling rate (see below). rate for all 3 sensors are equal.
    // we choose Vout here because this is the most critical control value (react fast to prevent OV)
    auto nSamples = sensors.Vout->numSamples;

    bool haveNewSample = (nSamples - lastMpptUpdateNumSamples) > 0;

    if (haveNewSample)
        timeLastSampler = wallClockUs();

    // auto range current sense ADC channel TODO add hysteresis
    /* float adcRangeBound = 2.0f;
    adcSampler.adc->setMaxExpectedVoltage(
            sensors.Iin->adcCh,
            (std::max(sensors.Iin->last, sensors.Iin->ewm.avg.get()) < sensors.Iin->transform.apply(adcRangeBound))
            ? adcRangeBound
            : sensors.Iin->transform.apply_inverse(30.f)
    ); */


    if (unlikely(adcSampler.isCalibrating())) {
        mppt.shutdownDcdc("calib", 0); // calibration must resume MPPT immediately on completion
    } else {
        if (mppt.active() or g_app.manualPwm) {
            rtcount("protect.pre");
            bool mppt_ok = true;
            if (!mppt.converter.disabled()) {
                mppt_ok &= mppt.protect(g_app.manualPwm);
                rtcount("protect");
                mppt_ok &= mppt.protectLf(g_app.manualPwm);
                rtcount("protectLf");
            }
            if (mppt_ok) {
                if (haveNewSample) {
                    if (!g_app.manualPwm) {
                        rtcount("mppt.update.pre");
                        mppt.update();
                        rtcount("mppt.update");
                    } else {
                        mppt.updateManual();
                    }
                    lastMpptUpdateNumSamples = nSamples;
                }
            } else {
                stopAndBackoff(4);
                if (g_app.manualPwm) mppt.setTargetDutyCycle(0); // don't re-fade after a manual-mode trip
            }
        } else if (wallClockUs() > delayStartUntil && mppt.startCondition()) {
            if (!g_app.manualPwm) {
                rtcount("mppt.startSweep.pre");
                mppt.startSweep();
                rtcount("mppt.startSweep");
                delayStartUntil = wallClockUs() + 4 * 1000000;
            }
        }

        if (scope && sensors.Vout && haveNewSample)
            scope->addSample12(&mppt, 0,
                               (uint16_t) max(0.f, sensors.Vout->last / 60.0f *
                                                   2000.0f));
    }


    if (g_app.manualPwm) {
        if (!converter.disabled())
            converter.pwmPerturb(0); // this will increase LS duty cycle if possible
    }
}


// Per-tick WiFi + network-service work for loopNetwork_task. Handles auto-reenable timer, the
// power/temp-gated wifiLoop, and self-healing network services on the WiFi-up edge.
static void networkLoopTick() {
#ifdef WITH_NETW
    if (g_app.disableWifi && g_app.wifiReenableMs && (int32_t) (wallClockMs() - g_app.wifiReenableMs) >= 0) {
        g_app.wifiReenableMs = 0;
        g_app.disableWifi = false;
        UART_LOG("WiFi re-enabled after timeout");
        connect_wifi_async();
    }

    if (!g_app.disableWifi) {
        /* only connect with disabled power conversion
         * ESP32's wifi can cause latency issues otherwise
         */
        wifiLoop((converter.disabled() || mppt.tracker._curPower < 10) && mppt.ucTemp.last() < 80);

        // self-heal: bring up enabled network services on the WiFi-up edge (they fail to start
        // at boot when WiFi isn't connected yet). _wifiConnected() has set up MDNS by now.
        static bool wifiWasUp = false;
        bool wifiUp = wifiIsConnected();
        if (wifiUp && !wifiWasUp) g_services.startEnabledNetworkServices();
        wifiWasUp = wifiUp;
    }
#endif
}

// True while a TCP scope client is attached and its blocking netLoop() will provide the cooperative
// yield this iteration; otherwise loopNetwork_task must yield explicitly.
static bool scopeKeepsAwake() {
#ifdef WITH_NETW
    return scopeService.state() == ServiceState::Running && scopeService.hasClient();
#else
    return false;
#endif
}

static void loopNetwork_task(void *arg) {
    //ESP_LOGI("main", "Net loop running on core %i", xPortGetCoreID());
    assert(xPortGetCoreID() == 0);

    auto nowMs(wallClockMs());

    loopUart(nowMs);
    flush_async_uart_log();
    process_queued_tasks();

    networkLoopTick();

    // ftp / telnet / telemetry / lcd / scope ticks (only the Running ones do work)
    g_services.tickAll();


    if ((wallClockUs() - lastTimeOutUs) >= (mppt.converter.disabled() ? (lfPeriod * 8) : lfPeriod) or !lastTimeOutUs) {
        loopLF(wallClockUs());
        // HA power publish moved to MqttService::onTick (MQTT.tickHook, wired in setup()).
        lastTimeOutUs = wallClockUs();
    }

    // Preserve the cooperative yield: scope's netLoop() blocks ~1 tick when a client is attached
    // and serves as the yield; otherwise we must yield explicitly.
    if (!scopeKeepsAwake())
        vTaskDelay(pdMS_TO_TICKS(1));
}

// Give the telnet client a chance to drain a FIN before we wipe the stack. stopAll() would slam
// the socket shut, so do this ahead of it.
static void restartCloseTelnet() {
#ifdef WITH_NETW
    telnetService.beginClose();
    for (int i = 0; i < 200 && telnetService.closePending(); ++i) delay(10);
#endif
}

void systemRestart() {
    converter.disable();
    UART_LOG("Rebooting");
    restartCloseTelnet();
    g_services.stopAll(); // tear down enabled services while WiFi is still up (see mqtt_task overflow)
    delay(300);
    ESP.restart();
}


[[maybe_unused]] void esp_task_wdt_isr_user_handler() {
    //throw std::runtime_error("reboot");
    if (esp_cpu_dbgr_is_attached()) return;

    enqueue_task([] {
        ESP_LOGE("main", "Restart after WDT trigger");
        systemRestart();
    });
}
