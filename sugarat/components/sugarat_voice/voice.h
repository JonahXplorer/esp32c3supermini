#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SUGARAT_VOICE_BOOT,
    SUGARAT_VOICE_SETUP,
    SUGARAT_VOICE_JOINING,
    SUGARAT_VOICE_ACTIVATE,
    SUGARAT_VOICE_BIND_AGENT,
    SUGARAT_VOICE_PREPARING,
    SUGARAT_VOICE_READY,
    SUGARAT_VOICE_OPENING,
    SUGARAT_VOICE_IN_CALL,
    SUGARAT_VOICE_RECONNECTING,
    SUGARAT_VOICE_ENDING,
    SUGARAT_VOICE_ERROR,
    SUGARAT_VOICE_RETRY,
} sugarat_voice_phase_t;

typedef struct {
    sugarat_voice_phase_t phase;
    char activation_code[7];
    bool can_end;
    bool speaking;
    uint8_t level;
} sugarat_voice_view_t;

esp_err_t sugarat_voice_start(void);
void sugarat_voice_start_call(void);
void sugarat_voice_view(sugarat_voice_view_t *view);
void sugarat_voice_end_call(void);
