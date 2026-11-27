#pragma once

#include "lvgl.h"
#include "ui_theme.h"

enum {
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    BG = 0x05070C,
    PANEL = 0x151B27,
    EDGE = 0x283345,
    TRACK = 0x344156,
    TEXT = 0xF3F6FC,
    MUTED = 0xA2AEC3,
    TEAL = 0x65EBDB,
    INK = 0x112B2C,
    LILAC = 0xB0A0FF,
    AMBER = 0xFFCB80,
    RED = 0xFF9EAA,
#else
    BG = UI_BG_RGB,
    PANEL = UI_SURFACE_RGB,
    EDGE = UI_BORDER_RGB,
    TRACK = UI_SURFACE_PRESSED_RGB,
    TEXT = UI_TEXT_RGB,
    MUTED = UI_TEXT_DIM_RGB,
    TEAL = UI_ACCENT_RGB,
    INK = UI_INK_RGB,
    LILAC = UI_ACCENT_RGB,
    AMBER = UI_WARN_RGB,
    RED = UI_DANGER_RGB,
#endif
};

lv_obj_t *screen_surface(lv_obj_t *parent, int width, int height, int radius, uint32_t color);
lv_obj_t *screen_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);
lv_obj_t *screen_button(lv_obj_t *parent, int width, int height, const char *text, lv_obj_t **label);
void screen_set_hidden(lv_obj_t *object, bool hidden);
void screen_mark_enabled(lv_obj_t *object, bool enabled);
