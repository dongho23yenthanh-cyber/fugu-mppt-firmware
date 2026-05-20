cmake_minimum_required(VERSION 3.16)

# WITH_BLE=1 layers the NimBLE config on top of the defaults, so the BLE stack (and its flash cost)
# is only baked in for BLE builds. Keep in sync with the WITH_BLE compile def in main/CMakeLists.txt.
if (DEFINED ENV{WITH_BLE})
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.ble")
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

littlefs_create_partition_image(littlefs config/fmetal
#        FLASH_IN_PROJECT
)

