#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101, ESP_ERR_INVALID_ARG = 0x102,
       ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_NOT_SUPPORTED = 0x106,
       ESP_ERR_TIMEOUT = 0x107, ESP_ERR_INVALID_RESPONSE = 0x108 };
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define ESP_INTR_FLAG_IRAM 1
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { esp_err_t e = (call); if (e != ESP_OK) return e; } while (0)
#define ESP_RETURN_ON_FALSE(test, err, tag, ...) do { if (!(test)) return (err); } while (0)
#define ESP_GOTO_ON_ERROR(call, label, tag, ...) do { esp_err_t e = (call); if (e != ESP_OK) { ret = e; goto label; } } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ms);
int64_t esp_timer_get_time(void);
const char *esp_err_to_name(esp_err_t err);

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
enum { I2C_ADDR_BIT_LEN_7, GPIO_NUM_11 = 11, GPIO_NUM_40 = 40,
       GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_PULLUP_ENABLE, GPIO_INTR_NEGEDGE };
typedef struct { int dev_addr_length; unsigned device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, intr_type; } gpio_config_t;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *device);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *command, size_t command_bytes,
                                      uint8_t *data, size_t bytes, int timeout);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t bytes, int timeout);
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(int pin, int value);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(int pin, void (*handler)(void *), void *context);

#define LV_ATTRIBUTE_MEM_ALIGN
typedef struct { int x, y; } lv_point_t;
typedef enum { LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED } lv_indev_state_t;
typedef struct { lv_point_t point; lv_indev_state_t state; } lv_indev_data_t;
typedef struct { int x1, y1, x2, y2; } lv_area_t;
typedef struct { int unused; } lv_display_t;
typedef struct { int unused; } lv_indev_t;
enum { LV_COLOR_FORMAT_RGB565, LV_DISPLAY_RENDER_MODE_PARTIAL, LV_INDEV_TYPE_POINTER };
static inline int lv_area_get_width(const lv_area_t *a) { return a->x2 - a->x1 + 1; }
static inline int lv_area_get_height(const lv_area_t *a) { return a->y2 - a->y1 + 1; }
void lv_draw_sw_rgb565_swap(void *pixels, unsigned count);
void lv_display_flush_ready(lv_display_t *display);
static inline void lv_init(void) {}
static inline void lv_tick_set_cb(uint32_t (*callback)(void)) {}
static inline lv_display_t *lv_display_create(int width, int height) { return (lv_display_t *)1; }
static inline void lv_display_set_theme(lv_display_t *display, void *theme) {}
static inline void lv_display_set_color_format(lv_display_t *display, int format) {}
static inline void lv_display_set_buffers(lv_display_t *display, void *a, void *b, size_t bytes, int mode) {}
static inline void lv_display_set_flush_cb(lv_display_t *display, void (*callback)(lv_display_t *, const lv_area_t *, uint8_t *)) {}
static inline void *lv_display_get_refr_timer(lv_display_t *display) { return NULL; }
static inline void lv_timer_ready(void *timer) {}
static inline lv_indev_t *lv_indev_create(void) { return (lv_indev_t *)1; }
static inline void lv_indev_set_type(lv_indev_t *input, int type) {}
static inline void lv_indev_set_read_cb(lv_indev_t *input, void (*callback)(lv_indev_t *, lv_indev_data_t *)) {}
