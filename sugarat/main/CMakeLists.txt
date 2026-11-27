idf_component_register(SRCS "main.c" "device_settings.c" "manual_control.c" "screen.c" "screen_port.c" "screen_conversations.c"
                            "ui/home_screen.c" "ui/home_presenter.c" "ui/ui_button.c"
                       PRIV_INCLUDE_DIRS "." "ui"
                       PRIV_REQUIRES esp_timer esp_wifi nvs_flash servo_radio sugarat_board sugarat_voice agent_link)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SERVO_PEER_MAC="${SERVO_PEER_MAC}")
