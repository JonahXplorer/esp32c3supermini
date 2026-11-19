#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "servo_radio.h"

#define SERVO_PWM_RESOLUTION LEDC_TIMER_14_BIT

enum {
    BUTTON_GPIO = GPIO_NUM_4,
    BUTTON_POLL_MS = 10,
    BUTTON_DEBOUNCE_SAMPLES = 3,
    SERVO_1_GPIO = 6,
    SERVO_2_GPIO = 7,
    SERVO_PWM_HZ = 50,
    SERVO_GPIO6_A_US = 1000,
    SERVO_GPIO6_B_US = 2000,
    SERVO_GPIO7_A_US = 2000,
    SERVO_GPIO7_B_US = 1000,
};

enum {
    SERVO_POSITION_A,
    SERVO_POSITION_B,
    SERVO_POSITION_COUNT,
};

_Static_assert(SERVO_GPIO6_A_US != SERVO_GPIO6_B_US, "GPIO6 positions must differ");
_Static_assert(SERVO_GPIO7_A_US != SERVO_GPIO7_B_US, "GPIO7 positions must differ");

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

    struct {
        int gpio;
        ledc_channel_t channel;
        int position_us[SERVO_POSITION_COUNT];
        int current_us;
    } servos[] = {
        {
            .gpio = SERVO_1_GPIO,
            .channel = LEDC_CHANNEL_0,
            .position_us = {
                [SERVO_POSITION_A] = SERVO_GPIO6_A_US,
                [SERVO_POSITION_B] = SERVO_GPIO6_B_US,
            },
        },
        {
            .gpio = SERVO_2_GPIO,
            .channel = LEDC_CHANNEL_1,
            .position_us = {
                [SERVO_POSITION_A] = SERVO_GPIO7_A_US,
                [SERVO_POSITION_B] = SERVO_GPIO7_B_US,
            },
        },
    };
    int target_position = SERVO_POSITION_A;
    const size_t servo_count = sizeof(servos) / sizeof(servos[0]);
    for (size_t i = 0; i < servo_count; ++i) {
        servos[i].current_us = servos[i].position_us[target_position];
        const ledc_channel_config_t channel = {
            .gpio_num = servos[i].gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = servos[i].channel,
            .timer_sel = LEDC_TIMER_0,
            .duty = pulse_to_duty(servos[i].current_us),
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }

    const bool radio_ready = servo_radio_start(SERVO_PEER_MAC) == ESP_OK;
    ESP_LOGI(TAG, "Ready at A: GPIO%d=%d us, GPIO%d=%d us; press GPIO%d to switch",
             SERVO_1_GPIO, SERVO_GPIO6_A_US, SERVO_2_GPIO, SERVO_GPIO7_A_US, BUTTON_GPIO);
    bool button_pressed = false;
    unsigned consecutive_samples = 0;
    int move_step_us = SERVO_SPEED_DEFAULT;
    uint32_t last_session = 0;
    uint32_t last_sequence = 0;
    bool announced_peer = false;
    while (true) {
        const bool pressed = gpio_get_level(BUTTON_GPIO) == 1;
        if (pressed == button_pressed) {
            consecutive_samples = 0;
        } else if (++consecutive_samples == BUTTON_DEBOUNCE_SAMPLES) {
            consecutive_samples = 0;
            button_pressed = pressed;
            if (button_pressed) {
                target_position = (target_position == SERVO_POSITION_A)
                                  ? SERVO_POSITION_B : SERVO_POSITION_A;
                ESP_LOGI(TAG, "Button pressed; moving to %c: GPIO%d=%d us, GPIO%d=%d us",
                         target_position == SERVO_POSITION_A ? 'A' : 'B',
                         servos[0].gpio, servos[0].position_us[target_position],
                         servos[1].gpio, servos[1].position_us[target_position]);
            }
        }

        servo_message_t message;
        if (radio_ready && servo_radio_receive(&message)) {
            const bool new_command = message.session != last_session || message.sequence > last_sequence;
            if (new_command && message.type == SERVO_SET_POSITION && message.position < SERVO_POSITION_COUNT) {
                target_position = message.position;
                last_session = message.session;
                last_sequence = message.sequence;
                ESP_LOGI(TAG, "Sugarat requested position %c", 'A' + target_position);
            } else if (new_command && message.type == SERVO_SET_SPEED &&
                       message.speed >= SERVO_SPEED_MIN && message.speed <= SERVO_SPEED_MAX) {
                move_step_us = message.speed;
                last_session = message.session;
                last_sequence = message.sequence;
                ESP_LOGI(TAG, "Sugarat set speed to %d us per %d ms", move_step_us, BUTTON_POLL_MS);
            }
            if (!announced_peer && message.type == SERVO_QUERY) {
                announced_peer = true;
                ESP_LOGI(TAG, "Sugarat status request received");
            }
            if (message.type >= SERVO_QUERY && message.type <= SERVO_SET_SPEED) {
                const bool moving = servos[0].current_us != servos[0].position_us[target_position] ||
                                    servos[1].current_us != servos[1].position_us[target_position];
                (void)servo_radio_send((servo_message_t){
                    .session = message.session, .sequence = message.sequence, .type = SERVO_STATE,
                    .position = target_position, .speed = move_step_us, .moving = moving,
                });
            }
        }

        for (size_t i = 0; i < servo_count; ++i) {
            int step_us = servos[i].position_us[target_position] - servos[i].current_us;
            if (step_us == 0) {
                continue;
            }
            if (step_us > move_step_us) {
                step_us = move_step_us;
            } else if (step_us < -move_step_us) {
                step_us = -move_step_us;
            }
            servos[i].current_us += step_us;
            servo_set_pulse(servos[i].channel, servos[i].current_us);
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}
