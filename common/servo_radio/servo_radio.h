#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum {
    SERVO_RADIO_CHANNEL = 6,
    SERVO_SPEED_MIN = 10,
    SERVO_SPEED_MAX = 50,
    SERVO_SPEED_DEFAULT = 25,
    SERVO_SPEED_INCREMENT = 5,
    SERVO_ANGLE_DEFAULT = 90,
    SERVO_ANGLE_MAX = 145,
    SERVO_ANGLE_INCREMENT = 5,
};

typedef enum {
    SERVO_QUERY = 1,
    SERVO_SET_POSITION,
    SERVO_SET_SPEED,
    SERVO_SET_ANGLE,
    SERVO_STATE,
} servo_message_type_t;

typedef struct {
    uint32_t protocol;
    uint32_t session;
    uint32_t sequence;
    uint8_t type;
    uint8_t position;
    uint8_t speed;
    uint8_t moving;
    uint8_t angle_deg;
    uint8_t angle_limit_deg;
    uint8_t reserved[2];
} servo_message_t;

esp_err_t servo_radio_start(const char *peer_mac);
bool servo_radio_receive(servo_message_t *message);
esp_err_t servo_radio_send(servo_message_t message);
