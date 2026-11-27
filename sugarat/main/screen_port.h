#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t screen_port_init(bool touch_ready);
#if defined(SUGARAT_NATIVE_AMOLED_UI)
// Wake the UI task at the LVGL deadline without rounding to a FreeRTOS tick.
void screen_port_wait_until(int64_t deadline_us);
#endif
