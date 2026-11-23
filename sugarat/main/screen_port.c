#include "screen_port.h"

#include "board.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

enum { DRAW_BUFFER_ROWS = 12 };

static const char *TAG = "sugarat_display";
// The LCD adapter copies pixels into its own DMA buffer before queuing the transaction.
static EXT_RAM_BSS_ATTR LV_ATTRIBUTE_MEM_ALIGN uint16_t draw_buffer[SCREEN_SIZE * DRAW_BUFFER_ROWS];

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void align_window(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    // An even pixel count keeps each RGB565 row word aligned for the LCD SPI bus.
    area->x1 &= ~1;
    area->x2 |= 1;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const int width = lv_area_get_width(area);
    const int height = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(pixels, width * height);
    const esp_err_t err = sugarat_display_write(area->x1, area->y1, width, height, pixels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display flush failed: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(display);
}

static void read_touch(lv_indev_t *input, lv_indev_data_t *data)
{
    static lv_point_t last_point;
    static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;
    bool pressed;
    int x, y;
    if (sugarat_touch_read(&pressed, &x, &y) == ESP_OK) {
        last_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (pressed) {
            last_point = (lv_point_t){.x = x, .y = y};
        }
    }
    data->point = last_point;
    data->state = last_state;
}

esp_err_t screen_port_init(bool touch_ready)
{
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = lv_display_create(SCREEN_SIZE, SCREEN_SIZE);
    if (!display) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_theme(display, NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(display, align_window, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_set_flush_cb(display, flush);
    lv_timer_ready(lv_display_get_refr_timer(display));

    if (touch_ready) {
        lv_indev_t *input = lv_indev_create();
        if (!input) {
            return ESP_ERR_NO_MEM;
        }
        lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(input, read_touch);
    }
    return ESP_OK;
}
