#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t wake_word_init(void);
void wake_word_deinit(void);
bool wake_word_feed(const int16_t *pcm, size_t samples);
void wake_word_reset(void);
