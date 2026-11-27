#pragma once
#include "audio_stub.h"
enum { SPI_TRANS_MODE_QIO = 16, SPI_TRANS_USE_TXDATA = 32, SPI_DEVICE_HALFDUPLEX = 64 };
typedef int spi_host_device_t;
typedef void *spi_device_handle_t;
typedef struct {
    uint16_t cmd;
    uint32_t addr, flags;
    size_t length;
    const void *tx_buffer;
    uint8_t tx_data[4];
} spi_transaction_t;
typedef struct { int command_bits, address_bits, mode, clock_speed_hz, spics_io_num, flags, queue_size; } spi_device_interface_config_t;
typedef struct esp_lcd_panel_io_t esp_lcd_panel_io_t;
typedef esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct { int unused; } esp_lcd_panel_io_callbacks_t;
struct esp_lcd_panel_io_t {
    esp_err_t (*tx_param)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*tx_color)(esp_lcd_panel_io_t *, int, const void *, size_t);
    esp_err_t (*rx_param)(esp_lcd_panel_io_t *, int, void *, size_t);
    esp_err_t (*del)(esp_lcd_panel_io_t *);
    esp_err_t (*register_event_callbacks)(esp_lcd_panel_io_t *, const esp_lcd_panel_io_callbacks_t *, void *);
};
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t);
void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps);
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t, size_t *);
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t *, TickType_t);
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t **, TickType_t);
