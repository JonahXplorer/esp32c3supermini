set(app "${CMAKE_CURRENT_LIST_DIR}/../../sugarat/main")
idf_component_register(SRCS "${app}/main.c" "${app}/device_settings.c"
                            "${app}/manual_control.c" "${app}/screen.c"
                            "screen_port.c" "frame_rotate.c" "${app}/screen_conversations.c"
                            "${app}/ui/home_screen.c" "${app}/ui/home_presenter.c" "${app}/ui/ui_button.c"
                       PRIV_INCLUDE_DIRS "${app}" "${app}/ui"
                       PRIV_REQUIRES esp_timer nvs_flash servo_radio sugarat_board sugarat_voice agent_link)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SERVO_PEER_MAC="${SERVO_PEER_MAC}" SUGARAT_NATIVE_AMOLED_UI=1)

# Optimize the display hot path without changing voice/AEC compiler options.
idf_component_get_property(lvgl_target lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_target} PRIVATE -O2)
set_source_files_properties("screen_port.c" "frame_rotate.c" "${app}/ui/home_screen.c" "${app}/screen.c"
                            PROPERTIES COMPILE_OPTIONS "-O2")
