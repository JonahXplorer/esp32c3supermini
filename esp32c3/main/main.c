#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_serial_output.h"
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
    SERVO_PERIOD_US = 1000000 / SERVO_PWM_HZ,
    SERVO_GPIO6_A_US = 1000,
    SERVO_GPIO6_90_US = 2000,
    SERVO_GPIO7_A_US = 2000,
    SERVO_GPIO7_90_US = 1000,
    PROBE_MIN_US = 500,
    PROBE_MAX_US = 2500,
    PROBE_STEP_US = 20,
    PROBE_GPIO6 = 1 << 0,
    PROBE_GPIO7 = 1 << 1,
    PROBE_BOTH = PROBE_GPIO6 | PROBE_GPIO7,
};

enum {
    SERVO_POSITION_A,
    SERVO_POSITION_B,
    SERVO_POSITION_COUNT,
};

_Static_assert(SERVO_GPIO6_MAX_US == 0 ||
               (SERVO_GPIO6_MAX_US > SERVO_GPIO6_90_US && SERVO_GPIO6_MAX_US < SERVO_PERIOD_US),
               "GPIO6 maximum-angle pulse must be above its 90-degree pulse and within the PWM period");
_Static_assert(SERVO_GPIO7_MAX_US == 0 ||
               (SERVO_GPIO7_MAX_US > 0 && SERVO_GPIO7_MAX_US < SERVO_GPIO7_90_US),
               "GPIO7 maximum-angle pulse must be positive and below its 90-degree pulse");

typedef struct {
    int gpio;
    ledc_channel_t channel;
    int a_us;
    int at_90_us;
    int at_max_us;
    int current_us;
} servo_t;

typedef struct {
    unsigned mask;
    int extra_us;
} probe_t;

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

static int servo_target_us(const servo_t *servo, int position, int angle_deg)
{
    if (position == SERVO_POSITION_A) {
        return servo->a_us;
    }
    const bool beyond_90 = angle_deg > SERVO_ANGLE_DEFAULT;
    const int start_us = beyond_90 ? servo->at_90_us : servo->a_us;
    const int end_us = beyond_90 ? servo->at_max_us : servo->at_90_us;
    const int segment_deg = beyond_90 ? angle_deg - SERVO_ANGLE_DEFAULT : angle_deg;
    const int segment_span_deg = beyond_90 ? SERVO_ANGLE_MAX - SERVO_ANGLE_DEFAULT : SERVO_ANGLE_DEFAULT;
    const int offset = (end_us - start_us) * segment_deg;
    const int half = segment_span_deg / 2;
    return start_us + (offset + (offset < 0 ? -half : half)) / segment_span_deg;
}

static int probe_pulse_us(const servo_t *servo, int extra_us)
{
    return servo->at_90_us + (servo->at_90_us > servo->a_us ? extra_us : -extra_us);
}

static void poll_probe(const servo_t servos[], size_t servo_count, probe_t *probe)
{
    static uint8_t command;
    uint8_t byte;
    if (esp_rom_output_rx_one_char(&byte) != 0) {
        return;
    }
    if (byte != '\r' && byte != '\n') {
        // One command per line, so holding a key cannot repeatedly step a servo.
        command = command == 0 ? byte : '?';
        return;
    }
    const uint8_t requested = command;
    command = 0;

    if (requested == 'h') {
        ESP_LOGI(TAG, "Pulse probe: b selects both, 6/7 selects GPIO, +/- steps by %d us, q exits", PROBE_STEP_US);
        return;
    }
    if (requested == 'b' || requested == '6' || requested == '7') {
        probe->mask = requested == 'b' ? PROBE_BOTH : 1u << (requested - '6');
        probe->extra_us = 0;
    } else if (probe->mask == 0) {
        return;
    } else if (requested == 'q') {
        probe->mask = 0;
        ESP_LOGI(TAG, "Pulse probe ended; button and Sugarat control restored");
        return;
    } else if (requested == '+' || requested == '-') {
        const int step = requested == '+' ? PROBE_STEP_US : -PROBE_STEP_US;
        const int next_extra_us = probe->extra_us + step;
        for (size_t i = 0; i < servo_count; ++i) {
            if ((probe->mask & (1u << i)) == 0) {
                continue;
            }
            const servo_t *servo = &servos[i];
            if (servo->current_us != probe_pulse_us(servo, probe->extra_us)) {
                return;
            }
            const int next_us = probe_pulse_us(servo, next_extra_us);
            if (next_extra_us < 0 || next_us < PROBE_MIN_US || next_us > PROBE_MAX_US) {
                ESP_LOGI(TAG, "Pulse probe holding at %d us of extra travel", probe->extra_us);
                return;
            }
        }
        probe->extra_us = next_extra_us;
    } else {
        return;
    }
    if (probe->mask == PROBE_BOTH) {
        ESP_LOGI(TAG, "Pulse probe GPIO6 target %d us, GPIO7 target %d us",
                 probe_pulse_us(&servos[0], probe->extra_us), probe_pulse_us(&servos[1], probe->extra_us));
    } else {
        const servo_t *servo = &servos[probe->mask == PROBE_GPIO6 ? 0 : 1];
        ESP_LOGI(TAG, "Pulse probe GPIO%d target %d us", servo->gpio, probe_pulse_us(servo, probe->extra_us));
    }
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

    servo_t servos[] = {
        {
            .gpio = SERVO_1_GPIO,
            .channel = LEDC_CHANNEL_0,
            .a_us = SERVO_GPIO6_A_US,
            .at_90_us = SERVO_GPIO6_90_US,
            .at_max_us = SERVO_GPIO6_MAX_US,
        },
        {
            .gpio = SERVO_2_GPIO,
            .channel = LEDC_CHANNEL_1,
            .a_us = SERVO_GPIO7_A_US,
            .at_90_us = SERVO_GPIO7_90_US,
            .at_max_us = SERVO_GPIO7_MAX_US,
        },
    };
    int target_position = SERVO_POSITION_A;
    const size_t servo_count = sizeof(servos) / sizeof(servos[0]);
    for (size_t i = 0; i < servo_count; ++i) {
        servos[i].current_us = servos[i].a_us;
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
    int angle_deg = SERVO_ANGLE_DEFAULT;
    const int angle_limit_deg = SERVO_GPIO6_MAX_US && SERVO_GPIO7_MAX_US
                                ? SERVO_ANGLE_MAX : SERVO_ANGLE_DEFAULT;
    probe_t probe = {0};
    ESP_LOGI(TAG, "Angle range 0-%d degrees, startup B angle %d degrees", angle_limit_deg, angle_deg);
    uint32_t last_session = 0;
    uint32_t last_sequence = 0;
    bool announced_peer = false;
    while (true) {
        poll_probe(servos, servo_count, &probe);
        const bool pressed = gpio_get_level(BUTTON_GPIO) == 1;
        if (pressed == button_pressed) {
            consecutive_samples = 0;
        } else if (++consecutive_samples == BUTTON_DEBOUNCE_SAMPLES) {
            consecutive_samples = 0;
            button_pressed = pressed;
            if (button_pressed && probe.mask == 0) {
                target_position = (target_position == SERVO_POSITION_A)
                                  ? SERVO_POSITION_B : SERVO_POSITION_A;
                ESP_LOGI(TAG, "Button pressed; moving to %c: GPIO%d=%d us, GPIO%d=%d us",
                         target_position == SERVO_POSITION_A ? 'A' : 'B',
                         servos[0].gpio, servo_target_us(&servos[0], target_position, angle_deg),
                         servos[1].gpio, servo_target_us(&servos[1], target_position, angle_deg));
            }
        }

        servo_message_t message;
        const bool received = radio_ready && servo_radio_receive(&message);
        bool reply = false;
        if (received && probe.mask == 0) {
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
            } else if (new_command && message.type == SERVO_SET_ANGLE && message.angle_deg <= angle_limit_deg) {
                angle_deg = message.angle_deg;
                last_session = message.session;
                last_sequence = message.sequence;
                ESP_LOGI(TAG, "Sugarat set B angle to %d degrees: GPIO%d=%d us, GPIO%d=%d us", angle_deg,
                         servos[0].gpio, servo_target_us(&servos[0], SERVO_POSITION_B, angle_deg),
                         servos[1].gpio, servo_target_us(&servos[1], SERVO_POSITION_B, angle_deg));
            }
            if (!announced_peer && message.type == SERVO_QUERY) {
                announced_peer = true;
                ESP_LOGI(TAG, "Sugarat status request received");
            }
            reply = message.type >= SERVO_QUERY && message.type <= SERVO_SET_ANGLE;
        }

        bool moving = false;
        for (size_t i = 0; i < servo_count; ++i) {
            const int target_us = probe.mask & (1u << i) ? probe_pulse_us(&servos[i], probe.extra_us)
                                                         : servo_target_us(&servos[i], target_position, angle_deg);
            int step_us = target_us - servos[i].current_us;
            if (step_us == 0) {
                continue;
            }
            moving = true;
            if (step_us > move_step_us) {
                step_us = move_step_us;
            } else if (step_us < -move_step_us) {
                step_us = -move_step_us;
            }
            servos[i].current_us += step_us;
            servo_set_pulse(servos[i].channel, servos[i].current_us);
        }
        if (reply) {
            (void)servo_radio_send((servo_message_t){
                .session = message.session, .sequence = message.sequence, .type = SERVO_STATE,
                .position = target_position, .speed = move_step_us, .moving = moving,
                .angle_deg = angle_deg, .angle_limit_deg = angle_limit_deg,
            });
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}
