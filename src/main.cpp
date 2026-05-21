
#include "logging.h"

#include <Arduino.h>
#include <Wire.h>
#include <USB.h>


#include "adc/sampling.h"

#include "console.h"
#include "console_ble.h"
#include "cli.h"
#include "buck.h"
#include "mppt.h"
#include "service.h"
#include "sensor_setup.h"
#include "util.h"
#include "tele/telemetry.h"
#include "tele/ftp_service.h"
#include "tele/telnet_service.h"
#include "tele/telemetry_service.h"
#include "tele/scope_service.h"
#include "viz/lcd.h"
#include "viz/lcd_service.h"
#include "viz/led.h"
#include "console_ble_service.h"
#include "etc/ota.h"

#include "etc/version.h"

#include "etc/perf.h"
#ifdef WITH_SPROFILER
#include <sprofiler.h>
#endif

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED

#include <hal/usb_serial_jtag_ll.h>

#endif

#include "storage/key-value.h"

#include <esp_task_wdt.h>
#include <esp_pm.h>
#include <filesystem>

#include "tele/home_assistant.h"


ADC_Sampler adcSampler{}; // schedules async ADC reading
VIinVout<const Sensor *> sensors{nullptr, nullptr, nullptr, nullptr};
SynchronousConverter converter; // buck or boost
LedIndicator led;
MpptController mppt{adcSampler, sensors, converter, lcdService.lcd}; // lcd owned by lcdService

unsigned long loopWallClockUs_ = 0;

unsigned long lastLoopTime = 0, maxLoopLag = 0;

unsigned long timeLastSampler = 0;

unsigned long delayStartUntil = 0;

const auto lfPeriod = 3000000; //(mppt.tracker.avgPower.get() < 1) ? 3000000 : 3000000;

#if CAPTURE_LOOP_DT
unsigned long maxLoopDT = 0;
#endif

unsigned long lastTimeOutUs = 0;
uint32_t lastNSamples = 0;
unsigned long lastMpptUpdateNumSamples = 0;

// todo bit fields
bool manualPwm = false;
bool disableWifi = false;
bool usbConnected = false;
bool setupErr = false;

float conversionEfficiency;
uint16_t loopRateMin = 0;

Scope *scope = &scopeService.scopeObj; // RT/ADC path uses this pointer; object owned by scopeService

KeyValueStorage nvs{};

void systemRestart();

static void loopNetwork_task(void *arg);

static void loopRT(void *arg); // this is the critical one

static void loopRTNewData(unsigned long nowMs);


const char* VER_STRING = "*** Fugu Firmware Version " FIRMWARE_VERSION " (" __DATE__ " " __TIME__ ")";

// Single reused log literal for the repeated "failed to read <conf>.conf" catch blocks in setup().
static void logConfErr(const char *name, const std::exception &e) {
    ESP_LOGE("main", "conf %s: %s", name, e.what());
}

void setup() {
    consoleInit();
    setupCli();
    ESP_LOGI("main", "%s", VER_STRING);

    rtcount_test_cycle_counter();

    nvs.init();

    if (!mountLFS()) {
        ESP_LOGE("main", "Error mounting LittleFS partition!");
        setupErr = true;
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
    // file → boot loop). Fall back to an empty config and run the safe-idle path via setupErr.
    ConfFile boardConf;
    try {
        boardConf = ConfFile{"/littlefs/conf/board.conf"};
    } catch (const std::exception &e) {
        logConfErr("board.conf", e);
        setupErr = true;
    }

    if (!boardConf && std::filesystem::exists("/littlefs/conf")) {
        for (const auto &entry: std::filesystem::directory_iterator("/littlefs/conf")) {
            ESP_LOGI("main", "file: %s", entry.path().c_str());
        }
    }

    auto mcuStr = boardConf.getString("mcu", "");
    if (mcuStr != CONFIG_IDF_TARGET) {
        ESP_LOGE("main", "board.conf expects MCU '%s', but target is '%s'", mcuStr.c_str(), CONFIG_IDF_TARGET);
        setupErr = true;
    }

    bool noI2C = true;

    if (boardConf)
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
                        setupErr = true;
                    }
                ESP_LOGI("main", "i2c pins SDA=%hi SCL=%hi freq=%lu", i2c_sda, boardConf.getByte("i2c_scl"), i2c_freq);
                if (!Wire.begin(i2c_sda, (uint8_t) boardConf.getLong("i2c_scl"), i2c_freq)) {
                    ESP_LOGE("main", "Failed to initialize Wire");
                    setupErr = true;
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
            setupErr = true;
        }


#ifdef NO_WIFI
    disableWifi = true;
#endif

    TeleConf teleConf{};

    if (!disableWifi) {
        connect_wifi_async();
        bool res = wait_for_wifi();
        led.setHexShort(res ? 0x565 : 0x200);
        lcdService.displayMessage(
            res ? ("WiFi connected.\n" + std::string(WiFi.localIP().toString().c_str())) : "WiFi timeout.", 2000);

        try {
            teleConf = ConfFile{"/littlefs/conf/tele.conf"};
        } catch (const std::exception &e) {
            // telemetry host is optional — a malformed tele.conf must not brick the device
            logConfErr("tele.conf", e);
        }
    }

    Limits lim{};
    try {
        lim = Limits{ConfFile{"/littlefs/conf/limits.conf"}};
    } catch (const std::runtime_error &er) {
        logConfErr("limits.conf", er);
        setupErr = true;
    }

    try {
        setupSensors(boardConf, lim);
        mppt.initSensors(boardConf);
        scope->addChannel(&mppt, 0, 'u', 12, "vout_filt"); // scope owned by scopeService (scope -> &scopeObj)

        if (!setupErr) {
            ConfFile coilConf{"/littlefs/conf/coil.conf"};
            ConfFile converterConf{"/littlefs/conf/converter.conf"};
            ConfFile chargerConf{"/littlefs/conf/charger.conf"};

            mppt.charger.begin(chargerConf);

            //auto mode = converterConf.getString("mode", "mppt");

            converter.init(converterConf, boardConf, coilConf);
        }

        if (!setupErr && !adcSampler.adcStates.empty()) {
            ConfFile trackerConf{"/littlefs/conf/tracker.conf", true};
            mppt.begin(trackerConf, boardConf, lim, teleConf);
        }
    } catch (const std::runtime_error &er) {
        ESP_LOGE("main", "error during sensor/converter/tracker setup: %s", er.what());
        //if(adcSampler.adc) delete adcSampler.adc;
        adcSampler.adcStates.clear();
        setupErr = true;
        if (!noI2C) scan_i2c();
    }


    if (!setupErr) {
        xTaskCreatePinnedToCore(loopRT, "loopRt", 4096 * 4, NULL, RT_PRIO, NULL, 1);
        //xTaskCreatePinnedToCore(loopNetwork_task, "netloop", 4096 * 4, NULL, 1, NULL, 0);
        //xTaskCreatePinnedToCore(loopCore0_LF, "core0LF", 4096, NULL, 1, NULL, 0);
    } else {
        led.setHexShort(0x200);
    }

    // Register the optional non-RT subsystems as services and start the enabled ones. MQTT keeps
    // its mppt/home-assistant wiring here (out of mqtt.cpp) via preStart, re-run on every start.
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
    g_services.registerService(&lcdService);
    g_services.registerService(&scopeService);
#ifdef WITH_BLE
    g_services.registerService(&bleConsoleService);
#endif
    g_services.startEnabledAtBoot(); // network services may fail now; self-heal on WiFi-up edge

    // this will defer all logs, if abort() is called during setup we might never see relevant messages
    // so calls this after everything else has been set up
    enable_esp_log_to_telnet();

#if defined(BENCH_TELE) && WITH_BINARY_TELE
    benchTele();   // one-shot encode/compress microbench
#endif

    ESP_LOGI("main", "setup() done.");

    /*manualPwm = true;
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
    mppt.shutdownDcdc();
    delayStartUntil = wallClockUs() + secondsDelay * 1000000;
}

static void loopRT(void *arg) {
    // low-latency control loop task

#define RT_CORE 1
#define NON_RT_CORE 0

#if CONFIG_ARDUINO_RUNNING_CORE == RT_CORE or CONFIG_ARDUINO_EVENT_RUNNING_CORE == RT_CORE or \
CONFIG_ARDUINO_UDP_RUNNING_CORE == RT_CORE or CONFIG_ARDUINO_SERIAL_EVENT_TASK_RUNNING_CORE == RT_CORE
#error "arduino runtime is configured to run on RT_CORE"
#endif


# if CONFIG_ESP_TIMER_ISR_AFFINITY != RT_CORE // and (RT_CORE ? CONFIG_ESP_TIMER_ISR_AFFINITY_CPU1==1 )
#warning "CONFIG_ESP_TIMER_ISR_AFFINITY"
#endif

# if CONFIG_ESP_TIMER_TASK_AFFINITY == RT_CORE
#error "CONFIG_ESP_TIMER_TASK_AFFINITY"
#endif


# if CONFIG_ESP_TIMER_ISR_AFFINITY != RT_CORE or CONFIG_ESP_TIMER_TASK_AFFINITY == RT_CORE or CONFIG_LWIP_TCPIP_TASK_AFFINITY == RT_CORE \
 or CONFIG_PTHREAD_TASK_CORE_DEFAULT == RT_CORE or CONFIG_FMB_PORT_TASK_AFFINITY == RT_CORE or CONFIG_MDNS_TASK_AFFINITY == RT_CORE
#warning  "esp runtime is configured to run on RT_CORE"
#endif

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
        if (lastLoopTime && lag > maxLoopLag && !converter.disabled()) maxLoopLag = lag;
        lastLoopTime = nowUs;

#if CAPTURE_LOOP_DT
        auto now2 = micros();
        auto loopDT = now2 - nowUs;
        if (loopDT > maxLoopDT && !pwm.disabled())
            maxLoopDT = loopDT;
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

void loopLF(const unsigned long &nowUs) {
    auto &nSamples(sensors.Vout ? sensors.Vout->numSamples : lastNSamples);
    auto dt = nowUs - lastTimeOutUs;
    uint32_t sps = (dt > 20000) ? (uint64_t) (nSamples - lastNSamples) * 1000000llu / dt : 0;


    if ((dt > (lfPeriod * 0.9f)) && sps < loopRateMin && !converter.disabled() &&
        nSamples > max(loopRateMin * 5, 200) &&
        !manualPwm && lastTimeOutUs && (nowUs - adcSampler.getTimeLastCalibrationUs()) > 2000000) {
        auto loopRunTime = (nowUs - adcSampler.getTimeLastCalibrationUs());
        ESP_LOGE("main", "Loop latency high (%lu<%hu Hz), shutdown! (nSamples=%lu D=%u rt=%.1fs)",
                 sps, loopRateMin, nSamples, converter.getCtrlOnPwmCnt(), loopRunTime * 1e-6f);
        stopAndBackoff(4);
    }

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    usbConnected = usb_serial_jtag_is_connected();
#endif

    mppt.ntc.read();
    mppt.ucTemp.read();
    if (sensors.Vout)
        mppt.charger.update(sensors.Vout->ewm.avg.get(), sensors.Iout->ewm.avg.get());

    if (mppt.ucTemp.last() > 95 && WiFi.isConnected()) {
        ESP_LOGW("main", "High chip temperature, shut-down WiFi");
        flush_async_uart_log();
        vTaskDelay(pdMS_TO_TICKS(200));
        WiFi.disconnect(true);
    }

    if (sensors.Vin)
        UART_LOG(
            "V=%4.*f/%5.*f I=%4.*f/%5.*fA %5.1fW %.0f℃%.0f℃ %2lusps %2lu㎅/s %s(H|L|Lm)=%4hu|%4hu|%4hu"
            " st=%5s,%i lag=%lu㎲ N=%lu rssi=%hi",
            sensors.Vin->last >= 9.55f ? 1 : 2, sensors.Vin->last,
            sensors.Vout->last >= 9.55f ? 2 : 2, sensors.Vout->last,
            sensors.Iin->ewm.avg.get() >= 9.55f ? 1 : 2, sensors.Iin->ewm.avg.get(), // sensors.Iin->last,
            sensors.Iout->ewm.avg.get() >= 9.55f ? 2 : 2, sensors.Iout->ewm.avg.get(),
            sensors.Vin->ewm.avg.get() * sensors.Iin->ewm.avg.get(),
            //ewm.chIin.std.get() * 1000.f, σIin=%.2fm
            mppt.ntc.last(), mppt.ucTemp.last(),
            sps,
            dt ? (uint32_t) (bytesSent * 1000llu / dt) : 0,
            converter.inDCM() ? "DCM" : "CCM",
            converter.getCtrlOnPwmCnt(), converter.getRectOnPwmCnt(), converter.getRectOnPwmMax(),
            //mppt.getPower()
            manualPwm
                ? "MANU"
                : (mppt.converter.disabled() && !mppt.startCondition()
                       ? (mppt.boardPowerSupplyUnderVoltage() ? "UV" : "START")
                       : mpptStateStr().c_str()),
            (int) mppt.active(),
            maxLoopLag,
            //maxLoopDT,
            nSamples,
            WiFi.RSSI()
        );
    lastNSamples = nSamples;
    bytesSent = 0;

    if (mppt.converter.disabled())
        mppt.meter.update(); // always update the meter

    if (manualPwm) {
        uint8_t i = constrain((sensors.Vout->last * sensors.Iout->last) / mppt.limits.P_max * 255, 1, 255);
        led.setRGB(0, i, i);
    } else if (mppt.converter.disabled()) {
        if (setupErr) led.setHexShort(0x600);
        else if (sensors.Vin) {
            if (mppt.boardPowerSupplyUnderVoltage(true)) {
                led.setHexShort(0x100);
            } else {
                if (nowUs > 60000000 * 15) led.setHexShort(0x000); // turn off light at night (protect insects)
                else led.setHexShort(sensors.Vout->last > sensors.Vin->last ? 0x100 : 0x300);
            }
        }
    } else {
        switch (mppt.getState()) {
            case MpptControlMode::Sweep:
                led.setHexShort(0x303); // purple
                break;
            case MpptControlMode::MPPT:
                if (sensors.Iout->ewm.avg.get() > 0.2f)
                    led.setHexShort(0x230);
                else
                    led.setHexShort(0x111);
                break;
            case MpptControlMode::CV:
                led.setHexShort(0x033);
                break;
            default:
                // CV/CC/CP, topping
                led.setHexShort(0x310);
                break;
        }
    }
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
        mppt.shutdownDcdc();
    } else {
        if (mppt.active() or manualPwm) {
            rtcount("protect.pre");
            bool mppt_ok = true;
            if (!mppt.converter.disabled()) {
                mppt_ok &= mppt.protect(manualPwm);
                rtcount("protect");
                mppt_ok &= mppt.protectLf(manualPwm);
                rtcount("protectLf");
            }
            if (mppt_ok) {
                if (haveNewSample) {
                    if (!manualPwm) {
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
            }
        } else if (wallClockUs() > delayStartUntil && mppt.startCondition()) {
            if (!manualPwm) {
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


    if (manualPwm) {
        if (!converter.disabled())
            converter.pwmPerturb(0); // this will increase LS duty cycle if possible
        //mppt.bflow.enable(true);
        // notice that mppt::protect() calls updateLowSideMaxDuty()
        // delay(1); // why?
    }
}


static void loopNetwork_task(void *arg) {
    //ESP_LOGI("main", "Net loop running on core %i", xPortGetCoreID());
    assert(xPortGetCoreID() == 0);

    auto nowMs(wallClockMs());

    loopUart(nowMs);
    flush_async_uart_log();
    process_queued_tasks();

    if (!disableWifi) {
        /* only connect with disabled power conversion
         * ESP32's wifi can cause latency issues otherwise
         */
        wifiLoop((converter.disabled() || mppt.tracker._curPower < 10) && mppt.ucTemp.last() < 80);

        // self-heal: bring up enabled network services on the WiFi-up edge (they fail to start
        // at boot when WiFi isn't connected yet). _wifiConnected() has set up MDNS by now.
        static bool wifiWasUp = false;
        bool wifiUp = WiFi.isConnected();
        if (wifiUp && !wifiWasUp) g_services.startEnabledNetworkServices();
        wifiWasUp = wifiUp;
    }

    // ftp / telnet / telemetry / lcd / scope ticks (only the Running ones do work)
    g_services.tickAll();


    if ((wallClockUs() - lastTimeOutUs) >= (mppt.converter.disabled() ? (lfPeriod * 8) : lfPeriod) or !lastTimeOutUs) {
        loopLF(wallClockUs());
        // HA power publish moved to MqttService::onTick (MQTT.tickHook, wired in setup()).
        lastTimeOutUs = wallClockUs();
    }

    // Preserve the cooperative yield: scope's netLoop() blocks ~1 tick when a client is attached
    // and serves as the yield; otherwise we must yield explicitly.
    if (!(scopeService.state() == ServiceState::Running && scopeService.hasClient()))
        vTaskDelay(pdMS_TO_TICKS(1));
}

void systemRestart() {
    converter.disable();
    UART_LOG("Rebooting in 200ms");
    g_services.stopAll(); // tear down MQTT/telnet/etc. while WiFi is still up (see mqtt_task overflow)
    delay(200);
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
