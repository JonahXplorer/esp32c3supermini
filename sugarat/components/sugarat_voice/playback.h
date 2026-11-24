#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "agent_link.h"
#include "esp_err.h"

esp_err_t voice_playback_start(void);
void voice_playback_stop(void);
void voice_playback_accept(bool enabled, const char *reset_reason);
void voice_playback_queue(const uint8_t *pcm, size_t bytes, void *context);
void voice_playback_end(void *context);
void voice_playback_reset(agent_audio_reset_reason_t reason, uint32_t reset_seq, void *context);
bool voice_playback_echo_guard(void);
unsigned voice_playback_level(void);
