#include "turn_timing.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

enum {
    SPEECH_LEVEL_MIN = 3,
    SPEECH_CONFIRM_US = 40000,
    SPEECH_SILENCE_US = 250000,
    MIC_MAX_SAMPLE_GAP_US = 100000,
    MATCH_WINDOW_US = 30000000,
    REPLY_RECORDS = 16,
};

typedef struct {
    unsigned turn;
    int64_t speech_end_us;
    int64_t final_us;
    int64_t response_us;
    int64_t audio_us;
} turn_stamps_t;

typedef struct {
    unsigned reply_id;
    turn_stamps_t stamps;
} reply_record_t;

typedef struct {
    int64_t last_sample_us;
    int64_t voiced_since_us;
    int64_t last_voiced_us;
    int64_t quiet_since_us;
    int64_t confirmed_end_us;
    unsigned final_during_silence;
    bool speaking;
} mic_timing_t;

static const char *TAG = "voice_timing";
static portMUX_TYPE timing_lock = portMUX_INITIALIZER_UNLOCKED;
static bool tracking;
static mic_timing_t mic;
static unsigned turn_sequence;
static int64_t last_final_us;
static turn_stamps_t pending_transcript;
static turn_stamps_t response;
// Network events and I2S playback run on separate tasks. Capture the stamps
// per playback reply so a subsequent reply cannot overwrite an earlier one.
static EXT_RAM_BSS_ATTR reply_record_t records[REPLY_RECORDS];

static long long timestamp_ms(int64_t time)
{
    return time ? (long long)(time / 1000) : -1;
}

static long long duration_ms(int64_t from, int64_t to)
{
    return from && to >= from ? (long long)((to - from) / 1000) : -1;
}

static bool recently(int64_t time, int64_t at)
{
    return time && at >= time && at - time <= MATCH_WINDOW_US;
}

void voice_timing_set_active(bool active)
{
    portENTER_CRITICAL(&timing_lock);
    if (tracking != active) {
        tracking = active;
        mic = (mic_timing_t){0};
        last_final_us = 0;
        pending_transcript = (turn_stamps_t){0};
        response = (turn_stamps_t){0};
        memset(records, 0, sizeof(records));
    }
    portEXIT_CRITICAL(&timing_lock);
}

static void attach_confirmed_end(unsigned turn, int64_t ended_us)
{
    if (pending_transcript.turn == turn) pending_transcript.speech_end_us = ended_us;
    if (response.turn == turn) response.speech_end_us = ended_us;
    for (size_t i = 0; i < REPLY_RECORDS; ++i) {
        if (records[i].reply_id && records[i].stamps.turn == turn) {
            records[i].stamps.speech_end_us = ended_us;
        }
    }
}

void voice_timing_mic_level(unsigned level, bool echo_suppressed)
{
    const int64_t now = esp_timer_get_time();
    int64_t ended_us = 0;
    portENTER_CRITICAL(&timing_lock);
    if (tracking && echo_suppressed) {
        // Playback hides the actual microphone level. Break the detection
        // interval without completing a speech segment or keeping an old end.
        mic = (mic_timing_t){0};
    } else if (tracking) {
        if (mic.last_sample_us && now - mic.last_sample_us > MIC_MAX_SAMPLE_GAP_US) {
            mic = (mic_timing_t){0};
        }
        mic.last_sample_us = now;
        if (level >= SPEECH_LEVEL_MIN) {
            if (mic.final_during_silence) {
                mic.speaking = false;
                mic.final_during_silence = 0;
            }
            mic.quiet_since_us = 0;
            mic.confirmed_end_us = 0;
            if (!mic.voiced_since_us) mic.voiced_since_us = now;
            if (!mic.speaking && now - mic.voiced_since_us >= SPEECH_CONFIRM_US) {
                mic.speaking = true;
                // An unclaimed transcript cannot be assigned to this new speech.
                if (pending_transcript.final_us < mic.voiced_since_us) pending_transcript = (turn_stamps_t){0};
            }
            if (mic.speaking) mic.last_voiced_us = now;
        } else {
            mic.voiced_since_us = 0;
            if (mic.speaking && !mic.quiet_since_us) mic.quiet_since_us = now;
            if (mic.speaking && now - mic.quiet_since_us >= SPEECH_SILENCE_US) {
                mic.speaking = false;
                ended_us = mic.last_voiced_us;
                // A final received during this uninterrupted quiet interval
                // already owns this end. It cannot also satisfy the next final.
                if (mic.final_during_silence) {
                    attach_confirmed_end(mic.final_during_silence, ended_us);
                    mic.final_during_silence = 0;
                } else {
                    mic.confirmed_end_us = ended_us;
                }
            }
        }
    }
    portEXIT_CRITICAL(&timing_lock);
    if (ended_us) ESP_LOGI(TAG, "Speech end estimate: at_ms=%lld", timestamp_ms(ended_us));
}

// User finals have no response ID. Give each final to at most one reply;
// subsequent replies still get network and playback timings.
static turn_stamps_t take_transcript(int64_t at)
{
    const turn_stamps_t stamps = recently(pending_transcript.final_us, at)
                                ? pending_transcript : (turn_stamps_t){0};
    pending_transcript = (turn_stamps_t){0};
    return stamps;
}

void voice_timing_ws_event(agent_link_platform_voice_timing_event_t event, int64_t received_us)
{
    const char *stage = NULL;
    portENTER_CRITICAL(&timing_lock);
    if (tracking && received_us > 0) {
        switch (event) {
        case AGENT_LINK_VOICE_USER_FINAL: {
            const int64_t speech_end = mic.confirmed_end_us > last_final_us &&
                                       recently(mic.confirmed_end_us, received_us) ? mic.confirmed_end_us : 0;
            pending_transcript = (turn_stamps_t){
                .turn = ++turn_sequence, .speech_end_us = speech_end, .final_us = received_us,
            };
            mic.confirmed_end_us = 0;
            if (!speech_end && !mic.final_during_silence && mic.speaking && mic.quiet_since_us &&
                mic.last_voiced_us > last_final_us && received_us >= mic.quiet_since_us &&
                received_us - mic.quiet_since_us <= SPEECH_SILENCE_US) {
                mic.final_during_silence = pending_transcript.turn;
            }
            last_final_us = received_us;
            stage = "User final transcript";
            break;
        }
        case AGENT_LINK_VOICE_RESPONSE_STARTED:
            response = take_transcript(received_us);
            response.response_us = received_us;
            stage = "Server response started";
            break;
        case AGENT_LINK_VOICE_FIRST_AUDIO:
            if (!response.response_us && !response.audio_us) response = take_transcript(received_us);
            response.audio_us = received_us;
            stage = "First WSS audio received";
            break;
        }
    }
    portEXIT_CRITICAL(&timing_lock);
    if (stage) ESP_LOGI(TAG, "%s: at_ms=%lld", stage, timestamp_ms(received_us));
}

void voice_timing_first_pcm(unsigned reply_id)
{
    portENTER_CRITICAL(&timing_lock);
    if (tracking) {
        records[reply_id % REPLY_RECORDS] = (reply_record_t){.reply_id = reply_id, .stamps = response};
    }
    portEXIT_CRITICAL(&timing_lock);
}

void voice_timing_reply_end(void)
{
    portENTER_CRITICAL(&timing_lock);
    response = (turn_stamps_t){0};
    portEXIT_CRITICAL(&timing_lock);
}

void voice_timing_speaker_start(unsigned reply_id, int64_t submitted_us)
{
    turn_stamps_t stamps = {0};
    portENTER_CRITICAL(&timing_lock);
    reply_record_t *slot = &records[reply_id % REPLY_RECORDS];
    const bool valid = tracking && slot->reply_id == reply_id;
    if (valid) {
        stamps = slot->stamps;
        slot->reply_id = 0;
    }
    portEXIT_CRITICAL(&timing_lock);
    if (!valid) return;

    ESP_LOGI(TAG, "Reply #%u timestamps_ms: speech_end=%lld transcript_final=%lld response_started=%lld first_audio=%lld speaker_i2s=%lld",
             reply_id, timestamp_ms(stamps.speech_end_us), timestamp_ms(stamps.final_us),
             timestamp_ms(stamps.response_us), timestamp_ms(stamps.audio_us), timestamp_ms(submitted_us));
    ESP_LOGI(TAG, "Reply #%u latency_ms: speech_to_final=%lld final_to_response=%lld response_to_audio=%lld "
                  "audio_to_speaker=%lld speech_to_speaker=%lld",
             reply_id, duration_ms(stamps.speech_end_us, stamps.final_us),
             duration_ms(stamps.final_us, stamps.response_us), duration_ms(stamps.response_us, stamps.audio_us),
             duration_ms(stamps.audio_us, submitted_us), duration_ms(stamps.speech_end_us, submitted_us));
}
