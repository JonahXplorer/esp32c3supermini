#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t volume;
    uint8_t brightness;
} device_settings_t;

device_settings_t device_settings_load(void);
esp_err_t device_settings_save(device_settings_t settings);
