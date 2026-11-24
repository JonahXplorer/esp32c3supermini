#pragma once

#include <stddef.h>
#include <stdint.h>

static inline unsigned voice_audio_level(const int16_t *pcm, size_t samples)
{
    if (!samples) return 0;
    unsigned total = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int sample = pcm[i];
        total += sample < 0 ? -sample : sample;
    }
    const unsigned level = total / samples / 30;
    return level < 100 ? level : 100;
}
