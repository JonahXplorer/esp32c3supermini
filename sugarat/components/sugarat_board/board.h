#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum { SCREEN_SIZE = 360 };

esp_err_t sugarat_display_init(void);
// Pixels are tightly packed RGB565 in panel byte order.
esp_err_t sugarat_display_write(int x, int y, int width, int height, const uint8_t *pixels);
esp_err_t sugarat_display_light(void);
esp_err_t sugarat_touch_init(void);
esp_err_t sugarat_touch_read(bool *pressed, int *x, int *y);
