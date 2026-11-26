cmake_minimum_required(VERSION 3.22)

# Reuse the production application and transport, selecting only this board's BSP.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/servo_radio"
                         "${CMAKE_CURRENT_LIST_DIR}/../sugarat/third_party/agent_link"
                         "${CMAKE_CURRENT_LIST_DIR}/../sugarat/components/sugarat_voice")
set(ENV{AGENT_LINK_BUILD_BACKEND} "WIFI")
unset(ENV{AGENT_LINK_BUILD_PROFILE})
set(SERVO_PEER_MAC "EC:D6:1B:B6:09:30" CACHE STRING "ESP32-C3 Wi-Fi STA MAC")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(amoled_175_jarvis)
