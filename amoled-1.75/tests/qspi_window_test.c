#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qspi_stub.h"
#include "../../sugarat/components/sugarat_board/drivers/co5300_qspi_io.c"

static spi_transaction_t *queued[3];
static unsigned submitted, returned, frees;
static bool locked, timeout, dma_error;
static int fail_queue = -1;

const char *esp_err_to_name(esp_err_t err) { return "test"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t sem, unsigned wait)
{ assert(sem == (void *)1 && !locked); locked = true; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t sem)
{ assert(sem == (void *)1 && locked); locked = false; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t sem) { assert(!locked); }
void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps) { return calloc(count, bytes); }
void *heap_caps_aligned_alloc(size_t align, size_t bytes, uint32_t caps)
{ assert(align == 4); return malloc(bytes); }
void heap_caps_free(void *memory) { ++frees; free(memory); }
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *device)
{ assert(cfg->queue_size == 3 && cfg->clock_speed_hz == 40000000); *device = (void *)2; return ESP_OK; }
esp_err_t spi_bus_remove_device(spi_device_handle_t device) { return ESP_OK; }
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t host, size_t *bytes) { *bytes = 4096; return ESP_OK; }
esp_err_t spi_device_queue_trans(spi_device_handle_t device, spi_transaction_t *t, TickType_t wait)
{
    assert(locked);
    if ((int)submitted == fail_queue) return ESP_FAIL;
    assert(submitted < 3); queued[submitted++] = t; return ESP_OK;
}
esp_err_t spi_device_get_trans_result(spi_device_handle_t device, spi_transaction_t **result, TickType_t wait)
{
    assert(locked); *result = NULL;
    if (timeout) return ESP_ERR_TIMEOUT;
    assert(returned < submitted); *result = queued[returned++];
    if (dma_error) { dma_error = false; return ESP_FAIL; }
    return ESP_OK;
}
static void reset_queue(void) { submitted = returned = 0; fail_queue = -1; timeout = dma_error = false; }

int main(void)
{
    esp_lcd_panel_io_handle_t handle;
    assert(co5300_new_qspi_io(0, 12, 40000000, 1024, &handle) == ESP_OK);
    uint8_t columns[] = {0, 6, 1, 0xd7}, rows[] = {0, 0, 0, 31}, pixels[512];
    memset(pixels, 0xa5, sizeof(pixels));
    assert(co5300_qspi_write_window(handle, columns, rows, pixels, sizeof(pixels)) == ESP_OK);
    assert(submitted == 3 && returned == 3);
    assert(queued[0]->cmd == 2 && queued[0]->addr == 0x2a00 && queued[0]->flags == SPI_TRANS_USE_TXDATA);
    assert(queued[1]->cmd == 2 && queued[1]->addr == 0x2b00);
    assert(!memcmp(queued[0]->tx_data, columns, 4) && !memcmp(queued[1]->tx_data, rows, 4));
    assert(queued[2]->cmd == 0x32 && queued[2]->addr == 0x2c00 && queued[2]->flags == SPI_TRANS_MODE_QIO);
    assert(queued[2]->length == sizeof(pixels) * 8 && queued[2]->tx_buffer != pixels);

    reset_queue(); timeout = true;
    assert(co5300_qspi_write_window(handle, columns, rows, pixels, sizeof(pixels)) == ESP_ERR_TIMEOUT);
    memset(pixels, 0x55, sizeof(pixels)); columns[1] = 0xff;
    assert(queued[0]->tx_data[1] == 6 && ((const uint8_t *)queued[2]->tx_buffer)[0] == 0xa5);
    const unsigned before = frees;
    assert(handle->del(handle) == ESP_ERR_TIMEOUT && frees == before);
    bool recovered = true;
    assert(co5300_qspi_io_recover(handle, &recovered) == ESP_ERR_TIMEOUT && !recovered);
    timeout = false;
    assert(co5300_qspi_io_recover(handle, &recovered) == ESP_OK && recovered && returned == 3);

    reset_queue(); fail_queue = 1;
    assert(co5300_qspi_write_window(handle, columns, rows, pixels, sizeof(pixels)) == ESP_FAIL && submitted == 1);
    fail_queue = -1;
    assert(co5300_qspi_io_recover(handle, &recovered) == ESP_OK && recovered && returned == 1);

    reset_queue(); dma_error = true;
    assert(co5300_qspi_write_window(handle, columns, rows, pixels, sizeof(pixels)) == ESP_FAIL && returned == 1);
    assert(co5300_qspi_io_recover(handle, &recovered) == ESP_OK && recovered && returned == 3);
    reset_queue();
    assert(handle->tx_param(handle, 0x02005100, pixels, 1) == ESP_OK && submitted == 1 && returned == 1);
    assert(handle->del(handle) == ESP_OK && frees == before + 2);
    puts("PASS: queued CASET/RASET/pixels, owned buffers, partial enqueue, timeout/DMA-error recovery and legacy commands");
    return 0;
}
