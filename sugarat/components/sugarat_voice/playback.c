#include "playback.h"

#include <stdatomic.h>
#include <string.h>

#include "agent_link.h"
#include "audio.h"
#include "audio_meter.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "turn_timing.h"
#include "voice_trace.h"

enum {
    PLAYBACK_FRAMES = 360,
    PLAYBACK_CAPACITY_MS = PLAYBACK_FRAMES * SUGARAT_AUDIO_FRAME * 1000 / SUGARAT_SAMPLE_RATE,
    BASE_PREBUFFER_MS = 400,
    FALLBACK_PREBUFFER_MS = 800,
    FALLBACK_REPLIES = 3,
    JITTER_PREBUFFER_MS = 1600,
    SLOW_PREBUFFER_MS = 5000,
    JITTER_GAP_MS = 500,
    SLOW_GAP_MS = 1000,
    SLOW_RATE_WINDOW_MS = 1200,
    // Release partial slots before Agent Link's three-quarter queue high-water mark.
    PREBUFFER_SLOT_LIMIT = PLAYBACK_FRAMES * 7 / 10,
    PA_DRAIN_MS = SUGARAT_SPEAKER_DMA_MS + 40,
    ECHO_GUARD_MS = 220,
    METER_HOLD_MS = 150,
    DRAIN_REPORTS = SUGARAT_SPEAKER_DMA_DESCRIPTORS + 2,
    REPORT_PLAYBACK_GAP_MS = 150,
    REPORT_PCM_GAP_MS = 150,
};

typedef struct {
    unsigned received_samples;
    unsigned dropped_frames;
    unsigned dropped_samples;
} playback_delivery_t;

typedef struct {
    unsigned generation;
    unsigned reply_id;
    // A zero-length frame marks the end of the reply after the preceding PCM.
    size_t count;
    union {
        int16_t pcm[SUGARAT_AUDIO_FRAME];
        playback_delivery_t delivery;
    };
} playback_frame_t;

typedef struct {
    unsigned id;
    playback_delivery_t delivery;
    unsigned enqueued_samples;
    unsigned consumed_samples;
    unsigned prebuffer_ms;
    int64_t first_audio_us;
    int64_t last_audio_us;
} playback_stats_t;

typedef struct {
    unsigned id;
    unsigned submitted_samples;
    unsigned write_errors;
    unsigned underruns;
    unsigned dma_empty_slots;
    unsigned max_write_gap_ms;
    unsigned max_empty_ms;
    int64_t last_write_us;
    sugarat_speaker_dma_t dma;
} playback_progress_t;

typedef struct {
    playback_delivery_t delivery;
    playback_progress_t progress;
    unsigned generation;
} playback_report_t;

typedef struct {
    uint32_t enqueued_samples;
    uint32_t processed_samples;
    unsigned last_i2s_reply;
} playback_queue_stats_t;

static const char *TAG = "jarvis_speaker";
static QueueHandle_t queue;
static StaticQueue_t queue_control;
static StaticSemaphore_t input_guard_storage;
static SemaphoreHandle_t input_guard;
static EXT_RAM_BSS_ATTR playback_frame_t storage[PLAYBACK_FRAMES + 1];
static EXT_RAM_BSS_ATTR _Alignas(16) StackType_t playback_stack[4096];
static StaticTask_t playback_tcb;
static TaskHandle_t worker;
static atomic_bool accepting;
static atomic_bool draining;
static atomic_uint generation;
static atomic_uint echo_until;
static atomic_uint meter_level;
static atomic_uint meter_until;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static playback_stats_t reply;
// Track PCM across reply-end markers. Those clear reply statistics while PCM may remain queued.
static playback_queue_stats_t queue_stats;
static unsigned reply_sequence;
static unsigned fallback_replies;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool voice_playback_echo_guard(void)
{
    const uint32_t until = atomic_load(&echo_until);
    return until != 0 && (int32_t)(until - now_ms()) > 0;
}

unsigned voice_playback_level(void)
{
    const uint32_t until = atomic_load(&meter_until);
    return until != 0 && (int32_t)(until - now_ms()) > 0 ? atomic_load(&meter_level) : 0;
}

static size_t playback_buffer_level(void *context)
{
    (void)context;
    // Each occupied slot consumes a full frame, even when the PCM chunk is shorter.
    return queue ? uxQueueMessagesWaiting(queue) * sizeof(((playback_frame_t *)0)->pcm) : 0;
}

static void log_reply(unsigned id, const char *reason, const playback_delivery_t *delivery,
                      const playback_progress_t *progress)
{
    ESP_LOGI(TAG, "Reply #%u %s: pcm=%u ms, i2s_submitted=%u ms, dropped_frames=%u, dropped_samples=%u, "
                  "underruns=%u, dma_empty_slots=%u, max_write_gap=%u ms, max_empty=%u ms, write_errors=%u",
             id, reason, delivery->received_samples / (SUGARAT_SAMPLE_RATE / 1000),
             progress->submitted_samples / (SUGARAT_SAMPLE_RATE / 1000), delivery->dropped_frames,
             delivery->dropped_samples, progress->underruns, progress->dma_empty_slots,
             progress->max_write_gap_ms, progress->max_empty_ms, progress->write_errors);
}

// input_guard orders PCM, end markers, and resets across the SDK and UI tasks.
static void finish_reply(void)
{
    portENTER_CRITICAL(&stats_lock);
    const playback_stats_t stats = reply;
    const unsigned current = atomic_load(&generation);
    reply = (playback_stats_t){0};
    portEXIT_CRITICAL(&stats_lock);
    voice_timing_reply_end();
    if (stats.id) {
        const playback_frame_t end = {.generation = current, .reply_id = stats.id, .delivery = stats.delivery};
        if (!stats.enqueued_samples || xQueueSend(queue, &end, 0) != pdTRUE) {
            log_reply(stats.id, "discarded", &stats.delivery, &(playback_progress_t){0});
        }
    }
    if (worker) {
        xTaskNotify(worker, 0, eNoAction);
    }
}

void voice_playback_end(void *context)
{
    (void)context;
    if (!input_guard) return;
    xSemaphoreTake(input_guard, portMAX_DELAY);
    if (atomic_load(&accepting)) finish_reply();
    xSemaphoreGive(input_guard);
}

static unsigned pending_pcm_ms(const playback_queue_stats_t *stats)
{
    // Producer and consumer snapshots can straddle a queue operation. This is
    // PCM not yet handled by the I2S writer, excluding the hardware DMA tail.
    const int32_t samples = (int32_t)(stats->enqueued_samples - stats->processed_samples);
    return samples > 0 ? (unsigned)samples / (SUGARAT_SAMPLE_RATE / 1000) : 0;
}

static void reset_playback(const char *reason, uint32_t reset_seq)
{
    const int64_t cleared_us = esp_timer_get_time();
    sugarat_speaker_enable(false);
    atomic_store(&draining, false);
    atomic_store(&meter_until, 0);
    portENTER_CRITICAL(&stats_lock);
    atomic_fetch_add(&generation, 1);
    const playback_stats_t stats = reply;
    const playback_queue_stats_t queued = queue_stats;
    reply = (playback_stats_t){0};
    queue_stats = (playback_queue_stats_t){0};
    portEXIT_CRITICAL(&stats_lock);
    voice_timing_reply_end();
    const unsigned slots = queue ? uxQueueMessagesWaiting(queue) : 0;
    if (queue) {
        xQueueReset(queue);
    }
    if (stats.id) {
        ESP_LOGI(TAG, "Reply #%u reset before speaker drain", stats.id);
    }
    if (worker) {
        xTaskNotify(worker, 0, eNoAction);
    }
    if (queue) {
        voice_trace_playback_reset(cleared_us, reason, reset_seq, stats.id, queued.last_i2s_reply,
                                   pending_pcm_ms(&queued), slots);
    }
}

void voice_playback_drain(const char *reason)
{
    if (!input_guard) return;
    xSemaphoreTake(input_guard, portMAX_DELAY);
    if (atomic_exchange(&accepting, false)) {
        portENTER_CRITICAL(&stats_lock);
        const unsigned queued_ms = pending_pcm_ms(&queue_stats);
        portEXIT_CRITICAL(&stats_lock);
        atomic_store(&draining, true);
        // Seal an incomplete reply so prebuffering can release its received tail.
        finish_reply();
        ESP_LOGI(TAG, "Playback draining: reason=%s pending_pcm_ms_est=%u", reason, queued_ms);
    }
    xSemaphoreGive(input_guard);
}

bool voice_playback_draining(void)
{
    return atomic_load(&draining);
}

void voice_playback_reset(agent_audio_reset_reason_t reason, uint32_t reset_seq, void *context)
{
    (void)context;
    if (!input_guard) return;
    switch (reason) {
        case AGENT_AUDIO_RESET_REMOTE_INTERRUPT:
        case AGENT_AUDIO_RESET_INPUT_START:
            xSemaphoreTake(input_guard, portMAX_DELAY);
            reset_playback(reason == AGENT_AUDIO_RESET_REMOTE_INTERRUPT ? "cloud_interrupt" : "input_started",
                           reset_seq);
            xSemaphoreGive(input_guard);
            break;
        case AGENT_AUDIO_RESET_INPUT_CLOSED:
            // The application decides whether to drain or discard before closing the mic.
            break;
        case AGENT_AUDIO_RESET_CONNECTION_LOST: voice_playback_drain("connection_lost"); break;
        case AGENT_AUDIO_RESET_CONNECTION_ENDED: voice_playback_drain("connection_ended"); break;
        case AGENT_AUDIO_RESET_CONNECTION_CANCELLED: voice_playback_drain("connection_cancelled"); break;
        default: voice_playback_drain("unknown_transport_reset"); break;
    }
}

void voice_playback_accept(bool enabled, const char *reset_reason)
{
    if (!input_guard) return;
    xSemaphoreTake(input_guard, portMAX_DELAY);
    const bool was_accepting = atomic_exchange(&accepting, enabled);
    if (!enabled && (was_accepting || atomic_load(&draining))) {
        reset_playback(reset_reason, 0);
    }
    xSemaphoreGive(input_guard);
}

void voice_playback_queue(const uint8_t *pcm, size_t bytes, void *context)
{
    (void)context;
    const size_t samples = bytes / sizeof(int16_t);
    if (!samples || !input_guard || !atomic_load(&accepting)) return;
    xSemaphoreTake(input_guard, portMAX_DELAY);
    if (!atomic_load(&accepting)) {
        xSemaphoreGive(input_guard);
        return;
    }
    const unsigned current = atomic_load(&generation);
    const int64_t arrival_us = esp_timer_get_time();
    portENTER_CRITICAL(&stats_lock);
    const bool first_pcm = !reply.id;
    if (first_pcm) {
        reply.id = ++reply_sequence;
        reply.first_audio_us = arrival_us;
        reply.prebuffer_ms = BASE_PREBUFFER_MS;
        if (fallback_replies) {
            reply.prebuffer_ms = FALLBACK_PREBUFFER_MS;
            --fallback_replies;
        }
    }
    const int64_t gap_us = first_pcm ? 0 : arrival_us - reply.last_audio_us;
    const int64_t span_us = arrival_us - reply.first_audio_us;
    const unsigned previous_prebuffer_ms = reply.prebuffer_ms;
    reply.last_audio_us = arrival_us;
    reply.delivery.received_samples += samples;
    const int64_t pcm_us = (int64_t)reply.delivery.received_samples * 1000000 / SUGARAT_SAMPLE_RATE;
    // An isolated short gap gets a modest reserve. Sustained delivery below 75% of
    // realtime or a full second between chunks needs more audio before playing.
    if (gap_us >= SLOW_GAP_MS * 1000LL ||
        (span_us >= SLOW_RATE_WINDOW_MS * 1000LL && pcm_us * 4 < span_us * 3)) {
        reply.prebuffer_ms = SLOW_PREBUFFER_MS;
    } else if (gap_us >= JITTER_GAP_MS * 1000LL && reply.prebuffer_ms < JITTER_PREBUFFER_MS) {
        reply.prebuffer_ms = JITTER_PREBUFFER_MS;
    }
    const unsigned prebuffer_ms = reply.prebuffer_ms;
    if (prebuffer_ms > previous_prebuffer_ms) {
        fallback_replies = FALLBACK_REPLIES;
    }
    const unsigned fallback_left = fallback_replies;
    playback_frame_t frame = {.generation = current, .reply_id = reply.id};
    portEXIT_CRITICAL(&stats_lock);
    if (first_pcm) voice_timing_first_pcm(frame.reply_id);

    size_t remaining = samples;
    while (remaining) {
        frame.count = remaining;
        if (frame.count > SUGARAT_AUDIO_FRAME) {
            frame.count = SUGARAT_AUDIO_FRAME;
        }
        const size_t copied = frame.count * sizeof(frame.pcm[0]);
        memcpy(frame.pcm, pcm, copied);
        // Leave one slot for voice_playback_end even if a burst fills the PCM queue.
        if (uxQueueSpacesAvailable(queue) <= 1 ||
            xQueueSend(queue, &frame, 0) != pdTRUE) {
            break;
        }
        pcm += copied;
        remaining -= frame.count;
    }
    const int64_t enqueued_ms = remaining != samples ? esp_timer_get_time() / 1000 : -1;
    portENTER_CRITICAL(&stats_lock);
    queue_stats.enqueued_samples += samples - remaining;
    if (reply.id == frame.reply_id) {
        reply.enqueued_samples += samples - remaining;
        reply.delivery.dropped_samples += remaining;
        reply.delivery.dropped_frames += (remaining + SUGARAT_AUDIO_FRAME - 1) / SUGARAT_AUDIO_FRAME;
    }
    portEXIT_CRITICAL(&stats_lock);
    if (remaining != samples) {
        xTaskNotify(worker, 0, eNoAction);
    }
    xSemaphoreGive(input_guard);
    if (first_pcm || gap_us >= REPORT_PCM_GAP_MS * 1000LL ||
        prebuffer_ms != previous_prebuffer_ms || remaining) {
        ESP_LOGI(TAG, "PCM enqueue: reply=%u callback_ms=%lld enqueue_ms=%lld gap_ms=%lld "
                      "accepted_samples=%u dropped_samples=%u received_ms=%lld target_ms=%u fallback_left=%u",
                 frame.reply_id, (long long)(arrival_us / 1000), (long long)enqueued_ms,
                 (long long)(gap_us / 1000), (unsigned)(samples - remaining), (unsigned)remaining,
                 (long long)(pcm_us / 1000), prebuffer_ms, fallback_left);
    }
}

static bool frame_is_current(const playback_frame_t *frame)
{
    return frame->generation == atomic_load(&generation);
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
        const unsigned samples = reply.enqueued_samples > reply.consumed_samples
                                 ? reply.enqueued_samples - reply.consumed_samples : 0;
        const int64_t first_audio_us = reply.first_audio_us;
        const unsigned target_ms = reply.prebuffer_ms;
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

static void report_drained(playback_report_t reports[DRAIN_REPORTS])
{
    const unsigned current = atomic_load(&generation);
    const unsigned completed = sugarat_speaker_dma().completed;
    const int64_t now_us = esp_timer_get_time();
    for (unsigned i = 0; i < DRAIN_REPORTS; ++i) {
        playback_report_t *report = &reports[i];
        const playback_progress_t *progress = &report->progress;
        if (!progress->id) continue;
        const bool drained = !progress->last_write_us ||
            completed - progress->dma.completed >= SUGARAT_SPEAKER_DMA_DESCRIPTORS;
        const bool timed_out = now_us - progress->last_write_us >= PA_DRAIN_MS * 1000LL;
        if (!drained && report->generation == current && !timed_out) continue;
        const char *reason = report->generation != current ? "reset" : drained ? "complete" : "drain_timeout";
        log_reply(progress->id, reason, &report->delivery, progress);
        report->progress.id = 0;
    }
}

static void queue_drain_report(playback_report_t reports[DRAIN_REPORTS], const playback_frame_t *end,
                               const playback_progress_t *progress)
{
    for (unsigned i = 0; i < DRAIN_REPORTS; ++i) {
        if (reports[i].progress.id) continue;
        reports[i] = (playback_report_t){.delivery = end->delivery, .progress = *progress,
                                         .generation = end->generation};
        return;
    }
    log_reply(progress->id, "drain_unverified", &end->delivery, progress);
}

static void record_underrun(const playback_frame_t *frame)
{
    portENTER_CRITICAL(&stats_lock);
    if (frame_is_current(frame)) {
        fallback_replies = FALLBACK_REPLIES;
        if (reply.id == frame->reply_id && reply.prebuffer_ms < JITTER_PREBUFFER_MS) {
            reply.prebuffer_ms = JITTER_PREBUFFER_MS;
        }
    }
    portEXIT_CRITICAL(&stats_lock);
}

static bool check_playback_gap(playback_progress_t *progress, const playback_frame_t *frame,
                               int64_t now_us)
{
    if (!progress->last_write_us) return false;
    const unsigned gap_ms = (unsigned)((now_us - progress->last_write_us) / 1000);
    const unsigned empty = sugarat_speaker_dma().empty_slots - progress->dma.empty_slots;
    if (!empty) return false;

    ++progress->underruns;
    progress->dma_empty_slots += empty;
    const unsigned empty_ms = empty * SUGARAT_SPEAKER_DMA_SAMPLES * 1000 / SUGARAT_SAMPLE_RATE;
    if (empty_ms > progress->max_empty_ms) progress->max_empty_ms = empty_ms;
    record_underrun(frame);
    if (gap_ms >= REPORT_PLAYBACK_GAP_MS) {
        ESP_LOGW(TAG, "Playback gap: reply=%u write_gap_ms=%u dma_empty_slots=%u", progress->id, gap_ms, empty);
    }
    return true;
}

static void play_audio(void *context)
{
    (void)context;
    playback_frame_t frame;
    playback_progress_t progress = {0};
    playback_report_t reports[DRAIN_REPORTS] = {0};
    int64_t last_write_us = 0;
    while (true) {
        report_drained(reports);
        if (xQueueReceive(queue, &frame, pdMS_TO_TICKS(PA_DRAIN_MS)) != pdTRUE) {
            sugarat_speaker_enable(false);
            if (atomic_load(&draining)) {
                xSemaphoreTake(input_guard, portMAX_DELAY);
                if (!uxQueueMessagesWaiting(queue)) atomic_store(&draining, false);
                xSemaphoreGive(input_guard);
            }
            continue;
        }
        if (!frame_is_current(&frame)) continue;
        if (!frame.count) {
            if (progress.id == frame.reply_id) {
                queue_drain_report(reports, &frame, &progress);
                progress = (playback_progress_t){0};
            }
            continue;
        }

        const bool first = progress.id != frame.reply_id;
        if (first) progress = (playback_progress_t){.id = frame.reply_id};
        const bool rebuffer = !first && check_playback_gap(&progress, &frame, esp_timer_get_time());
        if (first || rebuffer) {
            wait_for_prebuffer(&frame, last_write_us, rebuffer);
        }
        xSemaphoreTake(input_guard, portMAX_DELAY);
        const bool current = frame_is_current(&frame);
        if (current) sugarat_speaker_enable(true);
        xSemaphoreGive(input_guard);
        if (!current) continue;
        atomic_store(&echo_until, now_ms() + ECHO_GUARD_MS);
        const unsigned empty_before_write = sugarat_speaker_dma().empty_slots;
        const esp_err_t err = sugarat_speaker_write(frame.pcm, frame.count);
        atomic_store(&echo_until, now_ms() + ECHO_GUARD_MS);
        if (!frame_is_current(&frame)) {
            sugarat_speaker_enable(false);
            continue;
        }
        portENTER_CRITICAL(&stats_lock);
        if (frame.generation == atomic_load(&generation)) {
            queue_stats.processed_samples += frame.count;
            if (err == ESP_OK) queue_stats.last_i2s_reply = frame.reply_id;
        }
        if (reply.id == frame.reply_id) reply.consumed_samples += frame.count;
        portEXIT_CRITICAL(&stats_lock);
        if (err != ESP_OK) {
            ++progress.write_errors;
            ESP_LOGW(TAG, "Speaker write: %s", esp_err_to_name(err));
            sugarat_speaker_enable(false);
        } else {
            const unsigned level = voice_audio_level(frame.pcm, frame.count);
            if (level) {
                atomic_store(&meter_level, level);
                atomic_store(&meter_until, now_ms() + METER_HOLD_MS);
            }
            last_write_us = esp_timer_get_time();
            if (progress.submitted_samples == 0) voice_timing_speaker_start(frame.reply_id, last_write_us);
            const sugarat_speaker_dma_t dma = sugarat_speaker_dma();
            if (progress.last_write_us) {
                const unsigned gap_ms = (unsigned)((last_write_us - progress.last_write_us) / 1000);
                if (gap_ms > progress.max_write_gap_ms) progress.max_write_gap_ms = gap_ms;
                const unsigned empty = dma.empty_slots - empty_before_write;
                if (empty) {
                    ++progress.underruns;
                    progress.dma_empty_slots += empty;
                    record_underrun(&frame);
                    ESP_LOGW(TAG, "Speaker DMA starved during write: reply=%u empty_slots=%u", progress.id, empty);
                }
            }
            progress.last_write_us = last_write_us;
            progress.submitted_samples += frame.count;
            progress.dma = dma;
        }
    }
}

esp_err_t voice_playback_start(void)
{
    input_guard = xSemaphoreCreateMutexStatic(&input_guard_storage);
    if (!input_guard) return ESP_ERR_NO_MEM;
    queue = xQueueCreateStatic(PLAYBACK_FRAMES + 1, sizeof(playback_frame_t), (uint8_t *)storage, &queue_control);
    if (queue) {
        worker = xTaskCreateStatic(play_audio, TAG, sizeof(playback_stack), NULL, 6, playback_stack, &playback_tcb);
    }
    if (!worker) {
        voice_playback_stop();
        return ESP_ERR_NO_MEM;
    }
    agent_link_playback_set_buffer_ms(PLAYBACK_CAPACITY_MS);
    agent_link_playback_set_buffer_level_callback(playback_buffer_level, NULL);
    ESP_LOGI(TAG, "Playback buffer: %u ms (%u slots), underrun threshold: %u ms",
             PLAYBACK_CAPACITY_MS, PLAYBACK_FRAMES, SUGARAT_SPEAKER_DMA_MS);
    ESP_LOGI(TAG, "Playback prebuffer: normal=%u ms fallback=%u ms for %u replies jitter=%u ms slow=%u ms (static PSRAM)",
             BASE_PREBUFFER_MS, FALLBACK_PREBUFFER_MS, FALLBACK_REPLIES, JITTER_PREBUFFER_MS, SLOW_PREBUFFER_MS);
    return ESP_OK;
}

void voice_playback_stop(void)
{
    atomic_store(&accepting, false);
    atomic_store(&draining, false);
    if (worker) {
        vTaskDelete(worker);
        worker = NULL;
    }
    if (queue) {
        const int64_t cleared_us = esp_timer_get_time();
        const unsigned slots = uxQueueMessagesWaiting(queue);
        portENTER_CRITICAL(&stats_lock);
        const unsigned incoming_reply = reply.id;
        const playback_queue_stats_t queued = queue_stats;
        queue_stats = (playback_queue_stats_t){0};
        fallback_replies = 0;
        portEXIT_CRITICAL(&stats_lock);
        vQueueDelete(queue);
        queue = NULL;
        voice_trace_playback_reset(cleared_us, "speaker_stop", 0, incoming_reply, queued.last_i2s_reply,
                                   pending_pcm_ms(&queued), slots);
    }
    if (input_guard) {
        vSemaphoreDelete(input_guard);
        input_guard = NULL;
    }
}
