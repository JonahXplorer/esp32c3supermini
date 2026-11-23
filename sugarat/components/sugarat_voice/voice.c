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
#include "servo_tool.h"
#include "wake_word.h"

enum {
    UPLINK_FRAMES = 400,   // Up to eight seconds of speech after wake, while the grant opens.
    START_TIMEOUT_MS = 20000,
    CONVERSATION_CACHE_MS = 120000,
    CONVERSATION_RETRY_MS = 15000,
    TOOL_ATTACH_RETRY_MS = 30000,
};

typedef enum {
    VOICE_BOOT, VOICE_IDLE, VOICE_STARTING, VOICE_CONNECTING, VOICE_ACTIVE,
    VOICE_RECOVERING, VOICE_ENDING, VOICE_FAULT,
} voice_mode_t;

typedef struct {
    size_t count;
    int16_t pcm[SUGARAT_AUDIO_FRAME];
} audio_frame_t;

typedef struct {
    int agent_id;
    uint32_t valid_until;
    uint32_t retry_after;
    uint32_t tool_retry_after;
    bool tool_attached;
    agent_link_platform_conversation_t latest;
} conversation_cache_t;

static const char *TAG = "sugarat_voice";
static atomic_int mode = VOICE_BOOT;
static atomic_bool stop_call;
static atomic_uint error_until;
static atomic_uint requested_at;
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

static int voice_agent_id(void)
{
    agent_link_status_t link;
    agent_link_get_status(&link);
    if (link.phase != AGENT_LINK_PHASE_CONNECTED && link.phase != AGENT_LINK_PHASE_READY) {
        return 0;
    }
    agent_link_platform_agent_t agent;
    return agent_link_platform_bound_agent(&agent) == ESP_OK && agent.voice_supported ? agent.id : 0;
}

static void start_call(const char *source)
{
    if (atomic_load(&mode) != VOICE_IDLE || voice_agent_id() <= 0) {
        return;
    }
    int idle = VOICE_IDLE;
    if (!atomic_compare_exchange_strong(&mode, &idle, VOICE_STARTING)) {
        return;
    }
    const uint32_t start = now_ms();
    xQueueReset(uplink);
    atomic_store(&requested_at, start);
    atomic_store(&stop_call, false);
    atomic_store(&error_until, 0);
    ESP_LOGI(TAG, "Voice connect requested: source=%s start_ms=%lu", source, (unsigned long)start);
    atomic_store(&mode, VOICE_CONNECTING);
    xTaskNotifyGive(session_task);
}

void sugarat_voice_start_call(void)
{
    start_call("touch");
}

static void capture_audio(void *context)
{
    (void)context;
    audio_frame_t frame = {0};
    bool overflow_logged = false;
    bool wake_buffered = false;
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
        if (current != VOICE_IDLE && wake_buffered) {
            wake_word_reset();
            wake_buffered = false;
        }
        if (current == VOICE_IDLE) {
            overflow_logged = false;
            if (!voice_playback_echo_guard()) {
                wake_buffered = true;
                if (wake_word_feed(frame.pcm, frame.count)) {
                    wake_word_reset();
                    wake_buffered = false;
                    ESP_LOGI(TAG, "Jarvis detected");
                    start_call("wake_word");
                }
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

static esp_err_t lookup_latest(conversation_cache_t *cache, int agent_id, const char *source)
{
    const uint32_t started = now_ms();
    agent_link_platform_conversation_t latest = {0};
    size_t count = 0, total = 0;
    esp_err_t err = agent_link_platform_conversations(agent_id, 0, &latest, 1, &count, &total);
    if (err == ESP_OK && voice_agent_id() != agent_id) {
        err = ESP_ERR_INVALID_STATE;
    }
    const uint32_t finished = now_ms();
    const bool same_conversation = err == ESP_OK && cache->agent_id == agent_id && latest.id[0] &&
                                   strcmp(cache->latest.id, latest.id) == 0;
    const bool tool_attached = same_conversation && cache->tool_attached;
    const uint32_t tool_retry = same_conversation ? cache->tool_retry_after : 0;
    *cache = (conversation_cache_t){.agent_id = agent_id, .retry_after = finished + CONVERSATION_RETRY_MS};
    if (err == ESP_OK) {
        cache->latest = latest;
        cache->valid_until = finished + CONVERSATION_CACHE_MS;
        cache->tool_attached = tool_attached;
        cache->tool_retry_after = tool_retry;
    }
    ESP_LOGI(TAG, "Voice stage conversation lookup: source=%s ms=%lu result=%s found=%u", source,
             (unsigned long)(finished - started), esp_err_to_name(err), (unsigned)count);
    return err;
}

static esp_err_t create_voice_conversation(conversation_cache_t *cache)
{
    agent_link_platform_conversation_t conversation = {0};
    const uint32_t started = now_ms();
    const esp_err_t err = agent_link_platform_create_conversation(cache->agent_id, conversation.id, sizeof(conversation.id));
    ESP_LOGI(TAG, "Voice stage conversation create: ms=%lu result=%s",
             (unsigned long)(now_ms() - started), esp_err_to_name(err));
    if (err != ESP_OK) return err;
    if (!conversation.id[0]) return ESP_ERR_INVALID_RESPONSE;
    cache->latest = conversation;
    cache->valid_until = now_ms() + CONVERSATION_CACHE_MS;
    return ESP_OK;
}

static void attach_session_tool(conversation_cache_t *cache)
{
    if (cache->tool_attached && !agent_link_platform_tool_ready()) cache->tool_attached = false;
    if (!cache->tool_attached && cache->latest.id[0] && !before_deadline(cache->tool_retry_after)) {
        esp_err_t err = servo_tool_attach(cache->agent_id, cache->latest.id);
        if (err == ESP_ERR_NOT_ALLOWED && !atomic_load(&stop_call)) {
            // A different client owns this conversation's active tool connection.
            err = create_voice_conversation(cache);
            if (err == ESP_OK) err = servo_tool_attach(cache->agent_id, cache->latest.id);
        }
        cache->tool_attached = err == ESP_OK;
        cache->tool_retry_after = now_ms() + TOOL_ATTACH_RETRY_MS;
    }
}

static void prefetch_conversation(conversation_cache_t *cache)
{
    if (atomic_load(&mode) != VOICE_IDLE) {
        return;
    }
    const int agent_id = voice_agent_id();
    if (cache->agent_id != agent_id) {
        agent_link_platform_detach_tool();
        *cache = (conversation_cache_t){.agent_id = agent_id};
    }
    if (agent_id > 0 && !before_deadline(cache->valid_until) && !before_deadline(cache->retry_after)) {
        (void)lookup_latest(cache, agent_id, "background");
    }
    if (agent_id > 0 && before_deadline(cache->valid_until)) {
        attach_session_tool(cache);
    }
}

static esp_err_t request_session(conversation_cache_t *cache)
{
    const int agent_id = voice_agent_id();
    if (agent_id <= 0 || atomic_load(&stop_call)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (cache->agent_id == agent_id && before_deadline(cache->valid_until) && cache->latest.id[0]) {
        ESP_LOGI(TAG, "Voice stage conversation lookup: source=cache ms=0 result=ESP_OK found=1");
    } else {
        err = lookup_latest(cache, agent_id, "on_demand");
        if (err != ESP_OK || atomic_load(&stop_call)) {
            return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
        }
    }
    if (!cache->latest.id[0]) {
        err = create_voice_conversation(cache);
        if (err != ESP_OK) return err;
    }
    if (atomic_load(&stop_call) || voice_agent_id() != agent_id) {
        return ESP_ERR_INVALID_STATE;
    }
    attach_session_tool(cache);
    if (atomic_load(&stop_call) || voice_agent_id() != agent_id) {
        return ESP_ERR_INVALID_STATE;
    }
    return agent_link_request_realtime_session(agent_id, cache->latest.id);
}

static void close_stream(agent_stream_handle_t *stream)
{
    servo_tool_accept_transcripts(false);
    voice_playback_accept(false);
    if (*stream) {
        (void)agent_link_stream_close(*stream, false);
        *stream = NULL;
    }
}

static esp_err_t run_session(conversation_cache_t *cache)
{
    ESP_LOGI(TAG, "Voice stage worker: ms=%lu", (unsigned long)(now_ms() - atomic_load(&requested_at)));
    esp_err_t err = request_session(cache);
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
            if (cache->tool_attached) {
                const uint32_t refresh_started = now_ms();
                const esp_err_t refresh = agent_link_platform_refresh_tool(cache->agent_id, cache->latest.id);
                ESP_LOGI(TAG, "Voice stage tool hot-update: ms=%lu result=%s",
                         (unsigned long)(now_ms() - refresh_started), esp_err_to_name(refresh));
                if (atomic_load(&stop_call)) break;
                if (agent_link_state() != AGENT_STATE_READY) continue;
            }
            const uint32_t started = now_ms();
            err = agent_link_stream_open(AGENT_STREAM_VOICE, NULL, &stream);
            if (err != ESP_OK) {
                break;
            }
            if (opened) {
                ESP_LOGI(TAG, "Realtime voice resumed");
            } else {
                ESP_LOGI(TAG, "Realtime voice ready: total_ms=%lu stream_open_ms=%lu",
                         (unsigned long)(now_ms() - atomic_load(&requested_at)), (unsigned long)(now_ms() - started));
            }
            opened = true;
            servo_tool_accept_transcripts(true);
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
    if (!opened && !atomic_load(&stop_call)) {
        cache->valid_until = 0;
    }
    return err == ESP_OK && !opened && !atomic_load(&stop_call) ? ESP_FAIL : err;
}

static void handle_calls(void *context)
{
    (void)context;
    conversation_cache_t cache = {0};
    while (true) {
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) {
            prefetch_conversation(&cache);
            continue;
        }
        const esp_err_t err = run_session(&cache);
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
        xTaskCreate(handle_calls, "jarvis_session", 8192, NULL, 4, &session_task) == pdPASS &&
        xTaskCreate(capture_audio, "jarvis_mic", 6144, NULL, 5, &capture_task) == pdPASS) {
        return true;
    }
    stop_audio_workers();
    return false;
}

esp_err_t sugarat_voice_start(void)
{
    static const agent_platform_t platform = {
        .base_url = "https://bot.sugarat.ai", .product_id = 13, .chip_type = "esp32-s3",
    };
    static const agent_output_cb_t output = {
        .on_audio_out = voice_playback_queue, .on_audio_end = voice_playback_end,
        .on_audio_reset = voice_playback_reset,
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
    } else if (current == VOICE_STARTING || current == VOICE_CONNECTING) {
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
