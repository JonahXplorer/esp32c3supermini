#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int id;
    char name[80];
    bool voice_supported;
} agent_link_platform_agent_t;

typedef struct {
    char id[80];
    char name[96];
    char created_at[32];
} agent_link_platform_conversation_t;

// These calls use the activated device auth_key and perform HTTPS I/O.
// Call them from a worker, never from an LVGL callback.
bool agent_link_platform_available(void);
// Cached from device-gateway/auth; this does not fetch the account's agent list.
esp_err_t agent_link_platform_bound_agent(agent_link_platform_agent_t* agent);
// Legacy account-management APIs are unsupported on devices.
esp_err_t agent_link_platform_agents(int page, agent_link_platform_agent_t* entries,
                                     size_t capacity, size_t* count, int* total);
esp_err_t agent_link_platform_bind_agent(int agent_id);
esp_err_t agent_link_platform_conversations(int agent_id, size_t offset,
                                            agent_link_platform_conversation_t* entries,
                                            size_t capacity, size_t* count, size_t* total);
esp_err_t agent_link_platform_create_conversation(int agent_id, char* id, size_t id_size);
esp_err_t agent_link_platform_delete_conversation(int agent_id, const char* conversation_id);
// After cancelling realtime, wait for transport/grant cleanup before deleting its conversation.
esp_err_t agent_link_platform_wait_realtime_idle(uint32_t timeout_ms);

// Asynchronously request one grant and connect the existing realtime voice worker.
esp_err_t agent_link_request_realtime_session(int agent_id, const char* session_id);
// Maintain an unused HTTPS grant for the idle conversation. Call periodically
// while idle. Only agent_link_request_realtime_session() opens the WebSocket.
esp_err_t agent_link_prepare_realtime_session(int agent_id, const char* session_id);

struct cJSON;
typedef esp_err_t (*agent_link_platform_tool_cb_t)(const char* action, const struct cJSON* params,
                                                    struct cJSON* result, void* ctx);
// Install before agent_link_start(). The callback runs on the MQTT task.
void agent_link_platform_set_tool_callback(agent_link_platform_tool_cb_t callback, void* ctx);
// Install before agent_link_start(). Invoked on the voice worker for final user speech.
typedef void (*agent_link_platform_user_transcript_cb_t)(const char* text);
void agent_link_platform_set_user_transcript_callback(agent_link_platform_user_transcript_cb_t callback);
typedef enum {
    AGENT_LINK_VOICE_USER_FINAL,
    AGENT_LINK_VOICE_RESPONSE_STARTED,
    AGENT_LINK_VOICE_FIRST_AUDIO,
} agent_link_platform_voice_timing_event_t;
// Install before agent_link_start(). Timestamps are local WSS receive times from esp_timer_get_time().
typedef void (*agent_link_platform_voice_timing_cb_t)(agent_link_platform_voice_timing_event_t event, int64_t received_us);
void agent_link_platform_set_voice_timing_callback(agent_link_platform_voice_timing_cb_t callback);

typedef enum {
    AGENT_LINK_VOICE_TRACE_UPLINK,
    AGENT_LINK_VOICE_TRACE_SPEECH_START,
    AGENT_LINK_VOICE_TRACE_SPEECH_END,
    AGENT_LINK_VOICE_TRACE_PARTIAL,
    AGENT_LINK_VOICE_TRACE_FINAL,
    AGENT_LINK_VOICE_TRACE_RESPONSE_START,
    AGENT_LINK_VOICE_TRACE_FIRST_AUDIO,
    AGENT_LINK_VOICE_TRACE_OTHER,
    AGENT_LINK_VOICE_TRACE_RESPONSE_INTERRUPTED,
} agent_link_platform_voice_trace_kind_t;

typedef struct {
    agent_link_platform_voice_trace_kind_t kind;
    int64_t at_us;
    uint32_t stream;
    unsigned last_frame;
    unsigned frames, classified_frames, speech_frames;
    const char* event;
    const char* text;
    uint32_t reset_seq;
} agent_link_platform_voice_trace_t;

// Optional diagnostics on S3. The detector returns -1 for unknown, 0 for silence,
// or 1 for speech. Callbacks run on audio/network workers; strings are borrowed.
typedef int (*agent_link_platform_voice_detector_t)(const uint8_t* pcm, size_t bytes);
typedef void (*agent_link_platform_voice_trace_cb_t)(const agent_link_platform_voice_trace_t* trace);
void agent_link_platform_set_voice_trace(agent_link_platform_voice_detector_t detector,
                                         agent_link_platform_voice_trace_cb_t callback);
// Attach a tool object or array of tools to the bound conversation, preserving other tools.
esp_err_t agent_link_platform_attach_tool(int agent_id, const char* session_id, const char* definition_json);
esp_err_t agent_link_platform_refresh_tool(int agent_id, const char* session_id);
bool agent_link_platform_tool_ready(void);
void agent_link_platform_detach_tool(void);

#ifdef __cplusplus
}
#endif
