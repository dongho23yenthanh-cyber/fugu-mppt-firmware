cmake_minimum_required(VERSION 3.16)

# Layer optional sdkconfig fragments on top of the defaults so flag-gated features (and their
# flash cost) are only baked into builds that ask for them. Order matters — later fragments win.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
# WITH_BLE=1 enables the NimBLE stack (and its flash cost). Keep in sync with the WITH_BLE
# compile def in main/CMakeLists.txt.
if (DEFINED ENV{WITH_BLE})
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.ble")
endif ()
# WITH_NETW=0 disables IDF networking (mbedtls / esp_http_client / esp_https_ota / mDNS /
# optionally WiFi+lwIP). Default (unset or non-zero) leaves the full network stack in. Keep in
# sync with the WITH_NETW define in main/CMakeLists.txt.
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

littlefs_create_partition_image(littlefs config/lab/dry_mock
        FLASH_IN_PROJECT
)

#littlefs_create_partition_image(littlefs config/lab/wokwi_mock
#        FLASH_IN_PROJECT
#)

