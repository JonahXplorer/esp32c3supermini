#include "voice_trace.h"

#include <string.h>
#include "agent_link_platform.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "freertos/FreeRTOS.h"

enum { REPORT_US = 1000000, PREVIEW_BYTES = 96, MAX_OTHER_EVENTS = 6, EVENT_BYTES = 64 };

typedef struct {
    uint32_t stream;
    unsigned last_frame, frames, classified, speech;
    int64_t first_us, last_us;
    int64_t speech_first_us, speech_last_us;
} uplink_window_t;

static const char *TAG = "voice_timeline";
static portMUX_TYPE trace_lock = portMUX_INITIALIZER_UNLOCKED;
static vad_handle_t vad;
static int64_t wake_us;
static uint32_t stream;
static uplink_window_t window;
static unsigned starts, ends, partials, finals, other_count;
static char other_events[MAX_OTHER_EVENTS][EVENT_BYTES];

static long long since_wake(int64_t at, int64_t wake)
{
    return at >= wake && wake > 0 ? (long long)((at - wake) / 1000) : -1;
}

static int64_t current_wake(int64_t at, uint32_t *current_stream)
{
    portENTER_CRITICAL(&trace_lock);
    const int64_t wake = at >= wake_us ? wake_us : 0;
    if (current_stream) *current_stream = wake ? stream : 0;
    portEXIT_CRITICAL(&trace_lock);
    return wake;
}

void voice_trace_playback_reset(int64_t cleared_us, const char *reason, uint32_t reset_seq,
                                unsigned incoming_reply, unsigned last_i2s_reply,
                                unsigned pending_pcm_ms, unsigned queue_slots)
{
    uint32_t current_stream;
    const int64_t wake = current_wake(cleared_us, &current_stream);
    ESP_LOGI(TAG, "wake_ms=%lld stream=%lu at_ms=%lld boot_ms=%lld event=playback_reset reason=%s "
                 "reset_seq=%lu incoming_reply=%u last_i2s_reply=%u pending_pcm_ms_est=%u queue_slots=%u",
             (long long)(wake / 1000), (unsigned long)current_stream, since_wake(cleared_us, wake),
             (long long)(cleared_us / 1000), reason, (unsigned long)reset_seq,
             incoming_reply, last_i2s_reply, pending_pcm_ms, queue_slots);
}

static void log_uplink(const uplink_window_t *sample, int64_t wake)
{
    if (!sample->frames) return;
    ESP_LOGI(TAG, "wake_ms=%lld stream=%lu at_ms=%lld event=uplink first_ms=%lld frame=%u..%u "
                 "frames=%u vad_frames=%u speech_frames=%u first_speech_ms=%lld last_speech_ms=%lld",
             (long long)(wake / 1000), (unsigned long)sample->stream,
             since_wake(sample->last_us, wake), since_wake(sample->first_us, wake),
             sample->last_frame - sample->frames + 1, sample->last_frame,
             sample->frames, sample->classified, sample->speech,
             since_wake(sample->speech_first_us, wake), since_wake(sample->speech_last_us, wake));
}

static int detect_voice(const uint8_t *pcm, size_t bytes)
{
    if (!vad || bytes != SUGARAT_AUDIO_FRAME * sizeof(int16_t)) return -1;
    int16_t samples[SUGARAT_AUDIO_FRAME];
    memcpy(samples, pcm, bytes);
    bool audible = false;
    for (size_t i = 0; i < SUGARAT_AUDIO_FRAME; ++i) audible |= samples[i] != 0;
    const vad_state_t detected = vad_process(vad, samples, SUGARAT_SAMPLE_RATE, 20);
    return audible && detected == VAD_SPEECH ? 1 : 0;
}

static bool new_control_event(const char *event)
{
    if (!event || other_count >= MAX_OTHER_EVENTS) return false;
    const size_t length = strnlen(event, EVENT_BYTES);
    if (length == 0 || length >= EVENT_BYTES) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = event[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    for (unsigned i = 0; i < other_count; ++i) {
        if (!strcmp(other_events[i], event)) return false;
    }
    memcpy(other_events[other_count++], event, length + 1);
    return true;
}

static void copy_preview(char out[PREVIEW_BYTES + 1], const char *text)
{
    size_t used = 0;
    while (text && *text && used < PREVIEW_BYTES) {
        const unsigned char c = (unsigned char)*text;
        const size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (width > PREVIEW_BYTES - used || strnlen(text, width) < width) break;
        if (c < 0x20 || c == 0x7f || c == '"') {
            out[used++] = c == '"' ? '\'' : ' ';
        } else {
            memcpy(out + used, text, width);
            used += width;
        }
        text += width;
    }
    out[used] = '\0';
}

static void add_uplink(const agent_link_platform_voice_trace_t *event)
{
    if (!window.frames) {
        window.stream = event->stream;
        window.first_us = event->at_us;
    }
    window.last_us = event->at_us;
    window.last_frame = event->last_frame;
    window.frames += event->frames;
    window.classified += event->classified_frames;
    window.speech += event->speech_frames;
    if (event->speech_frames) {
        if (!window.speech_first_us) window.speech_first_us = event->at_us;
        window.speech_last_us = event->at_us;
    }
}

static void observe(const agent_link_platform_voice_trace_t *event)
{
    if (event->kind == AGENT_LINK_VOICE_TRACE_RESPONSE_INTERRUPTED) {
        const int64_t wake = current_wake(event->at_us, NULL);
        char reason[PREVIEW_BYTES + 1];
        copy_preview(reason, event->text);
        ESP_LOGI(TAG, "wake_ms=%lld stream=%lu at_ms=%lld boot_ms=%lld event=response_interrupted "
                     "wire=response.interrupted reset_seq=%lu tx_frames=%u reason=\"%s\"",
                 (long long)(wake / 1000), (unsigned long)event->stream, since_wake(event->at_us, wake),
                 (long long)(event->at_us / 1000), (unsigned long)event->reset_seq, event->last_frame, reason);
        return;
    }
    static const char *const names[] = {
        [AGENT_LINK_VOICE_TRACE_SPEECH_START] = "cloud_speech_start",
        [AGENT_LINK_VOICE_TRACE_SPEECH_END] = "cloud_speech_end",
        [AGENT_LINK_VOICE_TRACE_PARTIAL] = "transcript_partial",
        [AGENT_LINK_VOICE_TRACE_FINAL] = "transcript_final",
        [AGENT_LINK_VOICE_TRACE_RESPONSE_START] = "response_start",
        [AGENT_LINK_VOICE_TRACE_FIRST_AUDIO] = "first_audio",
        [AGENT_LINK_VOICE_TRACE_OTHER] = "cloud_event",
    };
    uplink_window_t finished = {0};
    int64_t wake;
    bool log_event = false;
    unsigned vad_starts = 0, vad_ends = 0;
    portENTER_CRITICAL(&trace_lock);
    wake = wake_us;
    if (wake && event->at_us >= wake && (!stream || (int32_t)(event->stream - stream) >= 0)) {
        if (stream != event->stream) {
            finished = window;
            window = (uplink_window_t){0};
            stream = event->stream;
        }
        if (event->kind == AGENT_LINK_VOICE_TRACE_UPLINK) {
            add_uplink(event);
            if (!finished.frames && (event->last_frame == event->frames ||
                event->at_us - window.first_us >= REPORT_US)) {
                finished = window;
                window = (uplink_window_t){0};
            }
        } else {
            log_event = event->kind != AGENT_LINK_VOICE_TRACE_OTHER || new_control_event(event->event);
            if (log_event && !finished.frames) {
                finished = window;
                window = (uplink_window_t){0};
            }
            starts += event->kind == AGENT_LINK_VOICE_TRACE_SPEECH_START;
            ends += event->kind == AGENT_LINK_VOICE_TRACE_SPEECH_END;
            partials += event->kind == AGENT_LINK_VOICE_TRACE_PARTIAL;
            finals += event->kind == AGENT_LINK_VOICE_TRACE_FINAL;
            vad_starts = starts;
            vad_ends = ends;
        }
    }
    portEXIT_CRITICAL(&trace_lock);
    log_uplink(&finished, wake);
    if (log_event) {
        char preview[PREVIEW_BYTES + 1];
        copy_preview(preview, event->text);
        ESP_LOGI(TAG, "wake_ms=%lld stream=%lu at_ms=%lld event=%s wire=%s tx_frames=%u "
                     "cloud_speech_starts=%u cloud_speech_ends=%u text=\"%s\"",
                 (long long)(wake / 1000), (unsigned long)event->stream, since_wake(event->at_us, wake),
                 names[event->kind], event->event, event->last_frame, vad_starts, vad_ends, preview);
    }
}

void voice_trace_init(void)
{
    vad = vad_create(VAD_MODE_2);
    if (!vad) ESP_LOGW(TAG, "Local VAD unavailable; sent frames will still be counted");
    agent_link_platform_set_voice_trace(detect_voice, observe);
}

void voice_trace_begin(const char *source, int64_t started_us)
{
    portENTER_CRITICAL(&trace_lock);
    wake_us = started_us;
    stream = 0;
    starts = ends = partials = finals = other_count = 0;
    window = (uplink_window_t){0};
    portEXIT_CRITICAL(&trace_lock);
    ESP_LOGI(TAG, "wake_ms=%lld at_ms=0 event=capture_requested source=%s local_vad=%s",
             (long long)(started_us / 1000), source, vad ? "webrtc" : "unavailable");
}

void voice_trace_end(void)
{
    portENTER_CRITICAL(&trace_lock);
    const int64_t wake = wake_us;
    const uplink_window_t finished = window;
    const unsigned vad_starts = starts, vad_ends = ends, partial_count = partials, final_count = finals;
    wake_us = 0;
    window = (uplink_window_t){0};
    portEXIT_CRITICAL(&trace_lock);
    if (!wake) return;
    log_uplink(&finished, wake);
    ESP_LOGI(TAG, "wake_ms=%lld at_ms=%lld event=capture_ended cloud_speech_starts=%u "
                 "cloud_speech_ends=%u partials=%u finals=%u",
             (long long)(wake / 1000), since_wake(esp_timer_get_time(), wake),
             vad_starts, vad_ends, partial_count, final_count);
}
