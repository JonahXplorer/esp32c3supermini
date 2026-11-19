set(board "${AGENT_LINK_FIRMWARE}/boards/sdgoods-esp32s3")
set(drivers "${AGENT_LINK_FIRMWARE}/boards/common")

idf_component_register(SRCS "main.c" "board.c" "screen.c"
                            "${board}/sdgoods_panel_init.c" "${drivers}/co5300_qspi_io.c"
                       PRIV_INCLUDE_DIRS "${drivers}"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc
                                     esp_driver_spi esp_lcd esp_timer servo_radio)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SERVO_PEER_MAC="${SERVO_PEER_MAC}")
