#include "power.h"

#include <stdatomic.h>
#include <stddef.h>

#include "board_pins.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    KEY_SAMPLE_MS = 10,
    KEY_DEBOUNCE_US = 40000,
    POWER_ON_HOLD_US = 1000000,
    POWER_OFF_HOLD_US = 1200000,
};

static const char *TAG = "sugarat_power";
static const struct {
    gpio_num_t pin;
    int level;
} off_outputs[] = {
    {AMPLIFIER_ENABLE, 0},
    {LCD_BACKLIGHT, 0},
    {LCD_RESET, 0},
    {LCD_POWER, 1}, // The panel supply is active low.
    {TOUCH_RESET, 0},
};

static esp_timer_handle_t key_timer;
static atomic_bool off_requested;
static struct {
    bool raw_down;
    bool stable_down;
    bool armed;
    int64_t changed_at;
    int64_t pressed_at;
} key;

static bool key_down(void)
{
    return gpio_get_level(POWER_KEY) == 0;
}

static void set_output(gpio_num_t pin, int level)
{
    // Set the output latch first so releasing a retained hold cannot pulse the pin.
    ESP_ERROR_CHECK(gpio_set_level(pin, level));
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_hold_dis(pin));
}

static void power_down_peripherals(void)
{
    for (size_t i = 0; i < sizeof off_outputs / sizeof off_outputs[0]; ++i) {
        set_output(off_outputs[i].pin, off_outputs[i].level);
    }
}

static _Noreturn void sleep_with_latch_off(void)
{
    int64_t released_at = esp_timer_get_time();
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (key_down()) {
            released_at = now;
        } else if (now - released_at >= KEY_DEBOUNCE_US) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_SAMPLE_MS));
    }

    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(POWER_KEY, 0));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(POWER_KEY));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(POWER_KEY));
    for (size_t i = 0; i < sizeof off_outputs / sizeof off_outputs[0]; ++i) {
        ESP_ERROR_CHECK(gpio_hold_en(off_outputs[i].pin));
    }

    // Releasing the latch cuts battery power. USB can still power the chip,
    // so keep the peripherals off and allow only the key to wake it.
    ESP_ERROR_CHECK(gpio_set_level(BATTERY_LATCH, 0));
    ESP_ERROR_CHECK(gpio_hold_en(BATTERY_LATCH));
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
}

static bool continuing_after_reset(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_SW:
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_USB:
        case ESP_RST_JTAG:
            return true;
        default:
            return false;
    }
}

static bool startup_hold_met(void)
{
    const int64_t start = esp_timer_get_time();
    while (key_down()) {
        if (esp_timer_get_time() - start >= POWER_ON_HOLD_US) return true;
        vTaskDelay(pdMS_TO_TICKS(KEY_SAMPLE_MS));
    }
    return false;
}

static void sample_key(void *context)
{
    (void)context;
    const int64_t now = esp_timer_get_time();
    const bool down = key_down();
    if (down != key.raw_down) {
        key.raw_down = down;
        key.changed_at = now;
    }
    if (now - key.changed_at < KEY_DEBOUNCE_US) return;

    if (key.stable_down != down) {
        key.stable_down = down;
        if (down) {
            key.pressed_at = key.changed_at;
        } else if (key.armed && key.changed_at - key.pressed_at >= POWER_OFF_HOLD_US) {
            atomic_store(&off_requested, true);
        }
    }
    if (!down) key.armed = true;
}

void sugarat_power_start(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool continuing = continuing_after_reset(reason);
    set_output(BATTERY_LATCH, continuing);
    power_down_peripherals();
    gpio_deep_sleep_hold_dis();

    // EXT0 wake leaves the pin routed to the RTC peripheral.
    ESP_ERROR_CHECK(rtc_gpio_deinit(POWER_KEY));
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << POWER_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    if (!continuing && !startup_hold_met()) {
        ESP_LOGI(TAG, "Power-on hold not met; staying off (reset=%d)", reason);
        sleep_with_latch_off();
    }
    ESP_ERROR_CHECK(gpio_set_level(BATTERY_LATCH, 1));
    ESP_LOGI(TAG, "Power on (reset=%d)", reason);

    key.raw_down = key_down();
    key.changed_at = esp_timer_get_time();
    const esp_timer_create_args_t timer = {
        .callback = sample_key,
        .name = "power_key",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer, &key_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(key_timer, KEY_SAMPLE_MS * 1000));
}

bool sugarat_power_off_requested(void)
{
    return atomic_load(&off_requested);
}

_Noreturn void sugarat_power_off(void)
{
    ESP_ERROR_CHECK(esp_timer_stop(key_timer));
    power_down_peripherals();
    ESP_LOGI(TAG, "Power off; releasing battery latch");
    sleep_with_latch_off();
}
