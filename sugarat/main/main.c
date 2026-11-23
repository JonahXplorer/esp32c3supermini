#include "board.h"
#include "screen.h"
#include "servo_radio.h"
#include "servo_tool.h"
#include "voice.h"

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
    STATUS_POLL_US = 200000,
    MAX_SENDS = 4,
    MAX_VOICE_SENDS = 8,
    REMOTE_STACK_BYTES = 8192,
};

static const char *TAG = "sugarat_remote";

static bool create_command(screen_action_t action, screen_state_t state, servo_message_t *message)
{
    switch (action.kind) {
    case SCREEN_SET_TARGET:
        if (action.value > 1 || action.value == state.position) {
            return false;
        }
        message->type = SERVO_SET_POSITION;
        message->position = action.value;
        return true;
    case SCREEN_SET_SPEED:
        if (action.value < SERVO_SPEED_MIN || action.value > SERVO_SPEED_MAX || action.value == state.speed) {
            return false;
        }
        message->type = SERVO_SET_SPEED;
        message->speed = action.value;
        return true;
    case SCREEN_SET_ANGLE:
        if (action.value > state.angle_limit_deg || action.value == state.angle_deg) {
            return false;
        }
        message->type = SERVO_SET_ANGLE;
        message->angle_deg = action.value;
        return true;
    default:
        return false;
    }
}

static void remote_task(void *context)
{
    ESP_ERROR_CHECK(sugarat_display_init());
    const bool touch_ready = sugarat_touch_init() == ESP_OK;
    ESP_ERROR_CHECK(screen_init(touch_ready));
    screen_state_t state = {
        .speed = SERVO_SPEED_DEFAULT, .angle_deg = SERVO_ANGLE_DEFAULT,
        .angle_limit_deg = SERVO_ANGLE_DEFAULT, .touch_ready = touch_ready,
    };
    screen_update(&state);
    (void)screen_poll();
    ESP_ERROR_CHECK(sugarat_display_light());
    ESP_ERROR_CHECK_WITHOUT_ABORT(servo_tool_init());
    const bool wifi_managed = sugarat_voice_start() == ESP_OK;
    const bool radio_ready = (wifi_managed ? servo_radio_attach(SERVO_PEER_MAC)
                                          : servo_radio_start(SERVO_PEER_MAC, SERVO_RADIO_FIXED)) == ESP_OK;
    ESP_LOGI(TAG, "Remote ready on Sugarat Button");

    const uint32_t session = esp_random();
    uint32_t sequence = 0;
    uint32_t last_received_sequence = 0;
    int64_t last_reply = -LINK_TIMEOUT_US;
    int64_t next_query = 0;
    int64_t next_retry = 0;
    int64_t next_status_poll = 0;
    unsigned attempts = 0;
    servo_message_t pending = {0};
    servo_tool_request_t tool_request = {0};
    bool pending_tool = false;

    while (true) {
        const int64_t now = esp_timer_get_time();
        const screen_state_t previous = state;
        if (now >= next_status_poll) {
            next_status_poll = now + STATUS_POLL_US;
            sugarat_voice_view(&state.voice);
        }
        servo_message_t reply;
        if (radio_ready && servo_radio_receive(&reply) &&
            reply.type == SERVO_STATE && reply.session == session &&
            reply.sequence >= last_received_sequence && reply.sequence <= sequence &&
            reply.position <= 1 && reply.speed >= SERVO_SPEED_MIN && reply.speed <= SERVO_SPEED_MAX && reply.moving <= 1 &&
            (reply.angle_limit_deg == SERVO_ANGLE_DEFAULT || reply.angle_limit_deg == SERVO_ANGLE_MAX) &&
            reply.angle_deg <= reply.angle_limit_deg) {
            const bool confirmed_command = pending.type && pending.sequence == reply.sequence;
            if (confirmed_command) {
                if (pending_tool) {
                    servo_tool_complete(&tool_request, reply.position == pending.position ? ESP_OK : ESP_FAIL,
                                        reply.moving != 0);
                    pending_tool = false;
                }
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
            state.receiver_channel = reply.channel;
            if (!previous.online || confirmed_command || state.position != previous.position || state.moving != previous.moving ||
                state.speed != previous.speed || state.angle_deg != previous.angle_deg ||
                state.angle_limit_deg != previous.angle_limit_deg || state.receiver_channel != previous.receiver_channel) {
                ESP_LOGI(TAG, "C3 confirmed target %c on channel %u, speed %u us per 10 ms, B angle %u/%u degrees%s",
                         'A' + state.position, (unsigned)state.receiver_channel, (unsigned)state.speed, (unsigned)state.angle_deg,
                         (unsigned)state.angle_limit_deg, state.moving ? ", moving" : "");
            }
        }
        state.online = now - last_reply < LINK_TIMEOUT_US;
        if (previous.online && !state.online) {
            ESP_LOGW(TAG, "C3 status timed out");
        }

        state.pending = pending.type != 0;
        screen_update(&state);
        const screen_action_t action = screen_poll();
        if (state.voice.phase == SUGARAT_VOICE_IN_CALL && previous.voice.phase != SUGARAT_VOICE_IN_CALL) {
            ESP_LOGI(TAG, "Voice UI in conversation: shown_ms=%lu", (unsigned long)(esp_timer_get_time() / 1000));
        }
        bool command_started = false;
        if (action.kind == SCREEN_START_CALL) {
            sugarat_voice_start_call();
        } else if (action.kind == SCREEN_END_CALL && state.voice.can_end) {
            sugarat_voice_end_call();
        } else if (state.online && !pending.type && create_command(action, state, &pending)) {
            command_started = true;
            ESP_LOGI(TAG, "Touch command %u", (unsigned)pending.type);
        }
        if (!pending.type && servo_tool_receive(&tool_request)) {
            if (esp_timer_get_time() >= tool_request.deadline_us) {
                servo_tool_complete(&tool_request, ESP_ERR_TIMEOUT, false);
            } else if (!radio_ready) {
                servo_tool_complete(&tool_request, ESP_ERR_INVALID_STATE, false);
            } else {
                pending.type = SERVO_SET_POSITION;
                pending.position = tool_request.position;
                pending_tool = true;
                command_started = true;
            }
        }
        if (command_started) {
            pending.session = session;
            pending.sequence = ++sequence;
            attempts = 0;
            next_retry = now;
        }

        if (radio_ready && pending.type && now >= next_retry) {
            const unsigned max_attempts = pending_tool ? MAX_VOICE_SENDS : MAX_SENDS;
            if (attempts < max_attempts && (!pending_tool || esp_timer_get_time() < tool_request.deadline_us)) {
                (void)servo_radio_send(pending);
                ++attempts;
                next_retry = now + RETRY_US;
            } else {
                ESP_LOGW(TAG, "C3 did not acknowledge %s command", pending_tool ? "voice" : "touch");
                if (pending_tool) {
                    servo_tool_complete(&tool_request, ESP_ERR_TIMEOUT, false);
                    pending_tool = false;
                }
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

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void app_main(void)
{
    // Board startup can initialize or write NVS, which requires an internal stack.
    const BaseType_t started = xTaskCreate(remote_task, "sugarat_ui", REMOTE_STACK_BYTES, NULL,
                                           tskIDLE_PRIORITY + 1, NULL);
    ESP_ERROR_CHECK(started == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
