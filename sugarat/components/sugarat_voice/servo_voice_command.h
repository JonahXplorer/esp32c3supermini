#pragma once

typedef enum {
    SERVO_VOICE_NONE,
    SERVO_VOICE_CLOSE,
    SERVO_VOICE_OPEN,
} servo_voice_command_t;

servo_voice_command_t servo_voice_command_parse(const char *transcript);
