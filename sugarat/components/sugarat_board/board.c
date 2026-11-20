#include "board.h"

#include "co5300_qspi_io.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    BATTERY_LATCH = GPIO_NUM_7,
    AMPLIFIER_ENABLE = GPIO_NUM_3,
    LCD_POWER = GPIO_NUM_12,
    LCD_RESET = GPIO_NUM_9,
    LCD_BACKLIGHT = GPIO_NUM_13,
    TOUCH_RESET = GPIO_NUM_5,
    LCD_STRIPE_ROWS = 12,
    LCD_STRIPE_BYTES = SCREEN_SIZE * LCD_STRIPE_ROWS * sizeof(uint16_t),
};

static const char *TAG = "sugarat_board";
static esp_lcd_panel_io_handle_t lcd;
static i2c_master_dev_handle_t touch;

esp_err_t sdgoods_panel_initialize(esp_lcd_panel_io_handle_t io);

static esp_err_t output(gpio_num_t pin, int level)
{
    ESP_RETURN_ON_ERROR(gpio_reset_pin(pin), TAG, "Configure output pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), TAG, "Set output level");
    return gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

esp_err_t sugarat_display_init(void)
{
    ESP_RETURN_ON_ERROR(output(BATTERY_LATCH, 1), TAG, "Battery latch");
    ESP_RETURN_ON_ERROR(output(AMPLIFIER_ENABLE, 0), TAG, "Turn off amplifier");
    ESP_RETURN_ON_ERROR(output(LCD_BACKLIGHT, 0), TAG, "Turn off backlight");
    ESP_RETURN_ON_ERROR(output(LCD_POWER, 0), TAG, "LCD power");
    vTaskDelay(pdMS_TO_TICKS(50));

    const spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_40,
        .data0_io_num = GPIO_NUM_46, .data1_io_num = GPIO_NUM_45,
        .data2_io_num = GPIO_NUM_42, .data3_io_num = GPIO_NUM_41,
        .data4_io_num = -1, .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = LCD_STRIPE_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    ESP_RETURN_ON_ERROR(co5300_new_qspi_io(SPI2_HOST, GPIO_NUM_21, 80 * 1000 * 1000,
                                          LCD_STRIPE_BYTES, &lcd), TAG, "LCD QSPI");
    ESP_RETURN_ON_ERROR(output(LCD_RESET, 0), TAG, "Assert LCD reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_RESET, 1), TAG, "Release LCD reset");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(sdgoods_panel_initialize(lcd), TAG, "Initialize ST77916");
    ESP_LOGI(TAG, "ST77916 360x360 ready");
    return ESP_OK;
}

esp_err_t sugarat_display_write(int x, int y, int width, int height, const uint8_t *pixels)
{
    ESP_RETURN_ON_FALSE(pixels && x >= 0 && y >= 0 && width > 0 && height > 0 &&
                        width <= SCREEN_SIZE - x && height <= SCREEN_SIZE - y,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid LCD window");
    const int right = x + width - 1;
    const uint8_t columns[] = {x >> 8, x & 0xff, right >> 8, right & 0xff};
    const unsigned row_bytes = width * sizeof(uint16_t);
    const int rows_per_transfer = LCD_STRIPE_BYTES / row_bytes;
    for (int top = y; top < y + height; top += rows_per_transfer) {
        const int remaining = y + height - top;
        const int lines = remaining < rows_per_transfer ? remaining : rows_per_transfer;
        const int bottom = top + lines - 1;
        const uint8_t rows[] = {top >> 8, top & 0xff, bottom >> 8, bottom & 0xff};
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd, 0x02002A00, columns, sizeof(columns)),
                            TAG, "LCD columns");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd, 0x02002B00, rows, sizeof(rows)),
                            TAG, "LCD rows");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(lcd, 0x32002C00, pixels + (top - y) * row_bytes,
                                                    lines * row_bytes), TAG, "LCD pixels");
    }
    return ESP_OK;
}

esp_err_t sugarat_display_light(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "Backlight timer");
    const ledc_channel_config_t pwm = {
        .gpio_num = LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 760,
    };
    return ledc_channel_config(&pwm);
}

esp_err_t sugarat_touch_init(void)
{
    ESP_RETURN_ON_ERROR(output(TOUCH_RESET, 0), TAG, "Assert touch reset");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RESET, 1), TAG, "Release touch reset");
    vTaskDelay(pdMS_TO_TICKS(60));

    const i2c_master_bus_config_t pins = {
        .i2c_port = I2C_NUM_1, .sda_io_num = GPIO_NUM_11, .scl_io_num = GPIO_NUM_10,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&pins, &bus), TAG, "Touch I2C bus");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, 0x15, 90), TAG, "CST816 probe");
    const i2c_device_config_t controller = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x15, .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &controller, &touch), TAG, "CST816 device");
    ESP_LOGI(TAG, "CST816 touch ready");
    return ESP_OK;
}

esp_err_t sugarat_touch_read(bool *pressed, int *x, int *y)
{
    const uint8_t reg = 0x02;
    uint8_t raw[5];
    const esp_err_t err = i2c_master_transmit_receive(touch, &reg, 1, raw, sizeof(raw), 20);
    if (err != ESP_OK) {
        return err;
    }
    const int fingers = raw[0] & 0xf;
    *pressed = fingers == 1;
    if (!*pressed) {
        return ESP_OK;
    }
    const int raw_x = ((raw[1] & 0xf) << 8) | raw[2];
    const int raw_y = ((raw[3] & 0xf) << 8) | raw[4];
    if (raw_x >= SCREEN_SIZE || raw_y >= SCREEN_SIZE) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *x = raw_y;
    *y = SCREEN_SIZE - 1 - raw_x;
    return ESP_OK;
}
