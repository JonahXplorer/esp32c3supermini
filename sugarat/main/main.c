#include "agent_link_network.h"
#include "audio.h"
#include "board.h"
#include "manual_control.h"
#include "screen.h"
#include "servo_radio.h"
#include "servo_tool.h"
#include "voice.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { UI_TICK_MS = 15, VOICE_STATUS_US = 100000, WIFI_STATUS_US = 1000000, UI_STACK_BYTES = 8192 };

static const char *TAG = "sugarat";

static void ui_task(void *context)
{
    (void)context;
    ESP_ERROR_CHECK(sugarat_display_init());
    const bool touch_ready = sugarat_touch_init() == ESP_OK;
    ESP_ERROR_CHECK(screen_init(touch_ready));
    screen_state_t state = {
        .manual = {.speed = SERVO_SPEED_DEFAULT, .angle_deg = SERVO_ANGLE_DEFAULT,
                   .angle_limit_deg = SERVO_ANGLE_DEFAULT},
        .touch_ready = touch_ready,
        .settings = device_settings_load(),
    };
    screen_update(&state);
    (void)screen_poll();
    ESP_ERROR_CHECK(sugarat_display_light(state.settings.brightness));
    sugarat_speaker_set_volume(state.settings.volume);

    ESP_ERROR_CHECK_WITHOUT_ABORT(servo_tool_init());
    const bool wifi_managed = sugarat_voice_start(state.settings.voice_china) == ESP_OK;
    manual_control_init(wifi_managed);
    ESP_LOGI(TAG, "Jarvis home ready; ESP-NOW starts on demand");

    int64_t next_voice_poll = 0;
    int64_t next_wifi_poll = 0;
    while (true) {
        const int64_t now = esp_timer_get_time();
        if (now >= next_voice_poll) {
            next_voice_poll = now + VOICE_STATUS_US;
            const sugarat_voice_phase_t previous = state.voice.phase;
            sugarat_voice_view(&state.voice);
            if (state.voice.phase == SUGARAT_VOICE_IN_CALL && previous != state.voice.phase) {
                ESP_LOGI(TAG, "Voice UI in conversation: shown_ms=%lu", (unsigned long)(now / 1000));
            }
        }
        if (now >= next_wifi_poll) {
            next_wifi_poll = now + WIFI_STATUS_US;
            agent_link_wifi_info_t wifi = {0};
            agent_link_wifi_info(&wifi);
            state.wifi_connected = wifi.connected;
            state.wifi_setup_active = wifi.setup_active;
        }
        screen_update(&state);
        const screen_action_t action = screen_poll();
        const bool call_active = state.voice.can_end || state.voice.phase == SUGARAT_VOICE_OPENING;
        if (action.kind == SCREEN_START_CALL) {
            sugarat_voice_start_call();
        } else if (action.kind == SCREEN_END_CALL && state.voice.can_end) {
            sugarat_voice_end_call();
        } else if (action.kind == SCREEN_SETUP_WIFI && !call_active) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(agent_link_wifi_setup());
            next_wifi_poll = 0;
        } else if (action.kind == SCREEN_SET_VOLUME) {
            sugarat_speaker_set_volume(action.value);
            state.settings.volume = action.value;
            if (!action.preview) ESP_ERROR_CHECK_WITHOUT_ABORT(device_settings_save(state.settings));
        } else if (action.kind == SCREEN_SET_BRIGHTNESS) {
            const esp_err_t result = sugarat_display_set_brightness(action.value);
            if (result == ESP_OK) {
                state.settings.brightness = action.value;
                if (!action.preview) ESP_ERROR_CHECK_WITHOUT_ABORT(device_settings_save(state.settings));
            } else {
                ESP_ERROR_CHECK_WITHOUT_ABORT(result);
            }
        } else if (action.kind == SCREEN_SET_VOICE_SERVER && action.value != state.settings.voice_china) {
            sugarat_voice_view(&state.voice);
            if (!sugarat_voice_call_busy(&state.voice)) {
                device_settings_t settings = state.settings;
                settings.voice_china = action.value != 0;
                const esp_err_t result = device_settings_save(settings);
                if (result == ESP_OK) {
                    ESP_LOGI(TAG, "Restarting to use %s", sugarat_voice_server_host(settings.voice_china));
                    esp_restart();
                }
                state.voice_server_save_failed = true;
                ESP_ERROR_CHECK_WITHOUT_ABORT(result);
            }
        }
        manual_control_update(screen_manual_is_open(), call_active, action, &state.manual);
#if defined(SUGARAT_NATIVE_AMOLED_UI)
        screen_wait();
#else
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
#endif
    }
}

void app_main(void)
{
    // Startup may write NVS, which requires an internal task stack.
    const BaseType_t started = xTaskCreate(ui_task, "sugarat_ui", UI_STACK_BYTES, NULL,
                                           tskIDLE_PRIORITY + 1, NULL);
    ESP_ERROR_CHECK(started == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
