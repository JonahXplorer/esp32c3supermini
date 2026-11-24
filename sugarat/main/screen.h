#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "voice.h"

typedef struct {
    uint8_t position;
    uint8_t speed;
    uint8_t angle_deg;
    uint8_t angle_limit_deg;
    uint8_t receiver_channel;
    uint8_t light;
    bool online;
    bool pending;
    bool moving;
    bool touch_ready;
    sugarat_voice_view_t voice;
} screen_state_t;

typedef enum {
    SCREEN_NONE,
    SCREEN_SET_SPEED,
    SCREEN_SET_ANGLE,
    SCREEN_SET_TARGET,
    SCREEN_SET_LIGHT,
    SCREEN_START_CALL,
    SCREEN_END_CALL,
} screen_action_kind_t;

typedef struct {
    screen_action_kind_t kind;
    uint8_t value;
} screen_action_t;

esp_err_t screen_init(bool touch_ready);
void screen_update(const screen_state_t *state);
screen_action_t screen_poll(void);
