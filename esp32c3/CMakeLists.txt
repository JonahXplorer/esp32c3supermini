cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/servo_radio")
set(SERVO_PEER_MAC "EC:DA:3B:67:12:2C" CACHE STRING "Sugarat Wi-Fi STA MAC")
set(SERVO_GPIO6_MAX_US 2500 CACHE STRING "Measured GPIO6 pulse at 145 degrees, or 0 until calibrated")
set(SERVO_GPIO7_MAX_US 500 CACHE STRING "Measured GPIO7 pulse at 145 degrees, or 0 until calibrated")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(c3_dual_servo)
