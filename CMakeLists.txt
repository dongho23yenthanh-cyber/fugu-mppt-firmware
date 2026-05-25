cmake_minimum_required(VERSION 3.16)

# Catch WITH_* env-var typos early. CMake silently treats unknown $ENV{WITH_FOO} as empty,
# so e.g. WITH_BINRAY_TELE=1 would build with the option *off* and no warning.
set(_known_with_vars
        WITH_BLE WITH_NETW WITH_SPROFILER WITH_VCONV WITH_MCPWM WITH_BINARY_TELE)
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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fugu-firmware)

set (CMAKE_CXX_FLAGS "-fexceptions") # -mfp16-format=ieee

if (IDF_TARGET STREQUAL "esp32")
    set(FUGU_LITTLEFS_SRC config/lab/wokwi_mock_esp32)
else ()
    set(FUGU_LITTLEFS_SRC config/lab/dry_mock)
endif ()
littlefs_create_partition_image(littlefs ${FUGU_LITTLEFS_SRC}
        FLASH_IN_PROJECT
)

