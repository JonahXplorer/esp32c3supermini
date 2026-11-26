#include "screen_port.h"

#include <string.h>
#include "board.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

enum { DRAW_BUFFER_ROWS = 12, PANEL_STRIPE_ROWS = 12 };
static const char *TAG = "amoled_display";
static EXT_RAM_BSS_ATTR LV_ATTRIBUTE_MEM_ALIGN uint16_t draw_buffer[SCREEN_SIZE * DRAW_BUFFER_ROWS];
// Retain boundary pixels when rounding partial updates to CO5300's 2x2 windows.
static EXT_RAM_BSS_ATTR uint16_t panel_pixels[AMOLED_PANEL_SIZE * AMOLED_PANEL_SIZE];
static EXT_RAM_BSS_ATTR uint16_t stripe[AMOLED_PANEL_SIZE * PANEL_STRIPE_ROWS];

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int physical_edge(int logical)
{
    return (logical * AMOLED_PANEL_SIZE + SCREEN_SIZE - 1) / SCREEN_SIZE;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const int width = lv_area_get_width(area), height = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(pixels, width * height);
    const uint16_t *source = (const uint16_t *)pixels;
    const int left = physical_edge(area->x1), top = physical_edge(area->y1);
    const int right = physical_edge(area->x2 + 1), bottom = physical_edge(area->y2 + 1);
    for (int y = top; y < bottom; ++y) {
        const int sy = y * SCREEN_SIZE / AMOLED_PANEL_SIZE - area->y1;
        for (int x = left; x < right; ++x) {
            const int sx = x * SCREEN_SIZE / AMOLED_PANEL_SIZE - area->x1;
            // Clockwise rotation in native panel space: (x,y) -> (465-y,x).
            panel_pixels[x * AMOLED_PANEL_SIZE + AMOLED_PANEL_SIZE - 1 - y] = source[sy * width + sx];
        }
    }
    const int x1 = (AMOLED_PANEL_SIZE - bottom) & ~1;
    const int x2 = (AMOLED_PANEL_SIZE - top + 1) & ~1;
    const int y1 = left & ~1, y2 = (right + 1) & ~1, columns = x2 - x1;
    for (int y = y1; y < y2; y += PANEL_STRIPE_ROWS) {
        const int rows = y2 - y < PANEL_STRIPE_ROWS ? y2 - y : PANEL_STRIPE_ROWS;
        for (int row = 0; row < rows; ++row) {
            memcpy(stripe + row * columns, panel_pixels + (y + row) * AMOLED_PANEL_SIZE + x1,
                   columns * sizeof(*stripe));
        }
        const esp_err_t err = sugarat_display_write(x1, y, columns, rows, (const uint8_t *)stripe);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Display flush: %s", esp_err_to_name(err));
            break;
        }
    }
    lv_display_flush_ready(display);
}

static void read_touch(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    static lv_point_t last_point;
    bool pressed = false;
    int x, y;
    // Release on a failed read so a transient I2C error cannot hold a slider down.
    if (sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed) last_point = (lv_point_t){x, y};
    else pressed = false;
    data->point = last_point;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t screen_port_init(bool touch_ready)
{
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = lv_display_create(SCREEN_SIZE, SCREEN_SIZE);
    if (!display) return ESP_ERR_NO_MEM;
    lv_display_set_theme(display, NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_timer_ready(lv_display_get_refr_timer(display));
    if (touch_ready) {
        lv_indev_t *input = lv_indev_create();
        if (!input) return ESP_ERR_NO_MEM;
        lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(input, read_touch);
    }
    return ESP_OK;
}
