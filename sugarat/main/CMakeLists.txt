idf_component_register(SRCS "main.c" "manual_control.c" "screen.c" "screen_port.c"
                       PRIV_REQUIRES esp_timer servo_radio sugarat_board sugarat_voice agent_link)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SERVO_PEER_MAC="${SERVO_PEER_MAC}")
