#include "playback.h"

#include <stdatomic.h>
#include <string.h>

#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum { PLAYBACK_FRAMES = 72, PA_DRAIN_MS = 120, ECHO_GUARD_MS = 220 };

typedef struct {
    unsigned generation;
    size_t count;
    int16_t pcm[SUGARAT_AUDIO_FRAME];
} playback_frame_t;

static const char *TAG = "jarvis_speaker";
static QueueHandle_t queue;
static StaticQueue_t queue_control;
static uint8_t *storage;
static TaskHandle_t worker;
static atomic_bool accepting;
static atomic_uint generation;
static atomic_uint echo_until;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool voice_playback_echo_guard(void)
{
    const uint32_t until = atomic_load(&echo_until);
    return until != 0 && (int32_t)(until - now_ms()) > 0;
}

void voice_playback_reset(void *context)
{
    (void)context;
    atomic_fetch_add(&generation, 1);
    if (queue) {
        xQueueReset(queue);
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
    if (!atomic_load(&accepting)) {
        return;
    }
    playback_frame_t frame = {.generation = current};
    while (bytes >= sizeof(frame.pcm[0])) {
        frame.count = bytes / sizeof(frame.pcm[0]);
        if (frame.count > SUGARAT_AUDIO_FRAME) {
            frame.count = SUGARAT_AUDIO_FRAME;
        }
        const size_t copied = frame.count * sizeof(frame.pcm[0]);
        memcpy(frame.pcm, pcm, copied);
        if (xQueueSend(queue, &frame, 0) != pdTRUE) {
            return;
        }
        pcm += copied;
        bytes -= copied;
    }
}

static void play_audio(void *context)
{
    (void)context;
    playback_frame_t frame;
    while (true) {
        if (xQueueReceive(queue, &frame, pdMS_TO_TICKS(PA_DRAIN_MS)) != pdTRUE) {
            sugarat_speaker_enable(false);
            continue;
        }
        if (!atomic_load(&accepting) || frame.generation != atomic_load(&generation)) {
            sugarat_speaker_enable(false);
            continue;
        }
        atomic_store(&echo_until, now_ms() + ECHO_GUARD_MS);
        sugarat_speaker_enable(true);
        const esp_err_t err = sugarat_speaker_write(frame.pcm, frame.count);
        atomic_store(&echo_until, now_ms() + ECHO_GUARD_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Speaker write: %s", esp_err_to_name(err));
            sugarat_speaker_enable(false);
        }
    }
}

esp_err_t voice_playback_start(void)
{
    storage = heap_caps_malloc(PLAYBACK_FRAMES * sizeof(playback_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage) {
        queue = xQueueCreateStatic(PLAYBACK_FRAMES, sizeof(playback_frame_t), storage, &queue_control);
    }
    if (!queue || xTaskCreate(play_audio, TAG, 4096, NULL, 4, &worker) != pdPASS) {
        voice_playback_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_playback_stop(void)
{
    if (worker) {
        vTaskDelete(worker);
        worker = NULL;
    }
    if (queue) {
        vQueueDelete(queue);
        queue = NULL;
    }
    heap_caps_free(storage);
    storage = NULL;
}
