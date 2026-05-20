
#include "logging.h"

#include <Arduino.h>
#include <Wire.h>
#include <USB.h>


#include "adc/sampling.h"

#include "console.h"
#include "console_ble.h"
#include <SimpleCLI.h>
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
#include <sprofiler.h>

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

// Console command table (SimpleCLI). Built once by setupCli(); handleCommand() feeds it whole
// command lines from every transport (UART/USB/telnet/BLE). See doc/Console.md.
static SimpleCLI cli;
void setupCli();

static void loopNetwork_task(void *arg);

static void loopRT(void *arg); // this is the critical one

static void loopRTNewData(unsigned long nowMs);


const char* VER_STRING = "*** Fugu Firmware Version " FIRMWARE_VERSION " (" __DATE__ " " __TIME__ ")";

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


    // A malformed board.conf must not abort setup() (which would reboot and re-read the same bad
    // file → boot loop). Fall back to an empty config and run the safe-idle path via setupErr.
    ConfFile boardConf;
    try {
        boardConf = ConfFile{"/littlefs/conf/board.conf"};
    } catch (const std::exception &e) {
        ESP_LOGE("main", "error reading board.conf: %s", e.what());
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
            ESP_LOGE("main", "error reading tele.conf: %s", e.what());
        }
    }

    Limits lim{};
    try {
        lim = Limits{ConfFile{"/littlefs/conf/limits.conf"}};
    } catch (const std::runtime_error &er) {
        ESP_LOGE("main", "error reading limits.conf: %s", er.what());
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

static void stopAndBackoff(uint32_t secondsDelay) {
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
                    ESP_LOGE("main", "Timeout waiting for new ADC sample, shutdown! numSamples=%lu dt=%lu ms",
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

static void loopLF(const unsigned long &nowUs) {
    auto &nSamples(sensors.Vout ? sensors.Vout->numSamples : lastNSamples);
    auto dt = nowUs - lastTimeOutUs;
    uint32_t sps = (dt > 20000) ? (uint64_t) (nSamples - lastNSamples) * 1000000llu / dt : 0;


    if ((dt > (lfPeriod * 0.9f)) && sps < loopRateMin && !converter.disabled() &&
        nSamples > max(loopRateMin * 5, 200) &&
        !manualPwm && lastTimeOutUs && (nowUs - adcSampler.getTimeLastCalibrationUs()) > 2000000) {
        auto loopRunTime = (nowUs - adcSampler.getTimeLastCalibrationUs());
        ESP_LOGE("main", "Loop latency too high (%lu < %hu Hz), shutdown! (nSamples=%lu, D=%u, loopRunTime=%.1fs )",
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

/*
void loopCore0_LF(void *arg) {
    // do everything with poor real-time performance @ 1Hz

    while (1) {
        // nothing here yet
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
 */

void systemRestart() {
    converter.disable();
    UART_LOG("Rebooting in 200ms");
    g_services.stopAll(); // tear down MQTT/telnet/etc. while WiFi is still up (see mqtt_task overflow)
    delay(200);
    ESP.restart();
}

// --- Console command callbacks ---------------------------------------------------------------
// One handler per command, registered in setupCli(). SimpleCLI dispatches by the first token and
// hands us a `cmd*`; wrap it in Command to read arguments. Validation that used to live in the
// old if-chain conditions now lives in the handler body: on bad input we ESP_LOGW and return
// (callbacks are void, so per-command failure can't propagate a bool — handleCommand only reports
// parser-level errors like unknown command). Output still goes through the global ESP_LOG/UART_LOG
// path (dispatch-only refactor; routing unchanged).

static void cmdSync(cmd *c) {
    if (!manualPwm) { ESP_LOGW("main", "sync: only in manual PWM (use 'dc N' first)"); return; }
    auto arg = Command(c).getArg(0).getValue();
    if (arg == "on" or arg == "1" or arg == "off" or arg == "0") {
        converter.forcedPwm_(false);
        converter.enableSyncRect(arg == "on" or arg == "1", true);
    } else if (arg == "forced") {
        converter.enableSyncRect(true);
        converter.forcedPwm_(true);
    } else {
        ESP_LOGW("main", "sync: expected on|off|forced");
    }
}

static void cmdBflow(cmd *c) {
    if (!manualPwm) { ESP_LOGW("main", "bf: only in manual PWM (use 'dc N' first)"); return; }
    if (!mppt.bflow) { ESP_LOGW("main", "panel switch not configured"); return; }
    auto newState = Command(c).getArg(0).getValue().toInt();
    if (newState != 0 and newState != 1) { ESP_LOGW("main", "bf: expected 0|1"); return; }
    if (mppt.bflow.state() != newState)
        ESP_LOGI("main", "Set bflow state %i", (int) newState);
    mppt.bflow.enable(newState);
}

static void cmdRestart(cmd *) { systemRestart(); }

static void cmdMppt(cmd *) {
    if (!manualPwm) { ESP_LOGW("main", "MPPT already enabled"); return; }
    ESP_LOGI("main", "MPPT re-enabled");
    manualPwm = false;
}

static void cmdDc(cmd *c) {
    if (adcSampler.isCalibrating()) { ESP_LOGW("main", "dc: busy calibrating"); return; }
    auto v = Command(c).getArg(0).getValue();
    if (v.length() == 0) { ESP_LOGW("main", "dc: expected duty cycle"); return; }
    auto dc = v.toInt();
    if (dc < 0 || dc > converter.pwmCtrlMax) { ESP_LOGW("main", "dc: out of range [0,%i]", (int) converter.pwmCtrlMax); return; }

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
        ESP_LOGW("main", "short-ls: requires boost mode and Vin~0");
    }
}

static void cmdSpeed(cmd *c) {
    float speedScale = Command(c).getArg(0).getValue().toFloat();
    if (speedScale >= 0 && speedScale < 10) {
        mppt.speedScale = speedScale;
        ESP_LOGI("main", "Set tracker speed scale %.4f", speedScale);
    } else {
        ESP_LOGW("main", "speed: out of range [0,10)");
    }
}

static void cmdFan(cmd *c) {
    if (!mppt.fan.fanSet(Command(c).getArg(0).getValue().toFloat() * 0.01f))
        ESP_LOGW("main", "fan: set failed");
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
        ESP_LOGW("main", "wifi: expected on|off");
    }
}

static void cmdWifiAdd(cmd *c) {
    auto ssidAndPw = Command(c).getArg(0).getValue();
    auto i = ssidAndPw.indexOf(':');
    if (i <= 0) { ESP_LOGW("main", "wifi-add: expected ssid:password"); return; }
    std::string ssid = ssidAndPw.substring(0, i).c_str();
    auto psk = ssidAndPw.substring(i + 1);
    ESP_LOGI("main", "adding wifi network %s (psk=%s)", ssid.c_str(), psk.c_str());
    add_ap(ssid, psk.c_str());
}

static void cmdScanI2c(cmd *) { scan_i2c(); }

static void cmdLs(cmd *) { ESP_LOGE("main", "not impl"); }

static void cmdOta(cmd *c) {
    auto url = Command(c).getArg(0).getValue();
    if (url.length() == 0) { ESP_LOGW("main", "ota: expected url"); return; }
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

static void cmdHostname(cmd *c) {
    auto hn = Command(c).getArg(0).getValue();
    if (hn.length() == 0) { ESP_LOGW("main", "hostname: expected name"); return; }
    nvs.open();
    nvs.writeString("hostname", hn.c_str());
    nvs.close();
}

// set-config <file> <key> <value...>  — value may contain spaces, so join the trailing tokens.
//   set-config coil.conf L0 50            set-config mqtt.conf broker_uri mqtt://192.168.1.134:1882
//   set-config limits.conf iout_max 35    set-config charger.conf cell_voltage_eoc 3.53
static void cmdSetConfig(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 3) { ESP_LOGW("main", "set-config: expected <file> <key> <value>"); return; }
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

// get-config <file> [key]  — print one key, or dump the whole file.
static void cmdGetConfig(cmd *c) {
    Command cc(c);
    if (cc.countArgs() < 1) { ESP_LOGW("main", "get-config: expected <file> [key]"); return; }
    auto fn = "/littlefs/conf/" + cc.getArg(0).getValue();
    ConfFile conf{fn.c_str()};
    if (cc.countArgs() >= 2) {
        auto key = cc.getArg(1).getValue();
        auto val = conf.getString(key.c_str(), "");
        printf("Conf '%s:%s' = '%s'\n", fn.c_str(), key.c_str(), val.c_str());
    } else {
        for (const auto &key: conf.keys())
            ESP_LOGI("main", "Conf '%s:%s' = '%s'", fn.c_str(), key.c_str(), conf.getString(key).c_str());
    }
}

static void cmdVset(cmd *c) {
    float v = Command(c).getArg(0).getValue().toFloat();
    if (v >= 0 and v <= 999) mppt.charger.params.Vbat_max = v;
    else ESP_LOGW("main", "vset: out of range [0,999]");
}

static void cmdIset(cmd *c) {
    float v = Command(c).getArg(0).getValue().toFloat();
    if (v >= 0 and v <= 999) mppt.charger.params.Ibat_lim = v;
    else ESP_LOGW("main", "iset: out of range [0,999]");
}

// service [list]                       service start|stop|restart|reload <name>
// service log <name> <error|warn|info>
static void cmdService(cmd *c) {
    Command cc(c);
    int n = cc.countArgs();
    String sub = n >= 1 ? cc.getArg(0).getValue() : String("list");

    if (sub == "list") {
        UART_LOG("%-10s %-8s %-6s %-8s %s", "NAME", "STATE", "LOG", "ENABLED", "DETAIL");
        for (auto *s: g_services.all()) {
            auto detail = s->statusDetail();
            UART_LOG("%-10s %-8s %-6s %-8s %s", s->name(), stateStr(s->state()),
                     levelToStr(s->logLevel()), s->enabled() ? "yes" : "no", detail.c_str());
        }
        return;
    }

    if (n < 2) { ESP_LOGW("main", "service: expected <name>"); return; }
    auto name = cc.getArg(1).getValue();
    auto *s = g_services.findByName(name.c_str());
    if (!s) { ESP_LOGW("main", "service: unknown '%s'", name.c_str()); return; }

    if (sub == "start") {
        s->setEnabledPersist(true);
        if (!s->start()) UART_LOG("start failed (state=%s)", stateStr(s->state()));
    } else if (sub == "stop") {
        s->setEnabledPersist(false);
        s->stop();
    } else if (sub == "restart") {
        s->restart();
    } else if (sub == "log") {
        if (n < 3) { ESP_LOGW("main", "service log: expected <error|warn|info>"); return; }
        s->setLogLevel(strToLevel(cc.getArg(2).getValue().c_str()), /*persist*/ true);
    } else {
        ESP_LOGW("main", "service: unknown subcommand '%s'", sub.c_str());
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
    cli.addSingleArgCmd("hostname", cmdHostname);
    cli.addSingleArgCmd("vset", cmdVset);
    cli.addSingleArgCmd("iset", cmdIset);

    cli.addBoundlessCmd("set-config", cmdSetConfig);
    cli.addBoundlessCmd("get-config", cmdGetConfig);
    cli.addBoundlessCmd("service", cmdService);
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

    cli.parse(inp);
    if (cli.errored()) {
        ESP_LOGE("main", "%s", cli.getError().toString().c_str()); // unknown command / parse error
        return false;
    }

    loopLF(wallClockUs());
    return true;
}

[[maybe_unused]] void esp_task_wdt_isr_user_handler() {
    //throw std::runtime_error("reboot");
    if (esp_cpu_dbgr_is_attached()) return;

    enqueue_task([] {
        ESP_LOGE("main", "Restart after WDT trigger");
        systemRestart();
    });
}
