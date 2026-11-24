#pragma once

#include <cstring>
#include "agent_link_platform.h"
#include "cJSON.h"

namespace agentlink::realtime {

struct VoiceFrameCounts {
    unsigned classified = 0, speech = 0;

    void Add(int verdict) {
        if (verdict >= 0) ++classified;
        if (verdict > 0) ++speech;
    }
};

inline agent_link_platform_voice_trace_kind_t CloudVoiceEvent(const char* event, const cJSON* json) {
    if (!std::strcmp(event, "input_audio.activity")) {
        const auto* kind = cJSON_GetObjectItemCaseSensitive(json, "kind");
        if (cJSON_IsString(kind)) {
            if (!std::strcmp(kind->valuestring, "start")) return AGENT_LINK_VOICE_TRACE_SPEECH_START;
            if (!std::strcmp(kind->valuestring, "end")) return AGENT_LINK_VOICE_TRACE_SPEECH_END;
        }
    }
    constexpr const char* starts[] = {
        "speech.started", "vad.start", "input_audio.speech_started", "input_audio_buffer.speech_started",
    };
    constexpr const char* ends[] = {
        "speech.ended", "speech.stopped", "vad.end", "input_audio.speech_ended",
        "input_audio.speech_stopped", "input_audio_buffer.speech_stopped",
    };
    for (const auto* name : starts)
        if (!std::strcmp(event, name)) return AGENT_LINK_VOICE_TRACE_SPEECH_START;
    for (const auto* name : ends)
        if (!std::strcmp(event, name)) return AGENT_LINK_VOICE_TRACE_SPEECH_END;
    return AGENT_LINK_VOICE_TRACE_OTHER;
}

}  // namespace agentlink::realtime
