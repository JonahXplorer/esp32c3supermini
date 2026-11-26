idf_component_register(SRCS "board.c" "touch.c" "audio.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../../sugarat/components/sugarat_board/drivers/co5300_qspi_io.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../sugarat/components/sugarat_board/drivers"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_spi esp_lcd esp_timer)
