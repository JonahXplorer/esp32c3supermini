cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/servo_radio")
set(AGENT_LINK_FIRMWARE "${CMAKE_CURRENT_LIST_DIR}/../../Agent_link/firmware" CACHE PATH
    "Agent Link firmware directory containing the SDGOODS board support")
set(SERVO_PEER_MAC "EC:D6:1B:B6:09:30" CACHE STRING "ESP32-C3 Wi-Fi STA MAC")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(sugarat_servo_remote)
