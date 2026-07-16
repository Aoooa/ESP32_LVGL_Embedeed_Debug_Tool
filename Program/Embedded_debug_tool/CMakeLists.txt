# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 组件目录与 main 同级
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/drv_uart"
    "${CMAKE_CURRENT_SOURCE_DIR}/drv_wifi"
    "${CMAKE_CURRENT_SOURCE_DIR}/app_bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/app_tcp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app_web"
)

idf_build_set_property(MINIMAL_BUILD ON)
project(Embedded_debug_tool)
