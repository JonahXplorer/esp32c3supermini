/*
 * CST9217 protocol adapted from Waveshare's bundled SensorLib by lewis he.
 * Copyright (c) 2024 lewis he
 * SPDX-License-Identifier: MIT
 * See NOTICE.touch and LICENSE.SensorLib for the pinned reference and license.
 */
#include "board.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    TOUCH_ADDRESS = 0x5A, TOUCH_RESET = GPIO_NUM_40, TOUCH_INTERRUPT = GPIO_NUM_11,
    TOUCH_POINTS = 2, TOUCH_REPORT_BYTES = TOUCH_POINTS * 5 + 5,
    TOUCH_TIMEOUT_MS = 40, ERROR_LOG_US = 2000000,
};
static const char *TAG = "amoled_touch";
static i2c_master_dev_handle_t touch;
static DRAM_ATTR atomic_bool touch_pending;
static bool touch_pressed;
static int touch_x, touch_y;
static int64_t last_error_us;
static unsigned suppressed_errors;

static void IRAM_ATTR touch_interrupt(void *context)
{
    (void)context;
    atomic_store_explicit(&touch_pending, true, memory_order_relaxed);
}

static esp_err_t read_register(uint16_t address, uint8_t *data, size_t bytes)
{
    const uint8_t command[] = {address >> 8, address & 0xff};
    // SensorLib writeThenRead: two-byte register address, then the whole report.
    return i2c_master_transmit_receive(touch, command, sizeof(command), data, bytes, TOUCH_TIMEOUT_MS);
}

static uint32_t little_endian_word(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static esp_err_t read_attributes(void)
{
    // Match TouchDrvCST92xx::getAttribute(): reset, wait 30ms, D1 01, wait 10ms.
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RESET, 0), TAG, "Assert reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RESET, 1), TAG, "Release reset");
    vTaskDelay(pdMS_TO_TICKS(30));
    const uint8_t command[] = {0xD1, 0x01};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(touch, command, sizeof(command), TOUCH_TIMEOUT_MS),
                        TAG, "Enter command mode");
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t data[8];
    ESP_RETURN_ON_ERROR(read_register(0xD1FC, data, 4), TAG, "Read checkcode");
    const uint32_t checkcode = little_endian_word(data);
    ESP_RETURN_ON_FALSE((checkcode & 0xffff0000) == 0xcaca0000,
                        ESP_ERR_INVALID_RESPONSE, TAG, "Invalid checkcode: %08lx", (unsigned long)checkcode);
    ESP_RETURN_ON_ERROR(read_register(0xD1F8, data, 4), TAG, "Read resolution");
    const unsigned width = data[0] | (data[1] << 8), height = data[2] | (data[3] << 8);
    ESP_RETURN_ON_FALSE(width == AMOLED_PANEL_SIZE && height == AMOLED_PANEL_SIZE,
                        ESP_ERR_NOT_SUPPORTED, TAG, "Unexpected touch resolution %ux%u", width, height);
    ESP_RETURN_ON_ERROR(read_register(0xD204, data, 4), TAG, "Read chip type");
    const unsigned chip = data[2] | (data[3] << 8), project = data[0] | (data[1] << 8);
    ESP_RETURN_ON_FALSE(chip == 0x9217 || chip == 0x9220,
                        ESP_ERR_NOT_SUPPORTED, TAG, "Unexpected touch chip %04x", chip);
    ESP_RETURN_ON_ERROR(read_register(0xD208, data, 8), TAG, "Read firmware information");
    const uint32_t firmware = little_endian_word(data), checksum = little_endian_word(data + 4);
    ESP_RETURN_ON_FALSE(firmware != 0xa5a5a5a5, ESP_ERR_INVALID_STATE, TAG, "Touch firmware missing");
    ESP_LOGI(TAG, "CST%04X %ux%u project=%04X firmware=%08lx checksum=%08lx; IRQ + read ACK",
             chip, width, height, project, (unsigned long)firmware, (unsigned long)checksum);
    return ESP_OK;
}

esp_err_t sugarat_touch_init(void)
{
    if (touch) return ESP_OK;
    ESP_RETURN_ON_ERROR(amoled_board_init(), TAG, "Shared I2C bus");
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TOUCH_ADDRESS, .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(amoled_i2c_bus(), &device, &touch), TAG, "Touch I2C device");
    esp_err_t ret;
    const gpio_config_t reset = {.pin_bit_mask = 1ULL << TOUCH_RESET, .mode = GPIO_MODE_OUTPUT};
    const gpio_config_t interrupt = {
        .pin_bit_mask = 1ULL << TOUCH_INTERRUPT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&reset), fail, TAG, "Reset pin");
    ESP_GOTO_ON_ERROR(gpio_config(&interrupt), fail, TAG, "Interrupt pin");
    ESP_GOTO_ON_ERROR(read_attributes(), fail, TAG, "Touch initialization");
    ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret == ESP_ERR_INVALID_STATE) ret = ESP_OK; // Reuse an already installed GPIO service.
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "GPIO interrupt service");
    atomic_store(&touch_pending, false);
    touch_pressed = false;
    ESP_GOTO_ON_ERROR(gpio_isr_handler_add(TOUCH_INTERRUPT, touch_interrupt, NULL), fail, TAG, "Touch interrupt");
    return ESP_OK;
fail:
    (void)i2c_master_bus_rm_device(touch);
    touch = NULL;
    return ret;
}

static esp_err_t report_error(const char *stage, esp_err_t err)
{
    const int64_t now = esp_timer_get_time();
    if (!last_error_us || now - last_error_us >= ERROR_LOG_US) {
        ESP_LOGW(TAG, "%s: %s (suppressed=%u)", stage, esp_err_to_name(err), suppressed_errors);
        last_error_us = now;
        suppressed_errors = 0;
    } else {
        ++suppressed_errors;
    }
    return err;
}

esp_err_t sugarat_touch_read(bool *pressed, int *x, int *y)
{
    if (!touch || !pressed || !x || !y) return ESP_ERR_INVALID_ARG;
    *pressed = touch_pressed;
    *x = touch_x;
    *y = touch_y;
    // Match the example's takeTouchInterrupt(); the ISR only records an event.
    if (!atomic_exchange_explicit(&touch_pending, false, memory_order_relaxed)) return ESP_OK;
    uint8_t report[TOUCH_REPORT_BYTES];
    touch_pressed = *pressed = false;
    esp_err_t err = read_register(0xD000, report, sizeof(report));
    if (err != ESP_OK) return report_error("Read report", err);
    // SensorLib acknowledges every successful read, even an empty/invalid report.
    const uint8_t acknowledge[] = {0xD0, 0x00, 0xAB};
    err = i2c_master_transmit(touch, acknowledge, sizeof(acknowledge), TOUCH_TIMEOUT_MS);
    if (err != ESP_OK) return report_error("Acknowledge report", err);
    if (report[6] != 0xAB) return ESP_OK;
    const unsigned points = report[5] & 0x7f;
    if (!points || points > TOUCH_POINTS) return ESP_OK;
    // The pointer UI uses the first slot from the reference's two-point report.
    const unsigned id = report[0] >> 4, status = report[0] & 0x0f;
    if (status != 0x06 || id >= TOUCH_POINTS) return ESP_OK;
    const unsigned raw_x = (report[1] << 4) | (report[3] >> 4);
    const unsigned raw_y = (report[2] << 4) | (report[3] & 0x0f);
    if (raw_x >= AMOLED_PANEL_SIZE || raw_y >= AMOLED_PANEL_SIZE) {
        return report_error("Touch coordinates", ESP_ERR_INVALID_RESPONSE);
    }
    // Native orientation mirrors both axes. Invert the display's clockwise 90deg
    // rotation BEFORE scaling, so partial-update and touch rounding stay aligned.
    touch_x = *x = (AMOLED_PANEL_SIZE - 1 - raw_y) * SCREEN_SIZE / AMOLED_PANEL_SIZE;
    touch_y = *y = raw_x * SCREEN_SIZE / AMOLED_PANEL_SIZE;
    touch_pressed = *pressed = true;
    return ESP_OK;
}
