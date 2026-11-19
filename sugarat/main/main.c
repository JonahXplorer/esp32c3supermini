#include "board.h"
#include "screen.h"
#include "servo_radio.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    POLL_MS = 10,
    QUERY_US = 300000,
    RETRY_US = 150000,
    LINK_TIMEOUT_US = 1500000,
    MAX_SENDS = 4,
};

static const char *TAG = "sugarat_remote";

static bool create_command(screen_action_t action, screen_state_t state, servo_message_t *message)
{
    switch (action) {
    case SCREEN_SWITCH:
        message->type = SERVO_SET_POSITION;
        message->position = state.position ^ 1u;
        return true;
    case SCREEN_SLOWER:
    case SCREEN_FASTER: {
        const int increment = action == SCREEN_FASTER ? SERVO_SPEED_INCREMENT : -SERVO_SPEED_INCREMENT;
        const int speed = state.speed + increment;
        if (speed < SERVO_SPEED_MIN || speed > SERVO_SPEED_MAX) {
            return false;
        }
        message->type = SERVO_SET_SPEED;
        message->speed = speed;
        return true;
    }
    case SCREEN_ANGLE_DOWN:
    case SCREEN_ANGLE_UP: {
        const int increment = action == SCREEN_ANGLE_UP ? SERVO_ANGLE_INCREMENT : -SERVO_ANGLE_INCREMENT;
        const int angle = state.angle_deg + increment;
        if (angle < 0 || angle > state.angle_limit_deg) {
            return false;
        }
        message->type = SERVO_SET_ANGLE;
        message->angle_deg = angle;
        return true;
    }
    default:
        return false;
    }
}

static bool screen_changed(screen_state_t before, screen_state_t after)
{
    return before.online != after.online || before.pending != after.pending ||
           before.position != after.position || before.speed != after.speed || before.moving != after.moving ||
           before.angle_deg != after.angle_deg || before.angle_limit_deg != after.angle_limit_deg;
}

void app_main(void)
{
    ESP_ERROR_CHECK(sugarat_display_init());
    ESP_ERROR_CHECK(screen_init());
    const bool touch_ready = sugarat_touch_init() == ESP_OK;
    screen_state_t state = {
        .speed = SERVO_SPEED_DEFAULT, .angle_deg = SERVO_ANGLE_DEFAULT,
        .angle_limit_deg = SERVO_ANGLE_DEFAULT, .touch_ready = touch_ready,
    };
    ESP_ERROR_CHECK(screen_draw(state));
    ESP_ERROR_CHECK(sugarat_display_light());
    const bool radio_ready = servo_radio_start(SERVO_PEER_MAC) == ESP_OK;
    ESP_LOGI(TAG, "Remote ready on Sugarat Button");

    const uint32_t session = esp_random();
    uint32_t sequence = 0;
    uint32_t last_received_sequence = 0;
    int64_t last_reply = -LINK_TIMEOUT_US;
    int64_t next_query = 0;
    int64_t next_retry = 0;
    unsigned attempts = 0;
    servo_message_t pending = {0};
    bool pressed = false;

    while (true) {
        const int64_t now = esp_timer_get_time();
        const screen_state_t previous = state;
        servo_message_t reply;
        if (radio_ready && servo_radio_receive(&reply) && reply.type == SERVO_STATE && reply.session == session &&
            reply.sequence >= last_received_sequence && reply.sequence <= sequence &&
            reply.position <= 1 && reply.speed >= SERVO_SPEED_MIN && reply.speed <= SERVO_SPEED_MAX && reply.moving <= 1 &&
            (reply.angle_limit_deg == SERVO_ANGLE_DEFAULT || reply.angle_limit_deg == SERVO_ANGLE_MAX) &&
            reply.angle_deg <= reply.angle_limit_deg) {
            const bool confirmed_command = pending.type && pending.sequence == reply.sequence;
            if (confirmed_command) {
                pending.type = 0;
                next_query = now;
            }
            last_received_sequence = reply.sequence;
            last_reply = now;
            state.position = reply.position;
            state.speed = reply.speed;
            state.angle_deg = reply.angle_deg;
            state.angle_limit_deg = reply.angle_limit_deg;
            state.moving = reply.moving != 0;
            if (!previous.online || confirmed_command || state.position != previous.position ||
                state.speed != previous.speed || state.angle_deg != previous.angle_deg ||
                state.angle_limit_deg != previous.angle_limit_deg) {
                ESP_LOGI(TAG, "C3 confirmed target %c, speed %u us per 10 ms, B angle %u/%u degrees%s",
                         'A' + state.position, (unsigned)state.speed, (unsigned)state.angle_deg,
                         (unsigned)state.angle_limit_deg, state.moving ? ", moving" : "");
            }
        }
        state.online = now - last_reply < LINK_TIMEOUT_US;

        if (touch_ready) {
            bool touching;
            int x, y;
            if (sugarat_touch_read(&touching, &x, &y) == ESP_OK) {
                if (touching && !pressed && state.online && !pending.type &&
                    create_command(screen_hit(x, y), state, &pending)) {
                    pending.session = session;
                    pending.sequence = ++sequence;
                    attempts = 0;
                    next_retry = now;
                    ESP_LOGI(TAG, "Touch command %u", (unsigned)pending.type);
                }
                pressed = touching;
            }
        }

        if (radio_ready && pending.type && now >= next_retry) {
            if (attempts < MAX_SENDS) {
                (void)servo_radio_send(pending);
                ++attempts;
                next_retry = now + RETRY_US;
            } else {
                ESP_LOGW(TAG, "C3 did not acknowledge touch command");
                pending.type = 0;
                next_query = now;
                last_reply = now - LINK_TIMEOUT_US;
                state.online = false;
            }
        }
        if (radio_ready && !pending.type && now >= next_query) {
            (void)servo_radio_send((servo_message_t){.type = SERVO_QUERY, .session = session, .sequence = ++sequence});
            next_query = now + QUERY_US;
        }

        state.pending = pending.type != 0;
        if (screen_changed(previous, state)) {
            ESP_ERROR_CHECK(screen_draw(state));
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
