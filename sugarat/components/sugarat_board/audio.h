#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

enum { SUGARAT_SAMPLE_RATE = 16000, SUGARAT_AUDIO_FRAME = 320 };
enum {
    SUGARAT_SPEAKER_DMA_DESCRIPTORS = 6,
    SUGARAT_SPEAKER_DMA_SAMPLES = SUGARAT_AUDIO_FRAME,
    SUGARAT_SPEAKER_DMA_MS = SUGARAT_SPEAKER_DMA_DESCRIPTORS * SUGARAT_SPEAKER_DMA_SAMPLES * 1000 / SUGARAT_SAMPLE_RATE,
};

typedef struct {
    unsigned completed;
    unsigned empty_slots;
} sugarat_speaker_dma_t;

esp_err_t sugarat_audio_init(void);
void sugarat_audio_deinit(void);
esp_err_t sugarat_mic_read(int16_t *pcm, size_t *samples);
esp_err_t sugarat_speaker_write(const int16_t *pcm, size_t samples);
sugarat_speaker_dma_t sugarat_speaker_dma(void);
void sugarat_speaker_enable(bool enabled);
