#pragma once
#include "lvgl.h"

/* Native AMOLED geometry. All scene coordinates and theme values live here. */
#define UI_SIZE                 466
#define UI_CENTER               (UI_SIZE / 2)
#define UI_BG                   lv_color_hex(0x000000)
#define UI_SURFACE              lv_color_hex(0x101919)
#define UI_SURFACE_PRESSED      lv_color_hex(0x203332)
#define UI_BORDER               lv_color_hex(0x344744)
#define UI_TEXT                 lv_color_hex(0xF0F4F3)
#define UI_TEXT_DIM             lv_color_hex(0x9BAAA7)
#define UI_ACCENT               lv_color_hex(0x55DDC3)
#define UI_RADIUS               16
#define UI_STROKE               2
#define UI_GAP                  8
#define UI_PAD                  12
#define UI_TOUCH_EXT            20
#define UI_PRESS_MS             120
#define UI_FONT_SMALL           (&lv_font_montserrat_14)
#define UI_FONT_BODY            (&lv_font_montserrat_18)
#define UI_FONT_TITLE           (&lv_font_montserrat_40)
#define UI_STATUS_Y             44
#define UI_HEADER_Y             0
#define UI_HEADER_H             104
#define UI_STATUS_X             143
#define UI_STATUS_W             180
#define UI_STATUS_H             24
#define UI_BATTERY_LABEL_X      289
#define UI_BRAND_Y              82
#define UI_BRAND_TRACKING       4
#define UI_ORBIT_X              129
#define UI_ORBIT_Y              110
#define UI_ORBIT_SIZE           208
#define UI_ORBIT_TITLE_Y        320
#define UI_ORBIT_HINT_Y         370
#define UI_HINT_W               340
#define UI_CODE_Y               194
#define UI_CODE_W               192
#define UI_HANDLE_W             32
#define UI_HANDLE_H             2
#define UI_HANDLE_Y             22
#define UI_ORBIT_BUTTON_Y       402
#define UI_BUTTON_W             176
#define UI_BUTTON_H             48
#define UI_BUTTON_ICON_X        30
#define UI_BUTTON_ICON_Y        12
#define UI_BUTTON_ICON_SIZE     24
#define UI_BUTTON_LABEL_X       62
#define UI_BUTTON_LABEL_Y       14
#define UI_RING_RADIUS          84
#define UI_RING_INNER           66
#define UI_MOTION_MS            1600
