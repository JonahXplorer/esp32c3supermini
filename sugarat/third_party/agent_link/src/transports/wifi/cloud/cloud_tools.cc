#include "cloud_tools.h"
#include "wifi_cloud_recovery.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "agent_link_platform.h"
#include "cJSON.h"
#include "cloud_clock.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

namespace {
constexpr const char* TAG = "agent_link.tools";
agent_link_platform_tool_cb_t s_callback = nullptr;
void* s_context = nullptr;
esp_mqtt_client_handle_t s_client = nullptr;
std::atomic<bool> s_subscribed{false}, s_enabled{false}, s_rejected{false};
char s_down[128], s_up[128];
int64_t s_renew_at_us = 0;

struct RecentReply {
    char request_id[64];
    char data[640];
    int code;
};
EXT_RAM_BSS_ATTR RecentReply s_recent[8]{};  // MQTT QoS 1 can redeliver an already executed command.
unsigned s_next = 0;

const char* String(const cJSON* item, const char* name) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(item, name);
    return cJSON_IsString(field) ? field->valuestring : nullptr;
}

const char* LogIdentifier(const char* value) {
    static constexpr char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:[]";
    return value && strspn(value, chars) == strlen(value) ? value : "<redacted>";
}

int BrokerPort(const CloudToolCredentials& token, bool wss) {
#ifdef CONFIG_MQTT_WSS_DEFAULT_PORT
    constexpr int wss_port = CONFIG_MQTT_WSS_DEFAULT_PORT;
#else
    constexpr int wss_port = 443;
#endif
#ifdef CONFIG_MQTT_SSL_DEFAULT_PORT
    constexpr int mqtts_port = CONFIG_MQTT_SSL_DEFAULT_PORT;
#else
    constexpr int mqtts_port = 8883;
#endif
    if (token.port) return token.port;
    return wss ? wss_port : mqtts_port;
}

void LogToolRoute(const CloudToolCredentials& token, bool wss, int port) {
    static constexpr char path_chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/-._~%";
    const char* path = wss ? (token.path ? token.path : "/") : "-";
    // WebSocket query strings can carry credentials. Only log the routing path.
    size_t length = strcspn(path, "?#");
    if (length > 128 || strspn(path, path_chars) != length) {
        path = "<redacted>";
        length = strlen(path);
    }
    ESP_LOGI(TAG, "tool MQTT broker: protocol=%s host=%s port=%d ws_path=%.*s",
             token.protocol, LogIdentifier(token.host), port, static_cast<int>(length), path);
    ESP_LOGI(TAG, "tool MQTT topics: down=%s up=%s qos=1", s_down, s_up);
}

void Send(const char* json, const char* request_id, int code, bool cached) {
    const int id = esp_mqtt_client_enqueue(s_client, s_up, json, static_cast<int>(strlen(json)), 1, 0, true);
    if (id < 0) {
        ESP_LOGW(TAG, "tool MQTT response queue failed: topic=%s reply_to=%s code=%d",
                 s_up, LogIdentifier(request_id), code);
    } else {
        ESP_LOGI(TAG, "tool MQTT response queued: topic=%s reply_to=%s code=%d mqtt_id=%d cached=%u",
                 s_up, LogIdentifier(request_id), code, id, static_cast<unsigned>(cached));
    }
}

int ResultCode(esp_err_t result) {
    switch (result) {
        case ESP_OK: return 0;
        case ESP_ERR_INVALID_ARG: return 400;
        case ESP_ERR_NOT_SUPPORTED: return 404;
        case ESP_ERR_INVALID_STATE: return 409;
        case ESP_ERR_TIMEOUT: return 504;
        default: return 500;
    }
}
void Handle(const char* data, size_t size) {
    cJSON* request = cJSON_ParseWithLength(data, size);
    const char* type = String(request, "type");
    const char* action = String(request, "action");
    const char* id = String(request, "msg_id");
    if (!type || strcmp(type, "request") != 0 || !action || !*action || strlen(action) > 64 ||
        !id || !*id || strlen(id) >= sizeof(RecentReply::request_id)) {
        ESP_LOGW(TAG, "tool MQTT request ignored: invalid envelope on topic=%s", s_down);
        cJSON_Delete(request);
        return;
    }
    ESP_LOGI(TAG, "tool MQTT request: topic=%s action=%s msg_id=%s",
             s_down, LogIdentifier(action), LogIdentifier(id));
    for (const auto& prior : s_recent) {
        if (strcmp(id, prior.request_id) == 0) {
            Send(prior.data, prior.request_id, prior.code, true);
            cJSON_Delete(request);
            return;
        }
    }
    cJSON* reply = cJSON_CreateObject();
    cJSON* result = cJSON_AddObjectToObject(reply, "params");
    if (!result) {
        cJSON_Delete(reply);
        cJSON_Delete(request);
        return;
    }
    const auto* args = cJSON_GetObjectItemCaseSensitive(request, "params");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(args)) {
        err = s_callback ? s_callback(action, args, result, s_context) : ESP_ERR_NOT_SUPPORTED;
    }
    const int64_t now = cloud_clock_now_ms();
    char msg_id[40];
    snprintf(msg_id, sizeof msg_id, "msg_%lld_%06x", static_cast<long long>(now),
             static_cast<unsigned>(esp_random() & 0xffffff));
    cJSON_AddStringToObject(reply, "msg_id", msg_id);
    cJSON_AddStringToObject(reply, "type", "response");
    cJSON_AddStringToObject(reply, "action", action);
    cJSON_AddNumberToObject(reply, "timestamp", static_cast<double>(now));
    cJSON_AddStringToObject(reply, "reply_to", id);
    const int code = ResultCode(err);
    cJSON_AddNumberToObject(reply, "code", code);
    cJSON_AddStringToObject(reply, "message", err == ESP_OK ? "success" : esp_err_to_name(err));
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    char* json = cJSON_PrintUnformatted(reply);
    if (json) {
        if (strlen(json) < sizeof(RecentReply::data)) {
            auto& slot = s_recent[s_next++ % (sizeof s_recent / sizeof s_recent[0])];
            snprintf(slot.request_id, sizeof slot.request_id, "%s", id);
            snprintf(slot.data, sizeof slot.data, "%s", json);
            slot.code = code;
        }
        Send(json, id, code, false);
        cJSON_free(json);
    }
    cJSON_Delete(reply);
    cJSON_Delete(request);
}

void OnEvent(void*, esp_event_base_t, int32_t id, void* data) {
    const auto ev = static_cast<esp_mqtt_event_handle_t>(data);
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            s_subscribed.store(false);
            if (esp_mqtt_client_subscribe_single(ev->client, s_down, 1) < 0) s_rejected.store(true);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            if (ev->data_len == 1 && static_cast<uint8_t>(ev->data[0]) < 0x80) {
                s_subscribed.store(true);
                ESP_LOGI(TAG, "device tool subscription ready: topic=%s qos=%u",
                         s_down, static_cast<unsigned>(static_cast<uint8_t>(ev->data[0])));
            } else {
                s_rejected.store(true);
                ESP_LOGW(TAG, "device tool subscription rejected by broker: topic=%s", s_down);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (s_subscribed.exchange(false)) ESP_LOGW(TAG, "tool MQTT connection lost");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "broker acknowledged device tool response: topic=%s mqtt_id=%d", s_up, ev->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                s_rejected.store(true);
                ESP_LOGW(TAG, "tool MQTT login refused (code=%d)", ev->error_handle->connect_return_code);
            }
            break;
        case MQTT_EVENT_DATA: {
            if (ev->current_data_offset != 0) break;
            const bool enabled = s_enabled.load() && s_subscribed.load() && wifi_cloud_requests_allowed();
            const bool expected_topic = ev->topic && ev->topic_len == static_cast<int>(strlen(s_down)) &&
                                        memcmp(ev->topic, s_down, ev->topic_len) == 0;
            const bool complete = ev->data && ev->data_len > 0 && ev->data_len == ev->total_data_len;
            if (enabled && !ev->retain && expected_topic && complete) {
                Handle(ev->data, ev->data_len);
            } else {
                ESP_LOGW(TAG, "tool MQTT packet ignored: expected_topic=%s match=%u enabled=%u retained=%d complete=%u",
                         s_down, static_cast<unsigned>(expected_topic), static_cast<unsigned>(enabled),
                         ev->retain, static_cast<unsigned>(complete));
            }
            break;
        }
        default: break;
    }
}
} // namespace

extern "C" void agent_link_platform_set_tool_callback(agent_link_platform_tool_cb_t callback, void* context) {
    s_context = context;
    s_callback = callback;
}

extern "C" void agent_link_platform_detach_tool() {
    s_enabled.store(false);
    if (s_client) {
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    s_subscribed.store(false);
    s_rejected.store(false);
}

extern "C" bool agent_link_platform_tool_ready() {
    return s_enabled.load() && s_subscribed.load() && esp_timer_get_time() < s_renew_at_us;
}

esp_err_t cloud_tools_start(const CloudToolCredentials& token) {
    const uint32_t network_generation = wifi_cloud_network_generation();
    if (!wifi_cloud_requests_allowed()) return ESP_ERR_INVALID_STATE;
    const bool wss = strcmp(token.protocol, "wss") == 0;
    if (!wss && strcmp(token.protocol, "mqtts") != 0) return ESP_ERR_NOT_SUPPORTED;
    agent_link_platform_detach_tool();
    snprintf(s_down, sizeof s_down, "%s", token.down);
    snprintf(s_up, sizeof s_up, "%s", token.up);
    esp_mqtt_client_config_t config{};
    config.broker.address.transport = wss ? MQTT_TRANSPORT_OVER_WSS : MQTT_TRANSPORT_OVER_SSL;
    config.broker.address.hostname = token.host;
    const int port = BrokerPort(token, wss);
    config.broker.address.port = port;
    if (wss && token.path) config.broker.address.path = token.path;
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.credentials.client_id = token.client_id;
    config.credentials.username = token.username;
    config.credentials.authentication.password = token.password;
    config.session.keepalive = 40;
    config.session.message_retransmit_timeout = 5000;
    config.network.reconnect_timeout_ms = 5000;
    config.network.timeout_ms = 10000;
    config.task.stack_size = 7 * 1024;
    LogToolRoute(token, wss, port);
    s_client = esp_mqtt_client_init(&config);
    if (!s_client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, &OnEvent, nullptr);
    if (err == ESP_OK) err = esp_mqtt_client_start(s_client);
    if (err == ESP_OK) {
        const int64_t deadline = esp_timer_get_time() + 12000000;
        while (!s_subscribed.load() && !s_rejected.load() && esp_timer_get_time() < deadline &&
               network_generation == wifi_cloud_network_generation()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (network_generation != wifi_cloud_network_generation()) err = ESP_ERR_INVALID_STATE;
        else if (!s_subscribed.load()) err = s_rejected.load() ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) agent_link_platform_detach_tool();
    else s_renew_at_us = esp_timer_get_time() + (token.expires_in - token.expires_in / 10) * 1000000LL;
    return err;
}

bool cloud_tools_topics_match(const char* down, const char* up) {
    return strcmp(s_down, down) == 0 && strcmp(s_up, up) == 0;
}

void cloud_tools_enable() { s_enabled.store(true); }
