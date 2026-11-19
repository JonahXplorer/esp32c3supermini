cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/servo_radio")
set(SERVO_PEER_MAC "EC:DA:3B:67:13:E8" CACHE STRING "Sugarat Wi-Fi STA MAC")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(c3_dual_servo)
