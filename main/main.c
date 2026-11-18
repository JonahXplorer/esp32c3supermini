#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SERVO_PWM_RESOLUTION LEDC_TIMER_14_BIT

enum {
    BUTTON_GPIO = GPIO_NUM_4,
    BUTTON_POLL_MS = 10,
    BUTTON_DEBOUNCE_SAMPLES = 3,
    SERVO_1_GPIO = 6,
    SERVO_2_GPIO = 7,
    SERVO_PWM_HZ = 50,
    SERVO_POSITION_A_US = 1000,
    SERVO_POSITION_B_US = 2000,
    SERVO_MOVE_STEP_US = 10,
};

_Static_assert(SERVO_POSITION_A_US != SERVO_POSITION_B_US, "Servo positions must differ");

static const char *TAG = "dual_servo";

static uint32_t pulse_to_duty(uint32_t pulse_us)
{
    // Convert pulse width into timer counts and round to the nearest count.
    const uint32_t counts_per_period = 1U << SERVO_PWM_RESOLUTION;
    return (uint32_t)(((uint64_t)pulse_us * SERVO_PWM_HZ * counts_per_period + 500000U)
                      / 1000000U);
}

static void servo_set_pulse(ledc_channel_t channel, uint32_t pulse_us)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, pulse_to_duty(pulse_us)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}

void app_main(void)
{
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_PWM_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_PWM_HZ,
        .clk_cfg = LEDC_USE_XTAL_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const struct {
        int gpio;
        ledc_channel_t channel;
    } servos[] = {
        {SERVO_1_GPIO, LEDC_CHANNEL_0},
        {SERVO_2_GPIO, LEDC_CHANNEL_1},
    };
    const size_t servo_count = sizeof(servos) / sizeof(servos[0]);
    for (size_t i = 0; i < servo_count; ++i) {
        const ledc_channel_config_t channel = {
            .gpio_num = servos[i].gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = servos[i].channel,
            .timer_sel = LEDC_TIMER_0,
            .duty = pulse_to_duty(SERVO_POSITION_A_US),
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }

    ESP_LOGI(TAG, "Ready: press GPIO%d to switch both servos between %d and %d us",
             BUTTON_GPIO, SERVO_POSITION_A_US, SERVO_POSITION_B_US);
    bool button_pressed = false;
    unsigned consecutive_samples = 0;
    int pulse_us = SERVO_POSITION_A_US;
    int target_us = SERVO_POSITION_A_US;
    while (true) {
        const bool pressed = gpio_get_level(BUTTON_GPIO) == 1;
        if (pressed == button_pressed) {
            consecutive_samples = 0;
        } else if (++consecutive_samples == BUTTON_DEBOUNCE_SAMPLES) {
            consecutive_samples = 0;
            button_pressed = pressed;
            if (button_pressed) {
                target_us = target_us == SERVO_POSITION_A_US ? SERVO_POSITION_B_US : SERVO_POSITION_A_US;
                ESP_LOGI(TAG, "Button pressed; moving both servos to %d us", target_us);
            }
        }

        if (pulse_us != target_us) {
            int step_us = target_us - pulse_us;
            if (step_us > SERVO_MOVE_STEP_US) {
                step_us = SERVO_MOVE_STEP_US;
            } else if (step_us < -SERVO_MOVE_STEP_US) {
                step_us = -SERVO_MOVE_STEP_US;
            }
            pulse_us += step_us;
            for (size_t i = 0; i < servo_count; ++i) {
                servo_set_pulse(servos[i].channel, pulse_us);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}
