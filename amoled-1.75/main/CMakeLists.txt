set(app "${CMAKE_CURRENT_LIST_DIR}/../../sugarat/main")
idf_component_register(SRCS "${app}/main.c" "${app}/device_settings.c"
                            "${app}/manual_control.c" "${app}/screen.c"
                            "screen_port.c" "${app}/screen_conversations.c"
                            "ui/home_screen.c" "ui/home_presenter.c"
                       PRIV_INCLUDE_DIRS "${app}" "ui"
                       PRIV_REQUIRES esp_timer nvs_flash servo_radio sugarat_board sugarat_voice agent_link)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SERVO_PEER_MAC="${SERVO_PEER_MAC}" SUGARAT_NATIVE_AMOLED_UI=1)
