# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 只编译 main 与它真正依赖的组件，不编 ESP-IDF 全量源码（省内存与时间）
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_turret)