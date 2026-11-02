cmake_minimum_required(VERSION 3.16)

# PlatformIO's Espressif32 platform invokes this ESP-IDF project through
# its managed framework package. Keep this project/component structure;
# there is intentionally no standalone idf.py workflow or board matrix.

execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIT_DESCRIBE_RC
    ERROR_QUIET
)
if(NOT GIT_DESCRIBE_RC EQUAL 0 OR NOT PROJECT_VER)
    set(PROJECT_VER "0.0.0-unknown")
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD"
        "${CMAKE_CURRENT_LIST_DIR}/.git/index")
endif()

set(BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/devkitc_s3")
list(APPEND SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${BOARD_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR
        "LabelSis QL-700 firmware targets ESP32-S3 only; current IDF_TARGET=\"${IDF_TARGET}\". "
        "Select the PlatformIO ql700 environment.")
endif()

message(STATUS "LabelSis: Brother QL-700 / ESP32-S3 / PlatformIO")
project(labelsis)
