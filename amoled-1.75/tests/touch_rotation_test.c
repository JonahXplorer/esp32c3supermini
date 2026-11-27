// Host regression tests compile the production touch and display code directly.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "idf_stub.h"
#define TAG touch_tag
#include "../components/sugarat_board/touch.c"
#undef TAG
#include "../main/screen_port.c"
#include "../main/frame_rotate.c"

static void (*irq_handler)(void *);
static unsigned attribute_reads, report_reads, acknowledgements, command_writes, flushes;
static unsigned delays[4], delay_count;
static bool fail_read, fail_ack, irq_during_read;
static uint8_t packet[15];
static uint16_t displayed[466 * 466], logical[466 * 466], pixels[466 * 466];
static unsigned windows, timer_starts, timer_stops;
static int64_t clock_us = 10000000;
static esp_timer_create_args_t wake_args;
static bool notified;
static frame_job_t queued_frame;
static bool frame_queued, frame_done;
bool stub_flush_last = true;
static bool reuse_input_during_tx;

TaskHandle_t xTaskCreateStatic(void (*entry)(void *), const char *name, unsigned stack,
                              void *arg, unsigned priority, StackType_t *buffer, StaticTask_t *control)
{ assert(entry == display_tx && priority == 2); return (void *)6; }
QueueHandle_t xQueueCreateStatic(unsigned length, unsigned item, uint8_t *storage, StaticQueue_t *control)
{ assert(length == 1 && item == sizeof(frame_job_t)); return (void *)9; }
int xQueueSend(QueueHandle_t q, const void *item, unsigned wait)
{ assert(q == frames && !frame_queued); queued_frame = *(const frame_job_t *)item; frame_queued = true; return pdTRUE; }
int xQueueReceive(QueueHandle_t q, void *item, unsigned wait) { return 0; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *control) { return (void *)10; }
int xSemaphoreTake(SemaphoreHandle_t sem, unsigned wait)
{ assert(sem == flush_done && frame_done); frame_done = false; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t sem)
{ assert(sem == flush_done && !frame_done); frame_done = true; return pdTRUE; }
size_t heap_caps_get_free_size(uint32_t caps) { return 100000; }

void vTaskDelay(unsigned ms) { assert(delay_count < 4); delays[delay_count++] = ms; }
int64_t esp_timer_get_time(void) { return clock_us; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)7; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task == (void *)7); notified = true; }
uint32_t ulTaskNotifyTake(int clear, unsigned wait)
{
    assert(clear == pdTRUE);
    if (wait) assert(notified);
    const bool result = notified; notified = false; return result;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{ wake_args = *args; *timer = (void *)8; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t us)
{
    assert(timer == (void *)8); ++timer_starts; clock_us += us;
    wake_args.callback(wake_args.arg); return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{ assert(timer == (void *)8); ++timer_stops; return ESP_OK; }
const char *esp_err_to_name(esp_err_t err) { return "mock failure"; }
esp_err_t amoled_board_init(void) { return ESP_OK; }
i2c_master_bus_handle_t amoled_i2c_bus(void) { return (void *)1; }
esp_err_t gpio_config(const gpio_config_t *cfg)
{
    if (cfg->mode == GPIO_MODE_INPUT) {
        assert(cfg->pin_bit_mask == (1ULL << 11));
        assert(cfg->pull_up_en == GPIO_PULLUP_ENABLE && cfg->intr_type == GPIO_INTR_NEGEDGE);
    } else assert(cfg->pin_bit_mask == (1ULL << 40));
    return ESP_OK;
}
esp_err_t gpio_set_level(int pin, int value) { assert(pin == 40); return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { assert(flags == ESP_INTR_FLAG_IRAM); return ESP_OK; }
esp_err_t gpio_isr_handler_add(int pin, void (*handler)(void *), void *context)
{ assert(pin == 11); irq_handler = handler; return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *dev)
{ assert(bus && cfg->device_address == 0x5a && cfg->scl_speed_hz == 400000); *dev = (void *)1; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *cmd, size_t cmd_bytes,
                                      uint8_t *data, size_t bytes, int timeout)
{
    assert(dev && cmd_bytes == 2 && timeout == 40);
    const unsigned reg = (cmd[0] << 8) | cmd[1];
    memset(data, 0, bytes);
    if (reg == 0xd000) {
        ++report_reads; assert(bytes == sizeof(packet));
        if (fail_read) return ESP_FAIL;
        memcpy(data, packet, bytes);
        if (irq_during_read) { irq_during_read = false; irq_handler(NULL); }
    } else {
        static const unsigned sequence[] = {0xd1fc, 0xd1f8, 0xd204, 0xd208};
        assert(attribute_reads < 4 && sequence[attribute_reads++] == reg);
        if (reg == 0xd1fc) { assert(bytes == 4); data[2] = data[3] = 0xca; }
        if (reg == 0xd1f8) { data[0] = data[2] = 0xd2; data[1] = data[3] = 1; }
        if (reg == 0xd204) { data[0] = 0x34; data[1] = 0x57; data[2] = 0x17; data[3] = 0x92; }
        if (reg == 0xd208) { assert(bytes == 8); data[0] = 1; }
    }
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t bytes, int timeout)
{
    assert(dev && timeout == 40);
    if (bytes == 2) { assert(data[0] == 0xd1 && data[1] == 1); ++command_writes; }
    else {
        static const uint8_t expected[] = {0xd0, 0x00, 0xab};
        assert(bytes == 3 && memcmp(data, expected, 3) == 0);
        ++acknowledgements;
        if (fail_ack) return ESP_FAIL;
    }
    return ESP_OK;
}
void lv_draw_sw_rgb565_swap(void *buffer, unsigned count)
{
    uint16_t *data = buffer;
    for (unsigned i = 0; i < count; ++i) data[i] = (data[i] >> 8) | (data[i] << 8);
}
void lv_display_flush_ready(lv_display_t *display) { ++flushes; }
esp_err_t sugarat_display_write(int x, int y, int width, int height, const uint8_t *buffer)
{
    assert(x >= 0 && y >= 0 && x + width <= 466 && y + height <= 466);
    assert(!((x | y | width | height) & 1));
    assert(width == 466 && height == 466);
    assert(frame_done); // Logical input is released after rotation, before SPI.
    if (reuse_input_during_tx) memset(pixels, 0x5a, sizeof(pixels));
    windows += (height + AMOLED_TRANSFER_ROWS - 1) / AMOLED_TRANSFER_ROWS;
    for (int row = 0; row < height; ++row)
        memcpy(displayed + (y + row) * 466 + x, buffer + row * width * 2, width * 2);
    return ESP_OK;
}

static void make_packet(unsigned x, unsigned y)
{
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x06; packet[1] = x >> 4; packet[2] = y >> 4;
    packet[3] = ((x & 15) << 4) | (y & 15); packet[5] = 1; packet[6] = 0xab;
}
static void touch_protocol(void)
{
    assert(sugarat_touch_init() == ESP_OK && irq_handler);
    assert(attribute_reads == 4 && command_writes == 1);
    assert(delay_count == 3 && delays[0] == 10 && delays[1] == 30 && delays[2] == 10);
    bool pressed; int x, y;
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && !pressed && report_reads == 0);
    make_packet(80, 120); irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed);
    assert(report_reads == 1 && acknowledgements == 1);
    // No fresh event must neither read I2C nor release a held drag.
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed && report_reads == 1);
    packet[6] = 0; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && !pressed && acknowledgements == 2);
    make_packet(1, 1); packet[5] = 0; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && !pressed);
    packet[5] = 3; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && !pressed);
    make_packet(1, 1); packet[0] = 0x07; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && !pressed);
    packet[0] = 0x26; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && !pressed);
    make_packet(466, 20); irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_ERR_INVALID_RESPONSE && !pressed);
    make_packet(15, 30); packet[5] = 2; packet[0] = 0x16; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed);
    unsigned acks = acknowledgements;
    fail_read = true; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_FAIL && !pressed && acknowledgements == acks);
    fail_read = false; fail_ack = true; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_FAIL && !pressed);
    fail_ack = false; irq_during_read = true; irq_handler(NULL);
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed);
    unsigned reads = report_reads;
    assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed && report_reads == reads + 1);
    puts("PASS: reference initialization, IRQ-only reads, 15-byte packets, read ACK, release and error recovery");
}
static void check_image(void)
{
    // Independent inverse-rotation oracle for every native physical pixel.
    for (int py = 0; py < 466; ++py) for (int px = 0; px < 466; ++px) {
        const int sx = py, sy = 465 - px;
        const uint16_t value = logical[sy * 466 + sx];
        assert(displayed[py * 466 + px] == (uint16_t)((value >> 8) | (value << 8)));
    }
}
static void repaint(unsigned salt)
{
    size_t count = 0;
    for (int y = 0; y < 466; ++y) for (int x = 0; x < 466; ++x) {
        const uint16_t value = (uint16_t)(x * 251 + y * 17 + salt);
        pixels[count++] = logical[y * 466 + x] = value;
    }
    const lv_area_t full = {0, 0, 465, 465};
    flush(NULL, &full, (uint8_t *)pixels);
    assert(frame_queued && !frame_done && queued_frame.pixels == pixels);
    // The next LVGL frame can be drawn before the current transfer completes.
    memset(draw_b, 0xa5, sizeof(draw_b));
    frame_job_t job = queued_frame; frame_queued = false;
    transmit_frame(&job);
    assert(frame_done);
    wait_flush(NULL);
    assert(!frame_done);
    for (size_t i = 0; i < count; ++i) assert(pixels[i] == logical[i]);
    ++flushes;
    check_image();
}
static void rotation_and_touch(void)
{
    repaint(0);
    repaint(1000);
    repaint(2000);
    repaint(3000);
    assert(flushes == 4);
    // Raw touch is mirrored by the board mounting. Verify it selects exactly the
    // logical pixel shown at that physical location after clockwise rotation.
    for (unsigned raw_y = 0; raw_y < 466; ++raw_y) for (unsigned raw_x = 0; raw_x < 466; ++raw_x) {
        make_packet(raw_x, raw_y); irq_handler(NULL);
        bool pressed; int x, y;
        assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed);
        const unsigned px = 465 - raw_x, py = 465 - raw_y;
        const uint16_t value = logical[y * 466 + x];
        assert(displayed[py * 466 + px] == (uint16_t)((value >> 8) | (value << 8)));
    }
    puts("PASS: tiled clockwise full-frame rotation and 217156 touch/display positions");
}
static void retained_frame(void)
{
    const lv_area_t areas[] = {{129, 110, 337, 318}, {464, 464, 465, 465}};
    for (unsigned a = 0; a < 2; ++a)
        for (int y = areas[a].y1; y <= areas[a].y2; ++y)
            for (int x = areas[a].x1; x <= areas[a].x2; ++x)
                pixels[y * 466 + x] = logical[y * 466 + x] ^= 0x71c3;
    stub_flush_last = false;
    flush(NULL, &areas[0], (uint8_t *)pixels);
    assert(!frame_queued && !frame_done);
    stub_flush_last = true;
    flush(NULL, &areas[1], (uint8_t *)pixels);
    assert(frame_queued && !frame_done && queued_frame.changed.x1 == 129 && queued_frame.changed.x2 == 465);
    frame_job_t job = queued_frame; frame_queued = false;
    transmit_frame(&job); wait_flush(NULL);
    check_image();

    const lv_area_t corner = {465, 465, 465, 465};
    pixels[466 * 466 - 1] = logical[466 * 466 - 1] ^= 0x1234;
    flush(NULL, &corner, (uint8_t *)pixels);
    job = queued_frame; frame_queued = false;
    reuse_input_during_tx = true;
    transmit_frame(&job); wait_flush(NULL);
    reuse_input_during_tx = false;
    assert(pixels[0] == 0x5a5a);
    check_image();
    puts("PASS: DIRECT rectangle batching, retained edge pixels, full transfers and logical-buffer reuse during SPI");
}
int main(void)
{
    touch_protocol();
    assert(screen_port_init(false) == ESP_OK);
    rotation_and_touch();
    assert(windows == 4 * 15);
    retained_frame();
    assert(windows == 6 * 15);
    const int64_t start = clock_us;
    screen_port_wait_until(start + 3300);
    assert(clock_us == start + 3300 && timer_starts == 1 && timer_stops == 1);
    screen_port_wait_until(clock_us - 1);
    assert(timer_starts == 1);
    notified = true;
    screen_port_wait_until(clock_us + 700);
    assert(!notified && timer_starts == 2 && timer_stops == 2);
    puts("PASS: full-frame rotation preserves logical buffers, next-frame overlap and completion ownership; 15 windows/frame; sub-tick UI wake");
    return 0;
}
