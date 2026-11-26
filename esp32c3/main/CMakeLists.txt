idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_system log nvs_flash servo_radio)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           SERVO_PEER_MAC="${SERVO_PEER_MAC}"
                           SERVO_ADDITIONAL_PEER_MAC="${SERVO_ADDITIONAL_PEER_MAC}"
                           SERVO_AMOLED_PEER_MAC="${SERVO_AMOLED_PEER_MAC}"
                           SERVO_GPIO6_MAX_US=${SERVO_GPIO6_MAX_US}
                           SERVO_GPIO7_MAX_US=${SERVO_GPIO7_MAX_US})
