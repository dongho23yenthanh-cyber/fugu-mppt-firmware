
if ($ENV{RUN_TESTS})
    set(MAIN_SRC "../test/main.cpp"
            "../test/test_meter.cpp"
            "../test/test_sampler.cpp"
            "../test/test_math.cpp"
            "../test/test_charger.cpp"
            "../test/test_ascii.cpp"
    )
elseif ($ENV{MAIN_SRC})
    set(MAIN_SRC $ENV{MAIN_SRC})
else ()
    # cli.cpp externs main.cpp's globals, so it's only built alongside the real main (not test mains).
    set(MAIN_SRC "../src/main.cpp" "../src/cli.cpp")
endif ()

idf_component_register(SRCS
        ${MAIN_SRC}
        "../src/sensor_setup.cpp"
        "../src/mppt.cpp"
        "../src/logging.cpp"
        "../src/util.cpp"
        "../src/adc/adc_esp32_cont.cpp"
        "../src/viz/lcd.cpp"
        "../src/tele/telemetry.cpp"
        "../src/tele/ftp_service.cpp"
        "../src/tele/telnet_service.cpp"
        "../src/tele/scope_service.cpp"
        "../src/web/server.cpp"
        "../src/console.cpp"
        "../src/console_ble.cpp"
        "../src/etc/rt.cpp"
        "../src/tele/mqtt.cpp"
        "../src/etc/ota.cpp"
        "../src/tele/HAMqttDevice.cpp"
        "../src/tele/home_assistant.cpp"
        "../src/math/float16.cpp"
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


if ($ENV{FUGU_BAT_V})
    # 14.25, 28.5, 57
    component_compile_definitions("FUGU_BAT_V=$ENV{FUGU_BAT_V}")
endif ()

# WITH_BLE=1 compiles the BLE NUS console (console_ble.cpp) and registers BleConsoleService.
# Must be paired with the NimBLE sdkconfig (top-level CMakeLists pulls in sdkconfig.ble).
if ($ENV{WITH_BLE})
    component_compile_definitions("WITH_BLE=1")
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
