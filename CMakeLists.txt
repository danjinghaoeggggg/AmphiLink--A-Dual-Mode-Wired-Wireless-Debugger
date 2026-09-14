# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(COMPONENT_DIRS
    "$ENV{IDF_PATH}/components"
    "${CMAKE_CURRENT_LIST_DIR}/main"
    "${CMAKE_CURRENT_LIST_DIR}/components/BSP"
    "${CMAKE_CURRENT_LIST_DIR}/components/Module"
    "${CMAKE_CURRENT_LIST_DIR}/components/APP"
)

set(PROJECT_VER "1.1.0")

if(NOT DEFINED SDKCONFIG_DEFAULTS AND "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
    # choose one of the following sdkconfig files to use as default by uncommenting it and commenting the others
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.octal-psram")
    # set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.no-psram")
    # set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.quad-psram")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(AmphiLink)
