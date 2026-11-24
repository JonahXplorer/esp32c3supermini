#include "voice.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "agent_link.h"
#include "agent_link_platform.h"
#include "agent_link_stream.h"
#include "audio.h"
#include "audio_meter.h"
#include "conversations_internal.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "playback.h"
#include "servo_tool.h"
#include "turn_timing.h"
#include "voice_trace.h"
#include "wake_word.h"

enum {
    UPLINK_FRAMES = 400,   // Up to eight seconds of speech after wake, while the grant opens.
    WAKE_PREROLL_SAMPLES = SUGARAT_SAMPLE_RATE,
    START_TIMEOUT_MS = 20000,
    RESIDENT_RETRY_MS = 3000,
    RESIDENT_RETRY_MAX_MS = 30000,
    CONVERSATION_CACHE_MS = 120000,
    CONVERSATION_RETRY_MS = 15000,
    TOOL_ATTACH_RETRY_MS = 30000,
    SESSION_STACK_BYTES = 16 * 1024,
    CONVERSATIONS_STACK_BYTES = 12 * 1024,
    SESSION_STACK_CHECK_MS = 15000,
    CONVERSATION_CLOSE_TIMEOUT_MS = 15000,
};

typedef enum {
    VOICE_BOOT, VOICE_IDLE, VOICE_STARTING, VOICE_CONNECTING, VOICE_ACTIVE,
    VOICE_RECOVERING, VOICE_ENDING, VOICE_MANAGING, VOICE_FAULT,
} voice_mode_t;

typedef struct {
    size_t count;
    int16_t pcm[SUGARAT_AUDIO_FRAME];
} audio_frame_t;

typedef struct {
    int16_t pcm[WAKE_PREROLL_SAMPLES];
    size_t write_at;
    size_t count;
} wake_preroll_t;

typedef struct {
    int agent_id;
    uint32_t valid_until;
    uint32_t retry_after;
    agent_link_platform_conversation_t latest;
    bool chosen;
    bool suppress_create;
} conversation_cache_t;

typedef struct {
    int agent_id;
    uint32_t call_started;
    bool active;
    char session_id[sizeof(((agent_link_platform_conversation_t *)0)->id)];
} tool_job_t;

static const char *TAG = "sugarat_voice";
static atomic_int mode = VOICE_BOOT;
static atomic_bool stop_call;
static atomic_uint error_until;
static atomic_uint requested_at;
static atomic_uint mic_level;
static atomic_bool conversation_changed;
static QueueHandle_t uplink;
static StaticQueue_t uplink_queue;
static EXT_RAM_BSS_ATTR audio_frame_t uplink_storage[UPLINK_FRAMES];
static EXT_RAM_BSS_ATTR wake_preroll_t wake_history;
static EXT_RAM_BSS_ATTR _Alignas(16) StackType_t session_stack[SESSION_STACK_BYTES];
static EXT_RAM_BSS_ATTR _Alignas(16) StackType_t capture_stack[6144];
static EXT_RAM_BSS_ATTR _Alignas(16) StackType_t tool_stack[8192];
static EXT_RAM_BSS_ATTR _Alignas(16) StackType_t prefetch_stack[CONVERSATIONS_STACK_BYTES];
static StaticTask_t session_tcb, capture_tcb, tool_tcb, prefetch_tcb;
static TaskHandle_t session_task;
static TaskHandle_t capture_task;
static TaskHandle_t tool_task;
static TaskHandle_t prefetch_task;
static StaticSemaphore_t conversation_mutex;
static SemaphoreHandle_t conversation_lookup;
static portMUX_TYPE conversation_lock = portMUX_INITIALIZER_UNLOCKED;
static conversation_cache_t selected_conversation;
static StaticQueue_t tool_queue;
static uint8_t tool_storage[sizeof(tool_job_t)];
static QueueHandle_t tool_jobs;
static portMUX_TYPE tool_reservation_lock = portMUX_INITIALIZER_UNLOCKED;
static tool_job_t reserved_tool_route;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool before_deadline(uint32_t deadline)
{
    return deadline != 0 && (int32_t)(deadline - now_ms()) > 0;
}

static conversation_cache_t load_conversation(void)
{
    portENTER_CRITICAL(&conversation_lock);
    const conversation_cache_t cache = selected_conversation;
    portEXIT_CRITICAL(&conversation_lock);
    return cache;
}

static void save_conversation(const conversation_cache_t *cache)
{
    portENTER_CRITICAL(&conversation_lock);
    selected_conversation = *cache;
    portEXIT_CRITICAL(&conversation_lock);
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

static bool voice_connection_ready(bool running)
{
    // The Wi-Fi transport publishes READY only after WSS session.ready.
    return running && agent_link_state() == AGENT_STATE_READY;
}

static void queue_preroll(const wake_preroll_t *history)
{
    size_t offset = (history->write_at + WAKE_PREROLL_SAMPLES - history->count) % WAKE_PREROLL_SAMPLES;
    size_t remaining = history->count;
    while (remaining) {
        audio_frame_t frame = {.count = remaining < SUGARAT_AUDIO_FRAME ? remaining : SUGARAT_AUDIO_FRAME};
        const size_t until_wrap = WAKE_PREROLL_SAMPLES - offset;
        const size_t first = frame.count < until_wrap ? frame.count : until_wrap;
        memcpy(frame.pcm, history->pcm + offset, first * sizeof(frame.pcm[0]));
        memcpy(frame.pcm + first, history->pcm, (frame.count - first) * sizeof(frame.pcm[0]));
        if (xQueueSend(uplink, &frame, 0) != pdTRUE) break;
        offset = (offset + frame.count) % WAKE_PREROLL_SAMPLES;
        remaining -= frame.count;
    }
    ESP_LOGI(TAG, "Voice preroll: wake_word=jarvis queued_ms=%u",
             (unsigned)((history->count - remaining) * 1000 / SUGARAT_SAMPLE_RATE));
}

static void start_call(const char *source, const wake_preroll_t *history)
{
    if (atomic_load(&mode) != VOICE_IDLE) return;
    if (!voice_connection_ready(agent_link_realtime_running()) || voice_agent_id() <= 0) {
        if (history) ESP_LOGI(TAG, "Jarvis detected while the voice connection was not ready");
        return;
    }
    int idle = VOICE_IDLE;
    if (!atomic_compare_exchange_strong(&mode, &idle, VOICE_STARTING)) {
        return;
    }
    const int64_t start_us = esp_timer_get_time();
    const uint32_t start = (uint32_t)(start_us / 1000);
    voice_trace_begin(source, start_us);
    xQueueReset(uplink);
    if (history) {
        ESP_LOGI(TAG, "Jarvis detected");
        queue_preroll(history);
    }
    atomic_store(&requested_at, start);
    atomic_store(&stop_call, false);
    atomic_store(&error_until, 0);
    voice_timing_set_active(true);
    ESP_LOGI(TAG, "Voice connect requested: source=%s start_ms=%lu", source, (unsigned long)start);
    atomic_store(&mode, VOICE_CONNECTING);
    xTaskNotifyGive(session_task);
}

void sugarat_voice_start_call(void)
{
    start_call("touch", NULL);
}

static void save_wake_audio(wake_preroll_t *history, const audio_frame_t *frame)
{
    const size_t until_wrap = WAKE_PREROLL_SAMPLES - history->write_at;
    const size_t first = frame->count < until_wrap ? frame->count : until_wrap;
    memcpy(history->pcm + history->write_at, frame->pcm, first * sizeof(frame->pcm[0]));
    memcpy(history->pcm, frame->pcm + first, (frame->count - first) * sizeof(frame->pcm[0]));
    history->write_at = (history->write_at + frame->count) % WAKE_PREROLL_SAMPLES;
    const size_t total = history->count + frame->count;
    history->count = total < WAKE_PREROLL_SAMPLES ? total : WAKE_PREROLL_SAMPLES;
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
        const bool echo_before_read = (before_read == VOICE_CONNECTING || before_read == VOICE_ACTIVE) &&
                                      voice_playback_echo_guard();
        if (sugarat_mic_read(frame.pcm, &frame.count) != ESP_OK || frame.count == 0) {
            vTaskDelay(1);
            continue;
        }
        if (warmup) {
            --warmup;
            continue;
        }
        const voice_mode_t current = atomic_load(&mode);
        const bool capturing = current == VOICE_CONNECTING || current == VOICE_ACTIVE;
        const unsigned level = capturing ? voice_audio_level(frame.pcm, frame.count) : 0;
        atomic_store(&mic_level, current == VOICE_ACTIVE ? level : 0);
        if (current != VOICE_IDLE && wake_buffered) {
            wake_word_reset();
            wake_buffered = false;
            wake_history.count = 0;
        }
        if (current == VOICE_IDLE) {
            overflow_logged = false;
            if (!voice_playback_echo_guard()) {
                wake_buffered = true;
                save_wake_audio(&wake_history, &frame);
                if (wake_word_feed(frame.pcm, frame.count)) {
                    wake_word_reset();
                    start_call("wake_word", &wake_history);
                }
            } else {
                wake_history.count = 0;
            }
        } else if (capturing && !atomic_load(&stop_call)) {
            const bool echo = voice_playback_echo_guard();
            voice_timing_mic_level(level, echo_before_read || echo);
            if (echo) {
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
    if (cache->agent_id != agent_id) {
        *cache = (conversation_cache_t){.agent_id = agent_id};
    }
    cache->retry_after = finished + CONVERSATION_RETRY_MS;
    if (err == ESP_OK) {
        cache->latest = latest;
        cache->valid_until = finished + CONVERSATION_CACHE_MS;
    }
    ESP_LOGI(TAG, "Voice stage conversation lookup: source=%s ms=%lu result=%s found=%u", source,
             (unsigned long)(finished - started), esp_err_to_name(err), (unsigned)count);
    return err;
}

static esp_err_t create_voice_conversation(conversation_cache_t *cache)
{
    agent_link_platform_conversation_t conversation = {0};
    const uint32_t started = now_ms();
    esp_err_t err = agent_link_platform_create_conversation(cache->agent_id, conversation.id, sizeof(conversation.id));
    if (err == ESP_OK && !conversation.id[0]) err = ESP_ERR_INVALID_RESPONSE;
    ESP_LOGI(TAG, "Voice stage conversation create: ms=%lu result=%s",
             (unsigned long)(now_ms() - started), esp_err_to_name(err));
    if (err != ESP_OK) return err;
    cache->latest = conversation;
    cache->valid_until = now_ms() + CONVERSATION_CACHE_MS;
    return ESP_OK;
}

esp_err_t voice_change_conversation(int agent_id, conversations_action_t action, const char *id)
{
    int idle = VOICE_IDLE;
    if (!atomic_compare_exchange_strong(&mode, &idle, VOICE_MANAGING)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    xSemaphoreTake(conversation_lookup, portMAX_DELAY);
    conversation_cache_t cache = load_conversation();
    if (cache.agent_id != agent_id) cache = (conversation_cache_t){.agent_id = agent_id};
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool restart = false;
    if (agent_id == voice_agent_id()) {
        const bool same = cache.latest.id[0] && strcmp(cache.latest.id, id) == 0;
        if (action == CONVERSATIONS_CREATE) {
            result = create_voice_conversation(&cache);
        } else if (action == CONVERSATIONS_SELECT) {
            if (!same) {
                cache.latest = (agent_link_platform_conversation_t){0};
                strcpy(cache.latest.id, id);
            }
            result = ESP_OK;
        } else if (action == CONVERSATIONS_DELETE) {
            result = ESP_OK;
            if (same) {
                restart = true;
                (void)agent_link_end_realtime();
                result = agent_link_platform_wait_realtime_idle(CONVERSATION_CLOSE_TIMEOUT_MS);
            }
            if (result == ESP_OK) result = agent_link_platform_delete_conversation(agent_id, id);
            if (result == ESP_OK && (same || !cache.latest.id[0])) {
                // Discover the next existing route. A deliberate empty list stays empty.
                cache = (conversation_cache_t){.agent_id = agent_id, .suppress_create = true};
                save_conversation(&cache);
                restart = true;
            }
        }
        if (result == ESP_OK && (action == CONVERSATIONS_CREATE || action == CONVERSATIONS_SELECT)) {
            cache.chosen = true;
            cache.suppress_create = false;
            save_conversation(&cache);
            if (action == CONVERSATIONS_CREATE || !same) {
                (void)agent_link_end_realtime();
                restart = true;
            }
        }
    }
    xSemaphoreGive(conversation_lookup);

    if (restart) {
        atomic_store(&error_until, 0);
        atomic_store(&conversation_changed, true);
    }
    atomic_store(&mode, VOICE_IDLE);
    xTaskNotifyGive(session_task);
    return result;
}

static void queue_tools(const conversation_cache_t *cache, bool active)
{
    if (!tool_task) return;
    tool_job_t job = {.agent_id = cache->agent_id, .call_started = atomic_load(&requested_at), .active = active};
    memcpy(job.session_id, cache->latest.id, sizeof(job.session_id));
    (void)xQueueOverwrite(tool_jobs, &job);
}

static bool tool_route_reserved(const conversation_cache_t *cache)
{
    portENTER_CRITICAL(&tool_reservation_lock);
    const bool reserved = reserved_tool_route.agent_id == cache->agent_id &&
                         strcmp(reserved_tool_route.session_id, cache->latest.id) == 0;
    portEXIT_CRITICAL(&tool_reservation_lock);
    return reserved;
}

static void prefetch_conversation(void *context)
{
    (void)context;
    while (true) {
        if (conversations_service(pdMS_TO_TICKS(1000))) continue;
        if (!agent_link_realtime_running() && atomic_load(&mode) == VOICE_IDLE &&
            xSemaphoreTake(conversation_lookup, 0) == pdTRUE) {
            const int agent_id = voice_agent_id();
            conversation_cache_t cache = load_conversation();
            if (cache.agent_id != agent_id) cache = (conversation_cache_t){.agent_id = agent_id};
            if (agent_id > 0 && !cache.chosen && !before_deadline(cache.valid_until) && !before_deadline(cache.retry_after) &&
                lookup_latest(&cache, agent_id, "background") == ESP_OK && !cache.latest.id[0] &&
                !cache.suppress_create && atomic_load(&mode) == VOICE_IDLE && create_voice_conversation(&cache) != ESP_OK) {
                cache.valid_until = 0;
            }
            if (agent_id == voice_agent_id()) save_conversation(&cache);
            xSemaphoreGive(conversation_lookup);
        }
    }
}

static esp_err_t request_session(conversation_cache_t *cache, int agent_id)
{
    xSemaphoreTake(conversation_lookup, portMAX_DELAY);
    if (atomic_load(&mode) == VOICE_MANAGING || voice_agent_id() != agent_id) {
        xSemaphoreGive(conversation_lookup);
        return ESP_ERR_INVALID_STATE;
    }

    *cache = load_conversation();
    esp_err_t err = ESP_OK;
    if (cache->agent_id == agent_id && cache->latest.id[0] && (cache->chosen || !tool_route_reserved(cache))) {
        ESP_LOGI(TAG, "Voice stage conversation lookup: source=cache ms=0 result=ESP_OK found=1");
    } else {
        if (cache->agent_id != agent_id || (!cache->latest.id[0] && !before_deadline(cache->valid_until))) {
            err = lookup_latest(cache, agent_id, "on_demand");
        }
        if (err == ESP_OK && ((!cache->latest.id[0] && !cache->suppress_create) ||
                              (cache->latest.id[0] && tool_route_reserved(cache)))) {
            err = create_voice_conversation(cache);
        }
        if (agent_id == voice_agent_id()) save_conversation(cache);
    }
    if (err == ESP_OK) {
        err = cache->latest.id[0] ? agent_link_request_realtime_session(agent_id, cache->latest.id)
                                  : ESP_ERR_NOT_FOUND;
    }
    xSemaphoreGive(conversation_lookup);
    return err;
}

static void close_stream(agent_stream_handle_t *stream, const char *reset_reason)
{
    voice_timing_set_active(false);
    if (*stream) {
        (void)agent_link_stream_close(*stream, true);
        *stream = NULL;
    }
    servo_tool_accept_transcripts(false);
    voice_playback_accept(false, reset_reason);
}

static void handle_tools(void *context)
{
    (void)context;
    tool_job_t route = {0};
    uint32_t retry_after = 0;
    while (true) {
        tool_job_t job;
        if (xQueueReceive(tool_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        const voice_mode_t current = atomic_load(&mode);
        if (job.active) {
            if (current != VOICE_ACTIVE || atomic_load(&stop_call) ||
                job.call_started != atomic_load(&requested_at) || agent_link_state() != AGENT_STATE_READY) continue;
        } else if (current != VOICE_IDLE) {
            continue;
        }
        if (job.agent_id != route.agent_id || strcmp(job.session_id, route.session_id) != 0) {
            if (route.session_id[0]) agent_link_platform_detach_tool();
            route = job;
            retry_after = 0;
        }
        if (!job.session_id[0]) continue;

        const bool attached = agent_link_platform_tool_ready();
        if ((attached && !job.active) || (!attached && before_deadline(retry_after))) continue;
        const uint32_t started = now_ms();
        const esp_err_t err = attached ? agent_link_platform_refresh_tool(job.agent_id, job.session_id)
                                       : servo_tool_attach(job.agent_id, job.session_id);
        if (!attached) {
            retry_after = now_ms() + TOOL_ATTACH_RETRY_MS;
            portENTER_CRITICAL(&tool_reservation_lock);
            reserved_tool_route = err == ESP_ERR_NOT_ALLOWED ? job : (tool_job_t){0};
            portEXIT_CRITICAL(&tool_reservation_lock);
        }
        ESP_LOGI(TAG, "Voice stage tool %s: background ms=%lu result=%s", attached ? "hot-update" : "mount",
                 (unsigned long)(now_ms() - started), esp_err_to_name(err));
    }
}

static void finish_call(agent_stream_handle_t *stream, esp_err_t err)
{
    atomic_store(&mode, VOICE_ENDING);
    close_stream(stream, atomic_load(&stop_call) ? "user_hangup" : "call_ended");
    voice_trace_end();
    if (err != ESP_OK && !atomic_load(&stop_call)) {
        atomic_store(&error_until, now_ms() + 3000);
        ESP_LOGW(TAG, "Voice session ended: %s", esp_err_to_name(err));
    } else {
        atomic_store(&error_until, 0);
    }
    xQueueReset(uplink);
    atomic_store(&mode, VOICE_IDLE);
}

static void handle_calls(void *context)
{
    (void)context;
    conversation_cache_t resident = {0};
    agent_stream_handle_t stream = NULL;
    uint32_t retry_at = 0;
    uint32_t retry_ms = RESIDENT_RETRY_MS;
    uint32_t next_stack_check = 0;
    UBaseType_t reported_stack_free = SESSION_STACK_BYTES;
    bool was_ready = false;
    audio_frame_t frame;

    while (true) {
        if (!before_deadline(next_stack_check)) {
            const UBaseType_t minimum_free = uxTaskGetStackHighWaterMark(NULL);
            if (minimum_free < reported_stack_free) {
                reported_stack_free = minimum_free;
                ESP_LOGI(TAG, "jarvis_session stack: min_free=%uB total=%uB (PSRAM)",
                         (unsigned)minimum_free, (unsigned)sizeof(session_stack));
            }
            next_stack_check = now_ms() + SESSION_STACK_CHECK_MS;
        }
        voice_mode_t current = atomic_load(&mode);
        if (current == VOICE_MANAGING) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            continue;
        }
        if (atomic_exchange(&conversation_changed, false)) {
            retry_at = 0;
            retry_ms = RESIDENT_RETRY_MS;
        }
        bool talking = current == VOICE_CONNECTING || current == VOICE_ACTIVE || current == VOICE_RECOVERING;
        const int agent_id = voice_agent_id();
        const bool running = agent_link_realtime_running();
        const bool ready = voice_connection_ready(running);
        if (ready && !was_ready) {
            retry_ms = RESIDENT_RETRY_MS;
            ESP_LOGI(TAG, "Resident voice connected: microphone=%s", stream ? "on" : "off");
        }
        was_ready = ready;

        if (talking && (atomic_load(&stop_call) || agent_id <= 0 ||
                        (!running && current != VOICE_CONNECTING) ||
                        (current == VOICE_CONNECTING &&
                         !before_deadline(atomic_load(&requested_at) + START_TIMEOUT_MS)))) {
            finish_call(&stream, atomic_load(&stop_call) ? ESP_OK : ESP_ERR_TIMEOUT);
            talking = false;
            current = VOICE_IDLE;
        }
        if (running && (resident.agent_id != agent_id ||
                        (current == VOICE_IDLE && !resident.chosen && tool_route_reserved(&resident)))) {
            if (talking) finish_call(&stream, ESP_ERR_INVALID_STATE);
            (void)agent_link_end_realtime();
            resident = (conversation_cache_t){0};
        } else if (!running && current != VOICE_BOOT && current != VOICE_FAULT && agent_id > 0) {
            const conversation_cache_t selected = load_conversation();
            const bool known_empty = selected.agent_id == agent_id && selected.suppress_create &&
                                     !selected.latest.id[0] && before_deadline(selected.valid_until);
            if (!known_empty && !before_deadline(retry_at)) {
                conversation_cache_t next;
                const esp_err_t err = request_session(&next, agent_id);
                next_stack_check = 0;
                if (err == ESP_OK) resident = next;
                ESP_LOGI(TAG, "Resident voice connection requested: result=%s", esp_err_to_name(err));
                retry_at = now_ms() + retry_ms;
                retry_ms = retry_ms < RESIDENT_RETRY_MAX_MS / 2 ? retry_ms * 2 : RESIDENT_RETRY_MAX_MS;
            }
        } else if (talking && !ready) {
            if (stream) {
                close_stream(&stream, "connection_not_ready");
                xQueueReset(uplink);
            }
            if (agent_link_realtime_recovering() && current != VOICE_RECOVERING) {
                atomic_store(&mode, VOICE_RECOVERING);
                ESP_LOGI(TAG, "Voice session state: reconnecting");
            }
        } else if (talking && ready) {
            if (!stream) {
                const esp_err_t err = agent_link_stream_open(AGENT_STREAM_VOICE, NULL, &stream);
                if (err == ESP_OK) {
                    voice_timing_set_active(true);
                    voice_playback_accept(true, NULL);
                    servo_tool_accept_transcripts(true);
                    ESP_LOGI(TAG, "Realtime voice ready: total_ms=%lu resident=yes",
                             (unsigned long)(now_ms() - atomic_load(&requested_at)));
                    atomic_store(&mode, VOICE_ACTIVE);
                    queue_tools(&resident, true);
                } else if (agent_link_state() == AGENT_STATE_READY) {
                    finish_call(&stream, err);
                }
            }
            if (stream && !atomic_load(&stop_call) && xQueuePeek(uplink, &frame, 0) == pdTRUE) {
                const esp_err_t err = agent_link_stream_write(stream, frame.pcm, frame.count * sizeof(frame.pcm[0]));
                if (err == ESP_OK) {
                    xQueueReceive(uplink, &frame, 0);
                    continue;
                }
                if (err != ESP_ERR_NO_MEM && agent_link_state() == AGENT_STATE_READY) {
                    finish_call(&stream, err);
                }
            }
        } else if (current == VOICE_IDLE && ready) {
            queue_tools(&resident, false);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(talking ? 10 : 250));
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
    if (tool_task) {
        vTaskDelete(tool_task);
        tool_task = NULL;
    }
    if (prefetch_task) {
        vTaskDelete(prefetch_task);
        prefetch_task = NULL;
    }
    if (conversation_lookup) {
        vSemaphoreDelete(conversation_lookup);
        conversation_lookup = NULL;
    }
    if (tool_jobs) {
        vQueueDelete(tool_jobs);
        tool_jobs = NULL;
    }
    voice_playback_stop();
    wake_word_deinit();
    sugarat_audio_deinit();
    if (uplink) {
        vQueueDelete(uplink);
        uplink = NULL;
    }
    wake_history.count = 0;
    wake_history.write_at = 0;
}

static bool start_audio_workers(void)
{
    conversation_lookup = xSemaphoreCreateMutexStatic(&conversation_mutex);
    conversations_init();
    uplink = xQueueCreateStatic(UPLINK_FRAMES, sizeof(audio_frame_t), (uint8_t *)uplink_storage, &uplink_queue);
    if (uplink && sugarat_audio_init() == ESP_OK && wake_word_init() == ESP_OK && voice_playback_start() == ESP_OK) {
        prefetch_task = xTaskCreateStatic(prefetch_conversation, "jarvis_prefetch", sizeof(prefetch_stack), NULL, 3,
                                         prefetch_stack, &prefetch_tcb);
        session_task = xTaskCreateStatic(handle_calls, "jarvis_session", sizeof(session_stack), NULL, 4,
                                         session_stack, &session_tcb);
        capture_task = xTaskCreateStatic(capture_audio, "jarvis_mic", sizeof(capture_stack), NULL, 5,
                                         capture_stack, &capture_tcb);
        tool_jobs = xQueueCreateStatic(1, sizeof(tool_job_t), tool_storage, &tool_queue);
        if (tool_jobs) {
            tool_task = xTaskCreateStatic(handle_tools, "jarvis_tools", sizeof(tool_stack), NULL, 3,
                                         tool_stack, &tool_tcb);
        }
        if (!tool_task) {
            ESP_LOGW(TAG, "Background voice tool refresh unavailable");
        }
        if (prefetch_task && session_task && capture_task) {
            ESP_LOGI(TAG, "Static PSRAM voice: uplink=%uB preroll=%uB worker stacks=%uB",
                     (unsigned)sizeof(uplink_storage), (unsigned)sizeof(wake_history),
                     (unsigned)(sizeof(prefetch_stack) + sizeof(session_stack) + sizeof(capture_stack) + sizeof(tool_stack)));
            return true;
        }
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
        .on_audio_reset_with_reason = voice_playback_reset,
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
        if (audio_ready) {
            agent_link_platform_set_voice_timing_callback(voice_timing_ws_event);
            voice_trace_init();
        }
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
    const int agent_id = current == VOICE_FAULT ? 0 : voice_agent_id();
    const conversation_cache_t selected = load_conversation();
    *view = (sugarat_voice_view_t){
        .can_end = current == VOICE_CONNECTING || current == VOICE_ACTIVE || current == VOICE_RECOVERING,
        .agent_id = agent_id,
    };
    if (selected.agent_id == agent_id) {
        memcpy(view->conversation_id, selected.latest.id, sizeof(view->conversation_id));
    }
    if (link.phase == AGENT_LINK_PHASE_PAIRING && link.code[0]) {
        view->phase = SUGARAT_VOICE_ACTIVATE;
        snprintf(view->activation_code, sizeof(view->activation_code), "%.6s", link.code);
    } else if (current == VOICE_ACTIVE) {
        view->phase = SUGARAT_VOICE_IN_CALL;
        const unsigned speaker_level = voice_playback_level();
        view->speaking = speaker_level != 0;
        view->level = view->speaking ? speaker_level : atomic_load(&mic_level);
    } else if (current == VOICE_STARTING || current == VOICE_CONNECTING) {
        view->phase = SUGARAT_VOICE_OPENING;
    } else if (current == VOICE_RECOVERING) {
        view->phase = SUGARAT_VOICE_RECONNECTING;
    } else if (current == VOICE_ENDING) {
        view->phase = SUGARAT_VOICE_ENDING;
    } else if (current == VOICE_MANAGING) {
        view->phase = SUGARAT_VOICE_PREPARING;
    } else if (current == VOICE_FAULT) {
        view->phase = SUGARAT_VOICE_ERROR;
    } else if (link.phase == AGENT_LINK_PHASE_SETUP) {
        view->phase = SUGARAT_VOICE_SETUP;
    } else if (link.phase == AGENT_LINK_PHASE_CONNECTING) {
        view->phase = SUGARAT_VOICE_JOINING;
    } else if (current == VOICE_IDLE) {
        if (agent_id <= 0) {
            view->phase = SUGARAT_VOICE_BIND_AGENT;
        } else if (selected.agent_id == agent_id && selected.suppress_create &&
                   !selected.latest.id[0] && before_deadline(selected.valid_until)) {
            view->phase = SUGARAT_VOICE_NO_SESSION;
        } else if (voice_connection_ready(agent_link_realtime_running())) {
            view->phase = SUGARAT_VOICE_READY;
        } else if (agent_link_realtime_recovering()) {
            view->phase = SUGARAT_VOICE_RECONNECTING;
        } else if (before_deadline(atomic_load(&error_until))) {
            view->phase = SUGARAT_VOICE_RETRY;
        } else {
            view->phase = SUGARAT_VOICE_PREPARING;
        }
    }
}

void sugarat_voice_end_call(void)
{
    atomic_store(&stop_call, true);
    if (session_task) xTaskNotifyGive(session_task);
}
