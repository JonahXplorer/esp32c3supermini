// Host regression tests compile the production touch and display code directly.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "idf_stub.h"
#define TAG touch_tag
#include "../components/sugarat_board/touch.c"
#undef TAG
#include "../main/screen_port.c"

static void (*irq_handler)(void *);
static unsigned attribute_reads, report_reads, acknowledgements, command_writes, flushes;
static unsigned delays[4], delay_count;
static bool fail_read, fail_ack, irq_during_read;
static uint8_t packet[15];
static uint16_t displayed[466 * 466], logical[360 * 360], pixels[360 * 360];

void vTaskDelay(unsigned ms) { assert(delay_count < 4); delays[delay_count++] = ms; }
int64_t esp_timer_get_time(void) { return 10000000; }
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
    assert(!((x | y | width | height) & 1) && height <= 12);
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
        const int sx = py * 360 / 466, sy = (465 - px) * 360 / 466;
        const uint16_t value = logical[sy * 360 + sx];
        assert(displayed[py * 466 + px] == (uint16_t)((value >> 8) | (value << 8)));
    }
}
static void repaint(lv_area_t area, unsigned salt)
{
    size_t count = 0;
    for (int y = area.y1; y <= area.y2; ++y) for (int x = area.x1; x <= area.x2; ++x) {
        const uint16_t value = (uint16_t)(x * 251 + y * 17 + salt);
        pixels[count++] = logical[y * 360 + x] = value;
    }
    flush(NULL, &area, (uint8_t *)pixels);
    check_image();
}
static void rotation_and_touch(void)
{
    repaint((lv_area_t){0, 0, 359, 359}, 0);
    repaint((lv_area_t){1, 3, 7, 8}, 1000);
    repaint((lv_area_t){359, 0, 359, 359}, 2000);
    repaint((lv_area_t){0, 355, 359, 359}, 3000);
    assert(flushes == 4);
    // Raw touch is mirrored by the board mounting. Verify it selects exactly the
    // logical pixel shown at that physical location after clockwise rotation.
    for (unsigned raw_y = 0; raw_y < 466; ++raw_y) for (unsigned raw_x = 0; raw_x < 466; ++raw_x) {
        make_packet(raw_x, raw_y); irq_handler(NULL);
        bool pressed; int x, y;
        assert(sugarat_touch_read(&pressed, &x, &y) == ESP_OK && pressed);
        const unsigned px = 465 - raw_x, py = 465 - raw_y;
        const uint16_t value = logical[y * 360 + x];
        assert(displayed[py * 466 + px] == (uint16_t)((value >> 8) | (value << 8)));
    }
    puts("PASS: clockwise full/partial redraws, aligned windows and 217156 touch/display positions");
}
int main(void)
{
    touch_protocol();
    rotation_and_touch();
    return 0;
}
