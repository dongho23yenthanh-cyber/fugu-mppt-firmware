# Host-side e2e regression for the four SimpleFTPServer security fixes.
# Builds the vendored FtpServer.cpp against the reusable Arduino shim and
# drives it via raw TCP from the same process.

cmake_minimum_required(VERSION 3.16)
project(ftp_e2e CXX)

if(NOT TARGET arduino_shim)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../arduino-shim
                     ${CMAKE_BINARY_DIR}/arduino-shim)
endif()

set(FTP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../components/SimpleFTPServer/SimpleFTPServer)

add_executable(ftp_e2e_test
    test_ftp_e2e.cpp
    ${FTP_SRC}/FtpServer.cpp
)
target_include_directories(ftp_e2e_test PRIVATE ${FTP_SRC})
target_link_libraries(ftp_e2e_test PRIVATE arduino_shim)
target_compile_definitions(ftp_e2e_test PRIVATE
    ESP32
    FTP_SERVER_NETWORK_TYPE=6              # NETWORK_ESP32
    STORAGE_TYPE=7                         # STORAGE_LITTLEFS
    DEFAULT_FTP_SERVER_NETWORK_TYPE_ESP32=6
    DEFAULT_STORAGE_TYPE_ESP32=7
    ESP_ARDUINO_VERSION_MAJOR=3
)
target_compile_options(ftp_e2e_test PRIVATE
    -Wno-deprecated-declarations
)

enable_testing()
add_test(NAME ftp_e2e COMMAND ftp_e2e_test)
