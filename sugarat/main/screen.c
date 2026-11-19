#include "screen.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "servo_radio.h"

#define RGB565(r, g, b) (((r) & 0xf8) << 8 | ((g) & 0xfc) << 3 | (b) >> 3)

enum {
    CENTER = SCREEN_SIZE / 2,
    SPEED_Y = 165,
    ANGLE_Y = 226,
    MINUS_X = 78,
    PLUS_X = 282,
    STEP_RADIUS = 24,
    SWITCH_X = 83,
    SWITCH_Y = 263,
    SWITCH_W = 194,
    SWITCH_H = 52,
    INK = RGB565(11, 20, 29),
    PANEL = RGB565(23, 37, 49),
    BORDER = RGB565(46, 64, 79),
    WHITE = RGB565(236, 244, 247),
    MUTED = RGB565(150, 172, 189),
    ACCENT = RGB565(109, 233, 186),
    AMBER = RGB565(255, 192, 108),
};

static uint16_t *pixels;

static const char glyphs[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.+- ";
static const uint8_t font[][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}, {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12}, {17,18,20,24,20,18,17},
    {16,16,16,16,16,16,31}, {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13},
    {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,27,17},
    {17,17,10,4,10,17,17}, {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {0,0,0,0,0,0,4}, {0,4,4,31,4,4,0}, {0,0,0,31,0,0,0}, {0},
};

static void rect(int x, int y, int w, int h, uint16_t color)
{
    const uint16_t wire_color = __builtin_bswap16(color);
    for (int row = y; row < y + h; ++row) {
        uint16_t *line = &pixels[row * SCREEN_SIZE + x];
        for (int col = 0; col < w; ++col) {
            line[col] = wire_color;
        }
    }
}

static void rounded(int x, int y, int w, int h, int radius, uint16_t color)
{
    int inset = radius;
    for (int row = 0; row < radius; ++row) {
        const int dy = radius - row - 1;
        while (inset > 0 && (radius - inset + 1) * (radius - inset + 1) + dy * dy <= radius * radius) {
            --inset;
        }
        rect(x + inset, y + row, w - 2 * inset, 1, color);
        rect(x + inset, y + h - row - 1, w - 2 * inset, 1, color);
    }
    rect(x, y + radius, w, h - radius * 2, color);
}

static void label(const char *text, int center, int y, int scale, uint16_t color)
{
    int x = center - ((int)strlen(text) * 6 - 1) * scale / 2;
    for (const char *c = text; *c; ++c, x += 6 * scale) {
        const char *index = strchr(glyphs, *c);
        if (!index) {
            continue;
        }
        const uint8_t *bitmap = font[index - glyphs];
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((bitmap[row] & (16 >> col)) != 0) {
                    rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

esp_err_t screen_init(void)
{
    pixels = heap_caps_malloc(SCREEN_SIZE * SCREEN_SIZE * sizeof(*pixels), MALLOC_CAP_SPIRAM);
    return pixels ? ESP_OK : ESP_ERR_NO_MEM;
}

static void adjustment(int y, const char *name, const char *value, bool can_decrease, bool can_increase)
{
    const int diameter = STEP_RADIUS * 2;
    label(name, CENTER, y - 24, 2, MUTED);
    label(value, CENTER, y - 4, 3, WHITE);
    rounded(MINUS_X - STEP_RADIUS, y - STEP_RADIUS, diameter, diameter,
            STEP_RADIUS, can_decrease ? BORDER : PANEL);
    rounded(PLUS_X - STEP_RADIUS, y - STEP_RADIUS, diameter, diameter,
            STEP_RADIUS, can_increase ? BORDER : PANEL);
    label("-", MINUS_X, y - 14, 4, can_decrease ? ACCENT : MUTED);
    label("+", PLUS_X, y - 14, 4, can_increase ? ACCENT : MUTED);
}

esp_err_t screen_draw(screen_state_t state)
{
    const bool active = state.online && state.touch_ready && !state.pending;
    rect(0, 0, SCREEN_SIZE, SCREEN_SIZE, INK);
    label("SERVO", CENTER, 35, 4, WHITE);

    const char *link = !state.touch_ready ? "TOUCH ERROR" : state.online ? "CONNECTED" : "WAITING FOR C3";
    rounded(82, 69, 196, 27, 13, PANEL);
    label(link, CENTER, 75, 2, state.online && state.touch_ready ? ACCENT : AMBER);

    rounded(79, 101, 202, 35, 16, PANEL);
    const char *position = !state.online ? "POSITION -" : state.position ? "POSITION B" : "POSITION A";
    if (state.online && state.moving) {
        position = state.position ? "MOVING TO B" : "MOVING TO A";
    }
    label(position, CENTER, 111, 2, WHITE);

    const unsigned centiseconds = (1000u + state.speed - 1) / state.speed;
    char seconds[16];
    snprintf(seconds, sizeof(seconds), "%u.%02uS", centiseconds / 100, centiseconds % 100);
    adjustment(SPEED_Y, "SPEED AT 90", seconds,
               state.speed > SERVO_SPEED_MIN && active, state.speed < SERVO_SPEED_MAX && active);

    char angle[12];
    char angle_label[16];
    snprintf(angle, sizeof(angle), "%u DEG", (unsigned)state.angle_deg);
    snprintf(angle_label, sizeof(angle_label), "ANGLE MAX %u", (unsigned)state.angle_limit_deg);
    adjustment(ANGLE_Y, angle_label, angle,
               state.angle_deg > 0 && active, state.angle_deg < state.angle_limit_deg && active);

    rounded(SWITCH_X, SWITCH_Y, SWITCH_W, SWITCH_H, 24, active ? ACCENT : PANEL);
    label(state.pending ? "SENDING" : state.position ? "GO TO A" : "GO TO B", CENTER, SWITCH_Y + 16,
          3, active ? INK : MUTED);
    return sugarat_display_present(pixels);
}

static bool hit_step(int x, int y, int center_x, int center_y)
{
    const int dx = x - center_x;
    const int dy = y - center_y;
    return dx * dx + dy * dy <= STEP_RADIUS * STEP_RADIUS;
}

screen_action_t screen_hit(int x, int y)
{
    if (hit_step(x, y, MINUS_X, SPEED_Y)) {
        return SCREEN_SLOWER;
    }
    if (hit_step(x, y, PLUS_X, SPEED_Y)) {
        return SCREEN_FASTER;
    }
    if (hit_step(x, y, MINUS_X, ANGLE_Y)) {
        return SCREEN_ANGLE_DOWN;
    }
    if (hit_step(x, y, PLUS_X, ANGLE_Y)) {
        return SCREEN_ANGLE_UP;
    }
    if (x >= SWITCH_X && x < SWITCH_X + SWITCH_W && y >= SWITCH_Y && y < SWITCH_Y + SWITCH_H) {
        return SCREEN_SWITCH;
    }
    return SCREEN_NONE;
}
