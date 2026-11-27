#include "screen_port.h"
#include "frame_rotate.h"

#include <assert.h>
#include <string.h>
#include "board.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

enum { FRAME_PIXELS = SCREEN_SIZE * SCREEN_SIZE, FRAME_BYTES = FRAME_PIXELS * sizeof(uint16_t),
       STATS_PERIOD_MS = 5000, TX_STACK_BYTES = 4096 };
static const char *TAG = "amoled_display";
/* LVGL draws the next logical frame while the worker transmits the previous
 * one. All three full frames live in PSRAM (~1.24 MiB); SPI owns its separate,
 * small internal DMA staging buffer. DIRECT retains static pixels; the panel
 * still receives a complete frame. No producer buffer is reused before ack. */
static EXT_RAM_BSS_ATTR _Alignas(64) uint16_t draw_a[FRAME_PIXELS];
static EXT_RAM_BSS_ATTR _Alignas(64) uint16_t draw_b[FRAME_PIXELS];
static EXT_RAM_BSS_ATTR _Alignas(64) uint16_t panel_frame[FRAME_PIXELS];

typedef struct {
    const uint16_t *pixels;
    lv_area_t changed;
    int64_t render_started, submitted;
} frame_job_t;
static QueueHandle_t frames;
static StaticQueue_t frame_queue;
static uint8_t frame_storage[sizeof(frame_job_t)];
static SemaphoreHandle_t flush_done;
static StaticSemaphore_t flush_semaphore;
static _Alignas(16) StackType_t tx_stack[TX_STACK_BYTES];
static StaticTask_t tx_tcb;
static esp_timer_handle_t ui_wake_timer;
static int64_t sample_started_us, render_started_us;
static lv_area_t changed = {SCREEN_SIZE, SCREEN_SIZE, -1, -1};

typedef struct {
    uint32_t frames, windows, errors;
    uint64_t prepare_us, rotate_us, tx_us, frame_us;
    uint32_t max_frame_us;
} display_stats_t;
static display_stats_t stats;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void render_started(lv_event_t *event)
{
    (void)event; render_started_us = esp_timer_get_time();
}

static void transmit_frame(const frame_job_t *job)
{
    const int64_t rotate_at = esp_timer_get_time();
    amoled_frame_rotate_region_rgb565(job->pixels, panel_frame, SCREEN_SIZE,
        job->changed.x1, job->changed.y1, lv_area_get_width(&job->changed), lv_area_get_height(&job->changed));
    const int64_t tx_at = esp_timer_get_time();
    /* The physical frame now owns all required pixels. Release the logical
     * buffer before SPI, allowing LVGL to synchronize/draw the next frame. The
     * single worker never changes panel_frame during the current transmission. */
    xSemaphoreGive(flush_done);
    const esp_err_t err = sugarat_display_write(0, 0, SCREEN_SIZE, SCREEN_SIZE, (const uint8_t *)panel_frame);
    const int64_t done_at = esp_timer_get_time();
    const uint32_t elapsed = (uint32_t)(done_at - job->render_started);
    portENTER_CRITICAL(&stats_lock);
    if (err == ESP_OK) {
        ++stats.frames;
        stats.windows += (SCREEN_SIZE + AMOLED_TRANSFER_ROWS - 1) / AMOLED_TRANSFER_ROWS;
        stats.prepare_us += job->submitted - job->render_started;
        stats.rotate_us += tx_at - rotate_at;
        stats.tx_us += done_at - tx_at;
        stats.frame_us += elapsed;
        if (elapsed > stats.max_frame_us) stats.max_frame_us = elapsed;
    } else ++stats.errors;
    portEXIT_CRITICAL(&stats_lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "Display flush: %s", esp_err_to_name(err));
}

static void display_tx(void *context)
{
    (void)context;
    frame_job_t job;
    while (true) if (xQueueReceive(frames, &job, portMAX_DELAY) == pdTRUE) transmit_frame(&job);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < SCREEN_SIZE && area->y2 < SCREEN_SIZE);
    if (changed.x1 > area->x1) changed.x1 = area->x1;
    if (changed.y1 > area->y1) changed.y1 = area->y1;
    if (changed.x2 < area->x2) changed.x2 = area->x2;
    if (changed.y2 < area->y2) changed.y2 = area->y2;
    if (!lv_display_flush_is_last(display)) {
        /* DIRECT may draw several invalid rectangles into the same complete
         * logical buffer. Submit once, after every rectangle is rendered. */
        lv_display_flush_ready(display);
        return;
    }
    const frame_job_t job = {.pixels = (const uint16_t *)pixels,
                             .changed = changed,
                             .render_started = render_started_us, .submitted = esp_timer_get_time()};
    const int queued = xQueueSend(frames, &job, portMAX_DELAY);
    assert(queued == pdTRUE);
    changed = (lv_area_t){SCREEN_SIZE, SCREEN_SIZE, -1, -1};
}

static void wait_flush(lv_display_t *display)
{
    (void)display;
    /* LVGL 9.2 clears its flushing flag after this callback returns. Blocking
     * here replaces LVGL's busy-spin and leaves CPU time for continuous audio. */
    xSemaphoreTake(flush_done, portMAX_DELAY);
}

static void log_stats(lv_timer_t *timer)
{
    (void)timer;
    const int64_t now = esp_timer_get_time(), elapsed = now - sample_started_us;
    if (elapsed <= 0) return;
    display_stats_t sample;
    portENTER_CRITICAL(&stats_lock);
    sample = stats; memset(&stats, 0, sizeof(stats));
    portEXIT_CRITICAL(&stats_lock);
    sample_started_us = now;
    const unsigned fps10 = sample.frames * 10000000ULL / elapsed;
    const uint32_t divisor = sample.frames ? sample.frames : 1;
    ESP_LOGI(TAG, "refresh: fps=%u.%u frames=%lu windows=%lu avg_prepare_us=%lu avg_rotate_us=%lu "
             "avg_tx_us=%lu avg_frame_us=%lu max_frame_us=%lu errors=%lu target_ms=%d mode=full_psram "
             "internal_free=%u psram_free=%u",
             fps10 / 10, fps10 % 10, (unsigned long)sample.frames, (unsigned long)sample.windows,
             (unsigned long)(sample.prepare_us / divisor), (unsigned long)(sample.rotate_us / divisor),
             (unsigned long)(sample.tx_us / divisor), (unsigned long)(sample.frame_us / divisor),
             (unsigned long)sample.max_frame_us, (unsigned long)sample.errors, AMOLED_FRAME_PERIOD_MS,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void wake_ui(void *context) { xTaskNotifyGive((TaskHandle_t)context); }

void screen_port_wait_until(int64_t deadline_us)
{
    (void)ulTaskNotifyTake(pdTRUE, 0);
    const int64_t remaining = deadline_us - esp_timer_get_time();
    if (remaining <= 0) return;
    const esp_err_t err = esp_timer_start_once(ui_wake_timer, (uint64_t)remaining);
    if (err != ESP_OK) { vTaskDelay(1); return; }
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100) + 1);
    (void)esp_timer_stop(ui_wake_timer);
}

static void read_touch(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    static lv_point_t last_point;
    bool pressed = false;
    int x, y;
    if (sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed) last_point = (lv_point_t){x, y};
    else pressed = false;
    data->point = last_point;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t screen_port_init(bool touch_ready)
{
    frames = xQueueCreateStatic(1, sizeof(frame_job_t), frame_storage, &frame_queue);
    flush_done = xSemaphoreCreateBinaryStatic(&flush_semaphore);
    if (!frames || !flush_done || !xTaskCreateStatic(display_tx, "amoled_tx", sizeof(tx_stack), NULL, 2, tx_stack, &tx_tcb))
        return ESP_ERR_NO_MEM;
    lv_init(); lv_tick_set_cb(tick_ms);
    lv_display_t *display = lv_display_create(SCREEN_SIZE, SCREEN_SIZE);
    if (!display) return ESP_ERR_NO_MEM;
    lv_display_set_theme(display, NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_a, draw_b, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, wait_flush);
    lv_display_add_event_cb(display, render_started, LV_EVENT_RENDER_START, NULL);
    lv_timer_set_period(lv_display_get_refr_timer(display), AMOLED_FRAME_PERIOD_MS);
    lv_timer_set_period(lv_anim_get_timer(), AMOLED_FRAME_PERIOD_MS);
    lv_timer_ready(lv_display_get_refr_timer(display));
    const esp_timer_create_args_t wake = {.callback = wake_ui, .arg = xTaskGetCurrentTaskHandle(),
        .dispatch_method = ESP_TIMER_TASK, .name = "amoled_ui_wake"};
    const esp_err_t err = esp_timer_create(&wake, &ui_wake_timer);
    if (err != ESP_OK) return err;
    sample_started_us = esp_timer_get_time();
    if (!lv_timer_create(log_stats, STATS_PERIOD_MS, NULL)) return ESP_ERR_NO_MEM;
    if (touch_ready) {
        lv_indev_t *input = lv_indev_create();
        if (!input) return ESP_ERR_NO_MEM;
        lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(input, read_touch);
        lv_timer_set_period(lv_indev_get_read_timer(input), AMOLED_TOUCH_PERIOD_MS);
    }
    ESP_LOGI(TAG, "PSRAM full-frame pipeline: logical=2x%dB rotated=%dB DMA=%dB frame_ms=%d",
             FRAME_BYTES, FRAME_BYTES, AMOLED_TRANSFER_BYTES, AMOLED_FRAME_PERIOD_MS);
    return ESP_OK;
}
