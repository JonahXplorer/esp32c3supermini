#include "voice.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "agent_link.h"
#include "agent_link_platform.h"
#include "agent_link_stream.h"
#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "playback.h"
#include "wake_word.h"

enum {
    UPLINK_FRAMES = 400,   // Up to eight seconds of speech after wake, while the grant opens.
    START_TIMEOUT_MS = 20000,
};

typedef enum { VOICE_BOOT, VOICE_IDLE, VOICE_CONNECTING, VOICE_ACTIVE, VOICE_RECOVERING, VOICE_ENDING, VOICE_FAULT } voice_mode_t;

typedef struct {
    size_t count;
    int16_t pcm[SUGARAT_AUDIO_FRAME];
} audio_frame_t;

static const char *TAG = "sugarat_voice";
static atomic_int mode = VOICE_BOOT;
static atomic_bool stop_call;
static atomic_uint error_until;
static QueueHandle_t uplink;
static StaticQueue_t uplink_queue;
static uint8_t *uplink_storage;
static TaskHandle_t session_task;
static TaskHandle_t capture_task;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool before_deadline(uint32_t deadline)
{
    return deadline != 0 && (int32_t)(deadline - now_ms()) > 0;
}

static void capture_audio(void *context)
{
    (void)context;
    audio_frame_t frame = {0};
    bool overflow_logged = false;
    unsigned warmup = 10;
    while (true) {
        const voice_mode_t before_read = atomic_load(&mode);
        if (before_read == VOICE_BOOT || before_read == VOICE_FAULT) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (sugarat_mic_read(frame.pcm, &frame.count) != ESP_OK || frame.count == 0) {
            vTaskDelay(1);
            continue;
        }
        if (warmup) {
            --warmup;
            continue;
        }
        const voice_mode_t current = atomic_load(&mode);
        if (current == VOICE_IDLE) {
            if (!voice_playback_echo_guard() && wake_word_feed(frame.pcm, frame.count)) {
                wake_word_reset();
                xQueueReset(uplink);
                overflow_logged = false;
                atomic_store(&stop_call, false);
                atomic_store(&mode, VOICE_CONNECTING);
                xTaskNotifyGive(session_task);
                ESP_LOGI(TAG, "Jarvis detected");
            }
        } else if (current == VOICE_CONNECTING || current == VOICE_ACTIVE) {
            if (voice_playback_echo_guard()) {
                memset(frame.pcm, 0, frame.count * sizeof(frame.pcm[0]));
            }
            if (xQueueSend(uplink, &frame, 0) != pdTRUE && !overflow_logged) {
                ESP_LOGW(TAG, "Microphone buffer full while waiting for voice transport");
                overflow_logged = true;
            }
        }
    }
}

static esp_err_t request_session(void)
{
    agent_link_platform_agent_t agent;
    esp_err_t err = agent_link_platform_bound_agent(&agent);
    if (err != ESP_OK || !agent.voice_supported || agent.id <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    agent_link_platform_conversation_t latest = {0};
    size_t count = 0, total = 0;
    err = agent_link_platform_conversations(agent.id, 0, &latest, 1, &count, &total);
    if (err != ESP_OK || atomic_load(&stop_call)) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    if (count == 0) {
        err = agent_link_platform_create_conversation(agent.id, latest.id, sizeof(latest.id));
    }
    if (err != ESP_OK || atomic_load(&stop_call) || latest.id[0] == '\0') {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    return agent_link_request_realtime_session(agent.id, latest.id);
}

static void close_stream(agent_stream_handle_t *stream)
{
    voice_playback_accept(false);
    if (*stream) {
        (void)agent_link_stream_close(*stream, false);
        *stream = NULL;
    }
}

static esp_err_t run_session(void)
{
    esp_err_t err = request_session();
    if (err != ESP_OK) {
        return err;
    }
    const uint32_t opening_deadline = now_ms() + START_TIMEOUT_MS;
    agent_stream_handle_t stream = NULL;
    bool opened = false;
    audio_frame_t frame;

    while (!atomic_load(&stop_call) && agent_link_realtime_running()) {
        if (agent_link_state() != AGENT_STATE_READY) {
            if (stream) {
                close_stream(&stream);
                atomic_store(&mode, VOICE_RECOVERING);
                xQueueReset(uplink);
            }
            if (!opened && !before_deadline(opening_deadline)) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!stream) {
            err = agent_link_stream_open(AGENT_STREAM_VOICE, NULL, &stream);
            if (err != ESP_OK) {
                break;
            }
            ESP_LOGI(TAG, "%s", opened ? "Realtime voice resumed" : "Realtime voice ready");
            opened = true;
            voice_playback_accept(true);
            atomic_store(&mode, VOICE_ACTIVE);
        }
        if (xQueuePeek(uplink, &frame, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }
        const esp_err_t sent = agent_link_stream_write(stream, frame.pcm, frame.count * sizeof(frame.pcm[0]));
        if (sent == ESP_OK) {
            xQueueReceive(uplink, &frame, 0);
        } else if (sent == ESP_ERR_NO_MEM || agent_link_state() != AGENT_STATE_READY) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            err = sent;
            break;
        }
    }
    atomic_store(&mode, VOICE_ENDING);
    close_stream(&stream);
    (void)agent_link_end_realtime();
    while (agent_link_realtime_running()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return err == ESP_OK && !opened && !atomic_load(&stop_call) ? ESP_FAIL : err;
}

static void handle_wake(void *context)
{
    (void)context;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const esp_err_t err = run_session();
        if (err != ESP_OK && !atomic_load(&stop_call)) {
            atomic_store(&error_until, now_ms() + 3000);
            ESP_LOGW(TAG, "Voice session ended: %s", esp_err_to_name(err));
        } else {
            atomic_store(&error_until, 0);
        }
        xQueueReset(uplink);
        atomic_store(&mode, VOICE_IDLE);
    }
}

static void stop_audio_workers(void)
{
    if (capture_task) {
        vTaskDelete(capture_task);
        capture_task = NULL;
    }
    if (session_task) {
        vTaskDelete(session_task);
        session_task = NULL;
    }
    voice_playback_stop();
    wake_word_deinit();
    sugarat_audio_deinit();
    if (uplink) {
        vQueueDelete(uplink);
        uplink = NULL;
    }
    heap_caps_free(uplink_storage);
    uplink_storage = NULL;
}

static bool start_audio_workers(void)
{
    uplink_storage = heap_caps_malloc(UPLINK_FRAMES * sizeof(audio_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (uplink_storage) {
        uplink = xQueueCreateStatic(UPLINK_FRAMES, sizeof(audio_frame_t), uplink_storage, &uplink_queue);
    }
    if (uplink && sugarat_audio_init() == ESP_OK && wake_word_init() == ESP_OK && voice_playback_start() == ESP_OK &&
        xTaskCreate(handle_wake, "jarvis_session", 8192, NULL, 4, &session_task) == pdPASS &&
        xTaskCreate(capture_audio, "jarvis_mic", 6144, NULL, 5, &capture_task) == pdPASS) {
        return true;
    }
    stop_audio_workers();
    return false;
}

esp_err_t sugarat_voice_start(void)
{
    static const agent_platform_t platform = {
        .base_url = "https://bot.deotaland.cn", .product_id = 13, .chip_type = "esp32-s3",
    };
    static const agent_output_cb_t output = {
        .on_audio_out = voice_playback_queue, .on_audio_reset = voice_playback_reset,
    };
    const bool audio_ready = start_audio_workers();
    const agent_link_config_t config = {
        .device_name = "Sugarat-Jarvis", .model = "SDGOODS-ESP32S3", .manufacturer = "Sugarat",
        .transport = AGENT_TRANSPORT_WIFI, .platform = &platform,
        .caps = audio_ready ? AGENT_CAP_MIC | AGENT_CAP_SPEAKER : 0,
        .output = &output,
    };
    esp_err_t err = agent_link_init(&config);
    if (err == ESP_OK) {
        err = agent_link_start();
    }
    if (err != ESP_OK && audio_ready) {
        stop_audio_workers();
    }
    atomic_store(&mode, err == ESP_OK && audio_ready ? VOICE_IDLE : VOICE_FAULT);
    if (!audio_ready) {
        ESP_LOGE(TAG, "Voice audio or wake initialization failed");
    }
    return err;
}

void sugarat_voice_view(sugarat_voice_view_t *view)
{
    const voice_mode_t current = atomic_load(&mode);
    agent_link_status_t link;
    agent_link_get_status(&link);
    *view = (sugarat_voice_view_t){
        .can_end = current == VOICE_CONNECTING || current == VOICE_ACTIVE || current == VOICE_RECOVERING,
    };
    if (link.phase == AGENT_LINK_PHASE_PAIRING && link.code[0]) {
        view->phase = SUGARAT_VOICE_ACTIVATE;
        snprintf(view->activation_code, sizeof(view->activation_code), "%.6s", link.code);
    } else if (current == VOICE_ACTIVE) {
        view->phase = SUGARAT_VOICE_IN_CALL;
    } else if (current == VOICE_CONNECTING) {
        view->phase = SUGARAT_VOICE_OPENING;
    } else if (current == VOICE_RECOVERING) {
        view->phase = SUGARAT_VOICE_RECONNECTING;
    } else if (current == VOICE_ENDING) {
        view->phase = SUGARAT_VOICE_ENDING;
    } else if (current == VOICE_FAULT) {
        view->phase = SUGARAT_VOICE_ERROR;
    } else if (link.phase == AGENT_LINK_PHASE_SETUP) {
        view->phase = SUGARAT_VOICE_SETUP;
    } else if (link.phase == AGENT_LINK_PHASE_CONNECTING) {
        view->phase = SUGARAT_VOICE_JOINING;
    } else if (before_deadline(atomic_load(&error_until))) {
        view->phase = SUGARAT_VOICE_RETRY;
    } else if (current == VOICE_IDLE) {
        agent_link_platform_agent_t agent;
        view->phase = agent_link_platform_bound_agent(&agent) == ESP_OK && agent.voice_supported
                      ? SUGARAT_VOICE_READY : SUGARAT_VOICE_BIND_AGENT;
    }
}

void sugarat_voice_end_call(void)
{
    atomic_store(&stop_call, true);
}
