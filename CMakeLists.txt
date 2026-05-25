cmake_minimum_required(VERSION 3.16)

# Catch WITH_* env-var typos early. CMake silently treats unknown $ENV{WITH_FOO} as empty,
# so e.g. WITH_BINRAY_TELE=1 would build with the option *off* and no warning.
set(_known_with_vars
        WITH_BLE WITH_NETW WITH_SPROFILER WITH_VCONV WITH_MCPWM WITH_BINARY_TELE
        WITH_MEASURE_COIL)
execute_process(COMMAND ${CMAKE_COMMAND} -E environment OUTPUT_VARIABLE _all_env)
string(REPLACE "\n" ";" _all_env "${_all_env}")
foreach (_line IN LISTS _all_env)
    if (_line MATCHES "^(WITH_[A-Za-z0-9_]+)=")
        if (NOT CMAKE_MATCH_1 IN_LIST _known_with_vars)
            message(FATAL_ERROR
                    "Unknown env var ${CMAKE_MATCH_1}. Known: ${_known_with_vars}")
        endif ()
    endif ()
endforeach ()

# Layer optional sdkconfig fragments on top of the defaults so flag-gated features (and their
# flash cost) are only baked into builds that ask for them. Order matters — later fragments win.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
# BLE on by default; set WITH_BLE=0 to skip the NimBLE sdkconfig fragment.
# Keep in sync with the WITH_BLE define in main/CMakeLists.txt.
set(WITH_BLE 1)
if (DEFINED ENV{WITH_BLE} AND NOT $ENV{WITH_BLE})
    set(WITH_BLE 0)
endif ()
if (WITH_BLE)
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.ble")
endif ()
# WITH_NETW=0 disables IDF networking (mbedtls / esp_http_client / esp_https_ota / mDNS / optionally WiFi+lwIP).
# Keep in sync with the WITH_NETW define in main/CMakeLists.txt.
if (DEFINED ENV{WITH_NETW} AND NOT $ENV{WITH_NETW})
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.no_netw")
endif ()

# The sampling profiler (components/esp32-semihosting-profiler) is opt-in via WITH_SPROFILER=1.
# When off (default), drop the whole component so its flash + ~8 KB .bss isn't linked; main.cpp
# guards its use with #ifdef WITH_SPROFILER. Keep in sync with main/CMakeLists.txt.
if (NOT DEFINED ENV{WITH_SPROFILER})
    set(EXCLUDE_COMPONENTS esp32-semihosting-profiler)
endif ()

# arduino-esp32 declares a lot of optional managed deps (RainMaker / Insights / Modem / ESP-SR /
# Zigbee / Modbus / etc.) that we never use. With ARDUINO_SELECTIVE_COMPILATION (sdkconfig.defaults)
# we already drop the corresponding arduino library .cpp's, so the only references left are inside
# the unused managed components themselves — exclude those archives so they don't compile or link.
# Kept: cbor (transitive header), mdns, network_provisioning (auto-selected by ESP_WIFI_ENABLED).
# Unused arduino-esp32 transitive deps (RainMaker / Insights / Modem / ESP-SR / Zigbee / Modbus
# / etc.) are stubbed out via override_path in main/idf_component.yml. EXCLUDE_COMPONENTS doesn't
# work for managed components, so the override-to-empty-stub is the next-best lever.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fugu-firmware)

# arduino-esp32's CMakeLists.txt doesn't list esp_wifi / network_provisioning in REQUIRES even
# though its public WiFi.h → WiFiType.h transitively includes esp_wifi_types.h and (when
# CONFIG_NETWORK_PROV_NETWORK_TYPE_WIFI=y, auto-selected from ESP_WIFI_ENABLED) WiFiGeneric.h
# includes network_provisioning/manager.h. Normally this works only because some unrelated
# managed transitive dep happens to PUBLIC-require these; stubbing the optional deps (RainMaker
# etc.) breaks that chain and the IDF strict-include checker errors any TU that touches WiFi.h.
# Patch by appending the missing PUBLIC requires from here.
idf_component_get_property(_arduino_lib espressif__arduino-esp32 COMPONENT_LIB)
target_link_libraries(${_arduino_lib} PUBLIC
        idf::esp_wifi idf::esp_event idf::esp_netif
        idf::espressif__network_provisioning)

set (CMAKE_CXX_FLAGS "-fexceptions") # -mfp16-format=ieee

if (IDF_TARGET STREQUAL "esp32")
    set(FUGU_LITTLEFS_SRC config/fugu1/fugu1_esp32)
else ()
    set(FUGU_LITTLEFS_SRC config/lab/dry_mock)
endif ()
littlefs_create_partition_image(littlefs ${FUGU_LITTLEFS_SRC}
        FLASH_IN_PROJECT
)

