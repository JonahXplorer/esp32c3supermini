#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t position;
    uint8_t speed;
    uint8_t angle_deg;
    uint8_t angle_limit_deg;
    bool online;
    bool pending;
    bool moving;
    bool touch_ready;
} screen_state_t;

typedef enum {
    SCREEN_NONE,
    SCREEN_SLOWER,
    SCREEN_FASTER,
    SCREEN_ANGLE_DOWN,
    SCREEN_ANGLE_UP,
    SCREEN_SWITCH,
} screen_action_t;

esp_err_t screen_init(void);
esp_err_t screen_draw(screen_state_t state);
screen_action_t screen_hit(int x, int y);
