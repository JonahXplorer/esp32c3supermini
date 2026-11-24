#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "realtime_format.h"

void cloud_platform_set_base_url(const char* base_url);
const char* cloud_platform_base_url(void);
bool cloud_platform_enabled(void);
void cloud_platform_set_bound_agent(const cJSON* auth_data);
void cloud_platform_drop_tools_on_network_loss();
inline constexpr int kVoiceGrantSessionEnded = 1002;
esp_err_t cloud_platform_grant(int agent_id, const char* session_id, bool refresh,
                               char* url, size_t url_size, char* token, size_t token_size,
                               agentlink::realtime::AudioFormat* audio, int* http_status,
                               int64_t* expires_us = nullptr,
                               const agentlink::realtime::AudioFormat* previous_audio = nullptr,
                               int* business_code = nullptr);
enum class CloudGrantCleanup { kNormal, kBeforeRetry };
void cloud_platform_end_grant(int agent_id, const char* session_id,
                              CloudGrantCleanup cleanup = CloudGrantCleanup::kNormal);
