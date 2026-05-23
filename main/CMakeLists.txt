
if ($ENV{RUN_TESTS})
    set(MAIN_SRC "../test/main.cpp"
            "../test/test_meter.cpp"
            "../test/test_sampler.cpp"
            "../test/test_math.cpp"
            "../test/test_charger.cpp"
            "../test/test_ascii.cpp"
            "../test/test_statmath.cpp"
            "../test/test_pd_control.cpp"
            "../test/test_buck.cpp"
            "../test/test_conf.cpp"
    )
elseif ($ENV{MAIN_SRC})
    set(MAIN_SRC $ENV{MAIN_SRC})
else ()
    # cli.cpp and measure_coil.cpp extern main.cpp's globals, so they're only built alongside the
    # real main (not test mains).
    set(MAIN_SRC "../src/main.cpp" "../src/cli.cpp" "../src/measure_coil.cpp")
endif ()

set(NETW_SRC
        "../src/tele/telemetry.cpp"
        "../src/tele/ftp_service.cpp"
        "../src/tele/telnet_service.cpp"
        "../src/tele/scope_service.cpp"
        "../src/tele/mqtt.cpp"
        "../src/tele/HAMqttDevice.cpp"
        "../src/tele/home_assistant.cpp"
        "../src/tele/tamp_compress.cpp"
        "../src/web/server.cpp"
        "../src/etc/ota.cpp"
)

# WITH_NETW=1 (default) compiles the WiFi/mDNS/MQTT/telemetry/HTTPS-OTA path. WITH_NETW=0 drops
# all of the above; the firmware then runs UART/USB/BLE consoles only. Reverse polarity to
# WITH_BLE on purpose — backwards-compat default is "network on". Keep in sync with the
# sdkconfig.no_netw layering in the top-level CMakeLists.
set(WITH_NETW 1)
if (DEFINED ENV{WITH_NETW} AND NOT $ENV{WITH_NETW})
    set(WITH_NETW 0)
endif ()

if (NOT WITH_NETW)
    set(NETW_SRC "")
endif ()

idf_component_register(SRCS
        ${MAIN_SRC}
        "../src/sensor_setup.cpp"
        "../src/mppt.cpp"
        "../src/logging.cpp"
        "../src/util.cpp"
        "../src/adc/adc_esp32_cont.cpp"
        "../src/viz/lcd.cpp"
        "../src/console.cpp"
        "../src/console_ble.cpp"
        "../src/etc/rt.cpp"
        "../src/etc/ota_ble.cpp"
        "../src/math/float16.cpp"
        ${NETW_SRC}
        INCLUDE_DIRS "." "../src"
        REQUIRES
        # if we start to list components here we need to list them all
        # put dependencies in component/Arduion-LiquidCrystal as only needed for linking
        #arduino
        #esp_system
        #Arduino-LiquidCrystal-I2C
        #arduino
        #esp_system
        # NetworkClientSecure # OTA
)

if (WITH_NETW)
    component_compile_definitions("WITH_NETW=1")
endif ()


if ($ENV{FUGU_BAT_V})
    # 14.25, 28.5, 57
    component_compile_definitions("FUGU_BAT_V=$ENV{FUGU_BAT_V}")
endif ()

# WITH_BLE=1 compiles the BLE NUS console (console_ble.cpp) and registers BleConsoleService.
# Must be paired with the NimBLE sdkconfig (top-level CMakeLists pulls in sdkconfig.ble).
if ($ENV{WITH_BLE})
    component_compile_definitions("WITH_BLE=1")
endif ()

# WITH_MCPWM=1 swaps the LEDC gate driver for the MCPWM driver (src/pwm/mcpwm.h):
# hardware dead-time, GPIO OST brake, glitch-free comparator updates. See doc/mcpwm-sync-buck-driver.md.
if ($ENV{WITH_MCPWM})
    component_compile_definitions("WITH_MCPWM=1")
endif ()

# WITH_BINARY_TELE=1 sends telemetry as the binary symbol-table wire (sym_line_protocol.h) +
# compression (tele.conf compressor=none|tamp, default tamp) instead of text influx line protocol.
# The receiver on UDP :8086 must then strip the 1-byte compressor id, decompress, and decode the
# length-prefixed frames (lpDecode is the reference) — plain InfluxDB ingestion no longer works.
if ($ENV{WITH_BINARY_TELE})
    component_compile_definitions("WITH_BINARY_TELE=1")
endif ()

# BENCH_TELE=1 runs a one-shot encode/compress microbench at boot (benchTele, prints via
# ESP_LOGW("bench", ...)). Requires WITH_BINARY_TELE (compares LineProtocol vs BinaryLineProtocol).
if ($ENV{BENCH_TELE})
    component_compile_definitions("BENCH_TELE=1")
endif ()

# WITH_SPROFILER=1 compiles in the semihosting sampling profiler (sprofiler_initialize, only useful
# with OpenOCD attached). Off by default; the top-level CMakeLists then EXCLUDE_COMPONENTS the
# esp32-semihosting-profiler component so its flash + ~8 KB .bss is dropped entirely.
if ($ENV{WITH_SPROFILER})
    component_compile_definitions("WITH_SPROFILER=1")
endif ()

component_compile_options(-Werror=missing-field-initializers)
component_compile_options(-Werror=attributes) # IRAM_ATTR used in forward decl

component_compile_options(--std=gnu++20)
component_compile_options(
      #  -fsanitize=address
      #  -fsanitize=shadow-call-stack # esp32: not supported
      #  -fsanitize=thread
        #-fsanitize=leak
        #-fsanitize=undefined -fno-sanitize=shift-base
      #  -fsanitize-address-use-after-scope
)

#target_compile_options(__idf_FastLED PRIVATE -Wno-error=reorder)

#component_compile_options(-fexceptions) # enable exceptions in menuconfig (CONFIG_COMPILER_CXX_EXCEPTIONS)!
