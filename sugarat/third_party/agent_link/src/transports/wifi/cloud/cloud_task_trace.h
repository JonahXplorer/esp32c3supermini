#pragma once

#include <string_view>

#include "cJSON.h"

namespace agentlink::task_trace {

inline constexpr char kInterrupted[] = "response.interrupted";
inline constexpr std::string_view kEventPrefixes[] = {"task.", "tool."};
inline constexpr std::string_view kEvents[] = {
    "session.ready", "session.ended", "session.error", "error",
    "response.started", kInterrupted,
};

inline const char* String(const cJSON* json, const char* field) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(json, field);
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

inline const char* Identifier(const char* value) {
    if (!value || !value[0]) return "-";
    const std::string_view id(value);
    constexpr std::string_view allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:[]";
    return id.size() <= 96 && id.find_first_not_of(allowed) == id.npos ? value : "<redacted>";
}

inline int Count(const cJSON* json, const char* field) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(json, field);
    return cJSON_IsNumber(value) ? value->valueint : -1;
}

inline bool MayContainTraceEvent(std::string_view message) {
    for (const auto prefix : kEventPrefixes)
        if (message.find(prefix) != message.npos) return true;
    for (const auto name : kEvents)
        if (message.find(name) != message.npos) return true;
    return false;
}

inline bool IsTraceEvent(std::string_view event) {
    for (const auto prefix : kEventPrefixes)
        if (event.substr(0, prefix.size()) == prefix) return true;
    for (const auto name : kEvents)
        if (event == name) return true;
    return false;
}

}  // namespace agentlink::task_trace
