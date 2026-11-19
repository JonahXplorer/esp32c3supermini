idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc log servo_radio)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SERVO_PEER_MAC="${SERVO_PEER_MAC}")
