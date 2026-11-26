#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// The original 360px interface is scaled to the native 466px AMOLED in screen_port.
enum { SCREEN_SIZE = 360, AMOLED_PANEL_SIZE = 466,
       SUGARAT_BRIGHTNESS_MIN = 5, SUGARAT_BRIGHTNESS_DEFAULT = 74 };

esp_err_t amoled_board_init(void);
i2c_master_bus_handle_t amoled_i2c_bus(void);
esp_err_t sugarat_display_init(void);
esp_err_t sugarat_display_write(int x, int y, int width, int height, const uint8_t *pixels);
esp_err_t sugarat_display_light(unsigned percent);
esp_err_t sugarat_display_set_brightness(unsigned percent);
esp_err_t sugarat_touch_init(void);
esp_err_t sugarat_touch_read(bool *pressed, int *x, int *y);
