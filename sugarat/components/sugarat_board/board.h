#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Both platforms use product 37, with separate credentials for each region.
#define SUGARAT_OVERSEAS_PRODUCT_ID 37
#define SUGARAT_OVERSEAS_CREDENTIAL_NAMESPACE "al_os_sugar37"
#define SUGARAT_CHINA_PRODUCT_ID 37
#define SUGARAT_CHINA_CREDENTIAL_NAMESPACE "al_cn_sugar37"

enum { SCREEN_SIZE = 360, SUGARAT_BRIGHTNESS_MIN = 5, SUGARAT_BRIGHTNESS_DEFAULT = 74 };

esp_err_t sugarat_display_init(void);
// Pixels are tightly packed RGB565 in panel byte order.
esp_err_t sugarat_display_write(int x, int y, int width, int height, const uint8_t *pixels);
esp_err_t sugarat_display_light(unsigned percent);
esp_err_t sugarat_display_set_brightness(unsigned percent);
esp_err_t sugarat_touch_init(void);
esp_err_t sugarat_touch_read(bool *pressed, int *x, int *y);
