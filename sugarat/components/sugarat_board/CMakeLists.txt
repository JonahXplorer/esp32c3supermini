idf_component_register(SRCS "board.c" "audio.c" "drivers/sdgoods_panel_init.c" "drivers/co5300_qspi_io.c"
                       INCLUDE_DIRS "." PRIV_INCLUDE_DIRS "drivers"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s
                                     esp_driver_ledc esp_driver_spi esp_lcd esp_timer)
