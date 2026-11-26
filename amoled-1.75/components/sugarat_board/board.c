#include "board.h"

#include "co5300_qspi_io.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Connections and panel sequence from Waveshare's maintained 1.75 BSP.
// See README.md for source links. The 1.75C is a different board.
enum { LCD_STRIPE_ROWS = 12, LCD_COLUMN_OFFSET = 6,
       LCD_STRIPE_BYTES = AMOLED_PANEL_SIZE * LCD_STRIPE_ROWS * sizeof(uint16_t) };

static const char *TAG = "amoled_board";
static i2c_master_bus_handle_t bus;
static esp_lcd_panel_io_handle_t lcd;

esp_err_t amoled_board_init(void)
{
    if (bus) return ESP_OK;
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_15, .scl_io_num = GPIO_NUM_14,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &bus), TAG, "Shared I2C bus");
    // The board powers these peripherals from its existing VCC3V3 rail.
    // Do not reprogram unrelated PMIC rails or the battery charging profile.
    ESP_LOGI(TAG, "Shared I2C ready; AXP2101=%s",
             i2c_master_probe(bus, 0x34, 100) == ESP_OK ? "present" : "not detected");
    return ESP_OK;
}

i2c_master_bus_handle_t amoled_i2c_bus(void)
{
    return bus;
}

static esp_err_t panel_command(uint8_t command, const uint8_t *data, size_t size)
{
    return esp_lcd_panel_io_tx_param(lcd, 0x02000000 | ((uint32_t)command << 8), data, size);
}

esp_err_t sugarat_display_init(void)
{
    ESP_RETURN_ON_ERROR(amoled_board_init(), TAG, "Board initialization");
    const spi_bus_config_t config = {
        .sclk_io_num = GPIO_NUM_38,
        .data0_io_num = GPIO_NUM_4, .data1_io_num = GPIO_NUM_5,
        .data2_io_num = GPIO_NUM_6, .data3_io_num = GPIO_NUM_7,
        .data4_io_num = -1, .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = LCD_STRIPE_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &config, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    ESP_RETURN_ON_ERROR(co5300_new_qspi_io(SPI2_HOST, GPIO_NUM_12, 40 * 1000 * 1000,
                                          LCD_STRIPE_BYTES, &lcd), TAG, "LCD QSPI");
    const gpio_config_t reset = {.pin_bit_mask = 1ULL << GPIO_NUM_39, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&reset), TAG, "LCD reset pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_39, 0), TAG, "Assert LCD reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_39, 1), TAG, "Release LCD reset");
    vTaskDelay(pdMS_TO_TICKS(120));
    static const uint8_t commands[][2] = {
        {0xFE, 0x20}, {0x19, 0x10}, {0x1C, 0xA0}, {0xFE, 0x00},
        {0xC4, 0x80}, {0x3A, 0x55}, {0x36, 0x00}, {0x35, 0x00},
        {0x53, 0x20}, {0x51, 0x00}, {0x63, 0xFF},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        ESP_RETURN_ON_ERROR(panel_command(commands[i][0], &commands[i][1], 1), TAG, "Panel configuration");
    }
    const uint8_t columns[] = {0x00, 0x06, 0x01, 0xD7};
    const uint8_t rows[] = {0x00, 0x00, 0x01, 0xD1};
    ESP_RETURN_ON_ERROR(panel_command(0x2A, columns, sizeof(columns)), TAG, "Panel columns");
    ESP_RETURN_ON_ERROR(panel_command(0x2B, rows, sizeof(rows)), TAG, "Panel rows");
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP_RETURN_ON_ERROR(panel_command(0x11, NULL, 0), TAG, "Panel sleep out");
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP_RETURN_ON_ERROR(panel_command(0x29, NULL, 0), TAG, "Panel display on");
    ESP_LOGI(TAG, "CO5300 AMOLED 466x466 ready");
    return ESP_OK;
}

esp_err_t sugarat_display_write(int x, int y, int width, int height, const uint8_t *pixels)
{
    ESP_RETURN_ON_FALSE(lcd && pixels && x >= 0 && y >= 0 && width > 0 && height > 0 &&
                        width <= AMOLED_PANEL_SIZE - x && height <= AMOLED_PANEL_SIZE - y &&
                        !((x | y | width | height) & 1),
                        ESP_ERR_INVALID_ARG, TAG, "Invalid AMOLED window");
    const int left = x + LCD_COLUMN_OFFSET, right = left + width - 1;
    const uint8_t columns[] = {left >> 8, left & 0xff, right >> 8, right & 0xff};
    const int row_bytes = width * sizeof(uint16_t);
    // Each transfer starts and ends on a two-row boundary.
    const int stripe_rows = (LCD_STRIPE_BYTES / row_bytes) & ~1;
    for (int top = y; top < y + height; top += stripe_rows) {
        const int remaining = y + height - top;
        const int lines = remaining < stripe_rows ? remaining : stripe_rows;
        const int bottom = top + lines - 1;
        const uint8_t rows[] = {top >> 8, top & 0xff, bottom >> 8, bottom & 0xff};
        ESP_RETURN_ON_ERROR(panel_command(0x2A, columns, sizeof(columns)), TAG, "LCD columns");
        ESP_RETURN_ON_ERROR(panel_command(0x2B, rows, sizeof(rows)), TAG, "LCD rows");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(lcd, 0x32002C00,
                            pixels + (top - y) * row_bytes, lines * row_bytes), TAG, "LCD pixels");
    }
    return ESP_OK;
}

esp_err_t sugarat_display_set_brightness(unsigned percent)
{
    if (!lcd) return ESP_ERR_INVALID_STATE;
    if (percent < SUGARAT_BRIGHTNESS_MIN || percent > 100) return ESP_ERR_INVALID_ARG;
    const uint8_t brightness = percent * 255 / 100;
    return panel_command(0x51, &brightness, 1);
}

esp_err_t sugarat_display_light(unsigned percent)
{
    return sugarat_display_set_brightness(percent);
}

