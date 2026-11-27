#pragma once

#include "driver/gpio.h"

enum {
    AMPLIFIER_ENABLE = GPIO_NUM_3,
    TOUCH_RESET = GPIO_NUM_5,
    POWER_KEY = GPIO_NUM_6,
    BATTERY_LATCH = GPIO_NUM_7,
    LCD_RESET = GPIO_NUM_9,
    LCD_POWER = GPIO_NUM_12,
    LCD_BACKLIGHT = GPIO_NUM_13,
};
