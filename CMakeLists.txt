cmake_minimum_required(VERSION 3.16)

# WITH_BLE=1 layers the NimBLE config on top of the defaults, so the BLE stack (and its flash cost)
# is only baked in for BLE builds. Keep in sync with the WITH_BLE compile def in main/CMakeLists.txt.
if (DEFINED ENV{WITH_BLE})
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.ble")
endif ()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fugu-firmware)

set (CMAKE_CXX_FLAGS "-fexceptions") # -mfp16-format=ieee

littlefs_create_partition_image(littlefs config/fmetal
#        FLASH_IN_PROJECT
)

