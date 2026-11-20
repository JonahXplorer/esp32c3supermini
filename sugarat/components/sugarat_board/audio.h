#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

enum { SUGARAT_SAMPLE_RATE = 16000, SUGARAT_AUDIO_FRAME = 320 };

esp_err_t sugarat_audio_init(void);
void sugarat_audio_deinit(void);
esp_err_t sugarat_mic_read(int16_t *pcm, size_t *samples);
esp_err_t sugarat_speaker_write(const int16_t *pcm, size_t samples);
void sugarat_speaker_enable(bool enabled);
