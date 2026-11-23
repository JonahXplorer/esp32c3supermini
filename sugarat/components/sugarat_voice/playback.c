#include "playback.h"

#include <stdatomic.h>
#include <string.h>

#include "agent_link.h"
#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum {
    PLAYBACK_FRAMES = 132,
    PLAYBACK_CAPACITY_MS = PLAYBACK_FRAMES * SUGARAT_AUDIO_FRAME * 1000 / SUGARAT_SAMPLE_RATE,
    BASE_PREBUFFER_MS = 800,
    JITTER_PREBUFFER_MS = 1600,
    JITTER_GAP_MS = 500,
    JITTER_HOLD_MS = 30000,
    // Leave room for Agent Link to deliver a decoded chunk even if PCM uses partial slots.
    PREBUFFER_SLOT_LIMIT = PLAYBACK_FRAMES * 2 / 3,
    PA_DRAIN_MS = 120,
    ECHO_GUARD_MS = 220,
    REPORT_PLAYBACK_GAP_MS = 150,
};

typedef struct {
    unsigned generation;
    unsigned reply_id;
    size_t count;
    int16_t pcm[SUGARAT_AUDIO_FRAME];
} playback_frame_t;

typedef struct {
    unsigned id;
    unsigned received_samples;
    unsigned enqueued_samples;
    unsigned played_samples;
    unsigned dropped_frames;
    unsigned dropped_samples;
    unsigned underruns;
    unsigned max_empty_ms;
    int64_t first_audio_us;
    int64_t last_audio_us;
    int64_t empty_since_us;
    bool rebuffer_pending;
} playback_stats_t;

static const char *TAG = "jarvis_speaker";
static QueueHandle_t queue;
static StaticQueue_t queue_control;
static uint8_t *storage;
static TaskHandle_t worker;
static atomic_bool accepting;
static atomic_uint generation;
static atomic_uint echo_until;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static playback_stats_t reply;
static unsigned reply_sequence;
static int64_t jitter_until_us;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool voice_playback_echo_guard(void)
{
    const uint32_t until = atomic_load(&echo_until);
    return until != 0 && (int32_t)(until - now_ms()) > 0;
}

static size_t playback_buffer_level(void *context)
{
    (void)context;
    // Each occupied slot consumes a full frame, even when the PCM chunk is shorter.
    return queue ? uxQueueMessagesWaiting(queue) * sizeof(((playback_frame_t *)0)->pcm) : 0;
}

static void finish_reply(const char *reason)
{
    portENTER_CRITICAL(&stats_lock);
    const playback_stats_t stats = reply;
    reply = (playback_stats_t){0};
    portEXIT_CRITICAL(&stats_lock);
    if (stats.id) {
        ESP_LOGI(TAG, "Reply #%u %s: pcm=%u ms, dropped_frames=%u, dropped_samples=%u, underruns=%u, max_empty=%u ms",
                 stats.id, reason, stats.received_samples / (SUGARAT_SAMPLE_RATE / 1000),
                 stats.dropped_frames, stats.dropped_samples, stats.underruns, stats.max_empty_ms);
    }
}

void voice_playback_end(void *context)
{
    (void)context;
    finish_reply("complete");
    if (worker) {
        xTaskNotify(worker, 0, eNoAction);
    }
}

void voice_playback_reset(void *context)
{
    (void)context;
    atomic_fetch_add(&generation, 1);
    if (queue) {
        xQueueReset(queue);
    }
    finish_reply("reset");
    if (worker) {
        xTaskNotify(worker, 0, eNoAction);
    }
}

void voice_playback_accept(bool enabled)
{
    atomic_store(&accepting, enabled);
    if (!enabled) {
        voice_playback_reset(NULL);
    }
}

void voice_playback_queue(const uint8_t *pcm, size_t bytes, void *context)
{
    (void)context;
    const unsigned current = atomic_load(&generation);
    const size_t samples = bytes / sizeof(int16_t);
    if (!samples || !atomic_load(&accepting)) {
        return;
    }
    const int64_t arrival_us = esp_timer_get_time();
    int64_t gap_start_us = 0;
    portENTER_CRITICAL(&stats_lock);
    if (current != atomic_load(&generation) || !atomic_load(&accepting)) {
        portEXIT_CRITICAL(&stats_lock);
        return;
    }
    if (!reply.id) {
        reply.id = ++reply_sequence;
        reply.first_audio_us = arrival_us;
    }
    if (reply.last_audio_us && arrival_us - reply.last_audio_us >= JITTER_GAP_MS * 1000LL) {
        jitter_until_us = arrival_us + JITTER_HOLD_MS * 1000LL;
    }
    reply.last_audio_us = arrival_us;
    // Count a gap only when more audio arrives for this reply. Allow for PCM still in DMA.
    if (reply.empty_since_us) {
        const int64_t empty_ms = (arrival_us - reply.empty_since_us) / 1000;
        if (empty_ms >= SUGARAT_SPEAKER_DMA_MS) {
            ++reply.underruns;
            reply.rebuffer_pending = true;
            jitter_until_us = arrival_us + JITTER_HOLD_MS * 1000LL;
            if (empty_ms > reply.max_empty_ms) {
                reply.max_empty_ms = (unsigned)empty_ms;
            }
        }
        if (empty_ms >= REPORT_PLAYBACK_GAP_MS) {
            gap_start_us = reply.empty_since_us;
        }
        reply.empty_since_us = 0;
    }
    reply.received_samples += samples;
    playback_frame_t frame = {.generation = current, .reply_id = reply.id};
    portEXIT_CRITICAL(&stats_lock);

    size_t remaining = samples;
    while (remaining) {
        frame.count = remaining;
        if (frame.count > SUGARAT_AUDIO_FRAME) {
            frame.count = SUGARAT_AUDIO_FRAME;
        }
        const size_t copied = frame.count * sizeof(frame.pcm[0]);
        memcpy(frame.pcm, pcm, copied);
        if (xQueueSend(queue, &frame, 0) != pdTRUE) {
            break;
        }
        pcm += copied;
        remaining -= frame.count;
    }
    portENTER_CRITICAL(&stats_lock);
    if (reply.id == frame.reply_id) {
        reply.enqueued_samples += samples - remaining;
        reply.dropped_samples += remaining;
        reply.dropped_frames += (remaining + SUGARAT_AUDIO_FRAME - 1) / SUGARAT_AUDIO_FRAME;
    }
    portEXIT_CRITICAL(&stats_lock);
    if (remaining != samples) {
        xTaskNotify(worker, 0, eNoAction);
    }
    if (gap_start_us) {
        ESP_LOGI(TAG, "Playback gap: reply=%u start_ms=%u end_ms=%u empty_ms=%u", frame.reply_id,
                 (unsigned)(gap_start_us / 1000), (unsigned)(arrival_us / 1000),
                 (unsigned)((arrival_us - gap_start_us) / 1000));
    }
}

static bool frame_is_current(const playback_frame_t *frame)
{
    return atomic_load(&accepting) && frame->generation == atomic_load(&generation);
}

static void wait_for_prebuffer(const playback_frame_t *frame, int64_t last_write_us, bool rebuffer)
{
    // On a reply boundary or underrun, hold one frame while the queue fills.
    // A normal reply end releases short replies and short tails.
    const int64_t started_us = esp_timer_get_time();
    while (frame_is_current(frame)) {
        const int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&stats_lock);
        const bool finished = reply.id != frame->reply_id;
        const unsigned samples = reply.enqueued_samples > reply.played_samples
                                 ? reply.enqueued_samples - reply.played_samples : 0;
        const int64_t first_audio_us = reply.first_audio_us;
        const unsigned target_ms = rebuffer || now_us < jitter_until_us ? JITTER_PREBUFFER_MS : BASE_PREBUFFER_MS;
        portEXIT_CRITICAL(&stats_lock);
        if (finished) {
            ESP_LOGI(TAG, "Playback %s: reply=%u trigger=reply_end", rebuffer ? "resumed" : "start", frame->reply_id);
            return;
        }
        const unsigned waited_ms = (unsigned)((now_us - (rebuffer ? started_us : first_audio_us)) / 1000);
        const unsigned buffered_ms = samples / (SUGARAT_SAMPLE_RATE / 1000);
        if (buffered_ms >= target_ms || uxQueueMessagesWaiting(queue) >= PREBUFFER_SLOT_LIMIT) {
            ESP_LOGI(TAG, "Playback %s: reply=%u trigger=%s buffered_ms=%u target_ms=%u waited_ms=%u",
                     rebuffer ? "resumed" : "start", frame->reply_id,
                     buffered_ms >= target_ms ? "prebuffer" : "queue_limit", buffered_ms, target_ms, waited_ms);
            return;
        }
        if (now_us - last_write_us >= PA_DRAIN_MS * 1000) {
            sugarat_speaker_enable(false);
        }
        xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(PA_DRAIN_MS));
    }
}

static void play_audio(void *context)
{
    (void)context;
    playback_frame_t frame;
    unsigned playing_reply = 0;
    unsigned last_written_reply = 0;
    int64_t last_write_us = 0;
    while (true) {
        if (last_written_reply && uxQueueMessagesWaiting(queue) == 0) {
            portENTER_CRITICAL(&stats_lock);
            if (reply.id == last_written_reply) {
                reply.empty_since_us = last_write_us;
            }
            portEXIT_CRITICAL(&stats_lock);
            last_written_reply = 0;
        }
        if (xQueueReceive(queue, &frame, pdMS_TO_TICKS(PA_DRAIN_MS)) != pdTRUE) {
            sugarat_speaker_enable(false);
            continue;
        }
        const bool first = frame.reply_id != playing_reply;
        portENTER_CRITICAL(&stats_lock);
        const bool rebuffer = !first && reply.id == frame.reply_id && reply.rebuffer_pending;
        if (reply.id == frame.reply_id) reply.rebuffer_pending = false;
        portEXIT_CRITICAL(&stats_lock);
        if (first || rebuffer) {
            wait_for_prebuffer(&frame, last_write_us, rebuffer);
        }
        if (!frame_is_current(&frame)) {
            sugarat_speaker_enable(false);
            continue;
        }
        playing_reply = frame.reply_id;
        portENTER_CRITICAL(&stats_lock);
        if (reply.id == frame.reply_id) {
            reply.empty_since_us = 0;
            reply.played_samples += frame.count;
        }
        portEXIT_CRITICAL(&stats_lock);
        atomic_store(&echo_until, now_ms() + ECHO_GUARD_MS);
        sugarat_speaker_enable(true);
        const esp_err_t err = sugarat_speaker_write(frame.pcm, frame.count);
        atomic_store(&echo_until, now_ms() + ECHO_GUARD_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Speaker write: %s", esp_err_to_name(err));
            sugarat_speaker_enable(false);
        } else {
            last_written_reply = frame.reply_id;
            last_write_us = esp_timer_get_time();
        }
    }
}

esp_err_t voice_playback_start(void)
{
    storage = heap_caps_malloc(PLAYBACK_FRAMES * sizeof(playback_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage) {
        queue = xQueueCreateStatic(PLAYBACK_FRAMES, sizeof(playback_frame_t), storage, &queue_control);
    }
    if (!queue || xTaskCreateWithCaps(play_audio, TAG, 4096, NULL, 4, &worker,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        voice_playback_stop();
        return ESP_ERR_NO_MEM;
    }
    agent_link_playback_set_buffer_ms(PLAYBACK_CAPACITY_MS);
    agent_link_playback_set_buffer_level_callback(playback_buffer_level, NULL);
    ESP_LOGI(TAG, "Playback buffer: %u ms (%u slots), underrun threshold: %u ms",
             PLAYBACK_CAPACITY_MS, PLAYBACK_FRAMES, SUGARAT_SPEAKER_DMA_MS);
    ESP_LOGI(TAG, "Playback prebuffer: %u ms, up to %u ms when jitter is detected",
             BASE_PREBUFFER_MS, JITTER_PREBUFFER_MS);
    return ESP_OK;
}

void voice_playback_stop(void)
{
    if (worker) {
        vTaskDeleteWithCaps(worker);
        worker = NULL;
    }
    if (queue) {
        vQueueDelete(queue);
        queue = NULL;
    }
    heap_caps_free(storage);
    storage = NULL;
}
