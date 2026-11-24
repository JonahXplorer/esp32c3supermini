#pragma once

#include "lvgl.h"
#include "voice.h"

void screen_conversations_build(lv_obj_t *page);
void screen_conversations_open(bool open);
void screen_conversations_update(const sugarat_voice_view_t *voice);
