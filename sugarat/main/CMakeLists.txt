idf_component_register(SRCS "main.c" "screen.c" "screen_port.c"
                       PRIV_REQUIRES esp_timer servo_radio sugarat_board sugarat_voice)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SERVO_PEER_MAC="${SERVO_PEER_MAC}")
