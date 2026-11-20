#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t voice_playback_start(void);
void voice_playback_stop(void);
void voice_playback_accept(bool enabled);
void voice_playback_queue(const uint8_t *pcm, size_t bytes, void *context);
void voice_playback_reset(void *context);
bool voice_playback_echo_guard(void);
