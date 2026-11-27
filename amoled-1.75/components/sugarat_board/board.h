#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// Keep credentials separate for each regional product, including on upgrades.
#define SUGARAT_CHINA_PRODUCT_ID 39
#define SUGARAT_CHINA_CREDENTIAL_NAMESPACE "al_cn_amoled39"
#define SUGARAT_OVERSEAS_PRODUCT_ID 50
#define SUGARAT_OVERSEAS_CREDENTIAL_NAMESPACE "al_os_amoled50"

// UI and touch coordinates use native panel pixels; screen_port rotates 90deg CW.
enum { SCREEN_SIZE = 466, AMOLED_PANEL_SIZE = 466,
       AMOLED_FRAME_PERIOD_MS = 33, AMOLED_TOUCH_PERIOD_MS = 10,
       AMOLED_TRANSFER_ROWS = 32,
       AMOLED_TRANSFER_PIXELS = AMOLED_PANEL_SIZE * AMOLED_TRANSFER_ROWS,
       AMOLED_TRANSFER_BYTES = AMOLED_TRANSFER_PIXELS * sizeof(uint16_t),
       SUGARAT_BRIGHTNESS_MIN = 5, SUGARAT_BRIGHTNESS_DEFAULT = 74 };

esp_err_t amoled_board_init(void);
i2c_master_bus_handle_t amoled_i2c_bus(void);
esp_err_t sugarat_display_init(void);
esp_err_t sugarat_display_write(int x, int y, int width, int height, const uint8_t *pixels);
esp_err_t sugarat_display_light(unsigned percent);
esp_err_t sugarat_display_set_brightness(unsigned percent);
esp_err_t sugarat_touch_init(void);
esp_err_t sugarat_touch_read(bool *pressed, int *x, int *y);
