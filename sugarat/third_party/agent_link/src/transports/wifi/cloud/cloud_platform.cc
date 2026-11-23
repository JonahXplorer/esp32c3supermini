#include "agent_link_platform.h"
#include "cloud_platform.h"
#include "cloud_limits.h"
#include "cloud_log.h"
#include "cloud_memory.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "cJSON.h"
#include "cloud_credential.h"
#include "cloud_tools.h"
#include "device_identity.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace {
constexpr const char* TAG = "agent_link.platform";
using agentlink::cloud_limits::kListBytes;
using agentlink::cloud_limits::kListPageSize;
constexpr size_t kSmallBytes = 8 * 1024;
char s_base[160] = {};
std::mutex s_agent_mutex;
agent_link_platform_agent_t s_bound_agent = {};
char s_bound_key[CLOUD_AUTH_KEY_MAX] = {};

bool SafeId(const char* id) {
    if (!id || !*id || strlen(id) > 79) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

const char* String(const cJSON* object, const char* key) {
    const cJSON* item = Field(object, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

const cJSON* Data(const cJSON* root) {
    const cJSON* data = Field(root, "data");
    return data ? data : root;
}

const cJSON* Items(const cJSON* data) {
    if (cJSON_IsArray(data)) return data;
    const cJSON* items = Field(data, "items");
    if (!cJSON_IsArray(items)) items = Field(data, "list");
    if (!cJSON_IsArray(items)) items = Field(data, "records");
    return items;
}

void Copy(char* out, size_t cap, const char* value) {
    if (out && cap) snprintf(out, cap, "%s", value ? value : "");
}

struct Response {
    char* buffer;
    size_t capacity;
    size_t size;
    bool overflow;
};

esp_err_t HttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data) return ESP_OK;
    auto* response = static_cast<Response*>(event->user_data);
    if (event->data_len < 0 || response->size + static_cast<size_t>(event->data_len) >= response->capacity) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->buffer + response->size, event->data, event->data_len);
    response->size += event->data_len;
    response->buffer[response->size] = '\0';
    return ESP_OK;
}

esp_err_t Exchange(const char* operation, esp_http_client_method_t method, const char* path, const char* body,
                   size_t capacity, cJSON** root_out, int* status_out) {
    agentlink::cloud::NetworkMemoryLock memory;
    if (root_out) *root_out = nullptr;
    if (status_out) *status_out = 0;
    const char* device_key = cloud_cred_auth_key();
    if (!s_base[0] || !device_key[0] || !path || !root_out)
        return ESP_ERR_INVALID_STATE;

    char url[800];
    const int url_len = snprintf(url, sizeof url, "%s%s", s_base, path);
    if (url_len <= 0 || url_len >= static_cast<int>(sizeof url)) return ESP_ERR_INVALID_SIZE;
    auto* buffer = static_cast<char*>(heap_caps_calloc(capacity, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) buffer = static_cast<char*>(calloc(capacity, 1));
    if (!buffer) return ESP_ERR_NO_MEM;
    Response response{buffer, capacity, 0, false};
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = method;
    config.timeout_ms = 12000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEvent;
    config.user_data = &response;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(buffer); return ESP_ERR_NO_MEM; }

    esp_err_t result = esp_http_client_set_header(client, "X-Device-Auth-Key", device_key);
    if (result == ESP_OK && body) {
        result = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (result == ESP_OK) result = esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
    }
    if (result == ESP_OK) result = esp_http_client_perform(client);
    const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    if (status_out) *status_out = status;
    esp_http_client_cleanup(client);
    if (result == ESP_OK && response.overflow) result = ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK && response.size) {
        cJSON* root = cJSON_ParseWithLength(buffer, response.size);
        if (!root) result = ESP_ERR_INVALID_RESPONSE;
        else {
            const cJSON* success = Field(root, "success");
            const cJSON* code = Field(root, "code");
            if ((cJSON_IsBool(success) && !cJSON_IsTrue(success)) ||
                (cJSON_IsNumber(code) && code->valueint != 0) || status < 200 || status >= 300) {
                char message[192];
                cloud_log_safe_message(message, sizeof message, String(root, "message"), device_key);
                ESP_LOGW(TAG, "%s: HTTP %d, code=%d, message=%s", operation, status,
                         cJSON_IsNumber(code) ? code->valueint : -1, message);
                result = ESP_ERR_INVALID_RESPONSE;
                cJSON_Delete(root);
            } else *root_out = root;
        }
    } else if (result == ESP_OK && method != HTTP_METHOD_DELETE) result = ESP_ERR_INVALID_RESPONSE;
    if (result == ESP_OK && (status < 200 || status >= 300)) result = ESP_ERR_INVALID_RESPONSE;
    if (result != ESP_OK) ESP_LOGW(TAG, "%s: HTTP %d, %s", operation, status, esp_err_to_name(result));
    memset(buffer, 0, response.size);
    free(buffer);
    return result;
}

const char* GrantString(const cJSON* data, const char* a, const char* b, const char* c) {
    const char* value = String(data, a);
    if (!value) value = String(data, b);
    if (!value) value = String(data, c);
    return value;
}

} // namespace

void cloud_platform_set_base_url(const char* base_url) {
    Copy(s_base, sizeof s_base, base_url);
    size_t len = strlen(s_base);
    while (len && s_base[len - 1] == '/') s_base[--len] = '\0';
}

bool cloud_platform_enabled(void) { return s_base[0] && cloud_cred_is_bound(); }
const char* cloud_platform_base_url(void) { return s_base; }

extern "C" bool agent_link_platform_available(void) { return cloud_platform_enabled(); }

void cloud_platform_set_bound_agent(const cJSON* auth_data) {
    agent_link_platform_agent_t agent = {};
    const cJSON* object = Field(auth_data, "agent");
    const cJSON* id = Field(object, "id");
    if (cJSON_IsNumber(id) && id->valueint > 0 && id->valuedouble == id->valueint) {
        agent.id = id->valueint;
        Copy(agent.name, sizeof agent.name, String(object, "name"));
        agent.voice_supported = cJSON_IsObject(Field(Field(object, "capabilities"), "realtime_voice"));
    }
    std::lock_guard<std::mutex> lock(s_agent_mutex);
    s_bound_agent = agent;
    Copy(s_bound_key, sizeof s_bound_key, cloud_cred_auth_key());
}

extern "C" esp_err_t agent_link_platform_bound_agent(agent_link_platform_agent_t* agent) {
    if (!agent) return ESP_ERR_INVALID_ARG;
    *agent = {};
    std::lock_guard<std::mutex> lock(s_agent_mutex);
    const char* key = cloud_cred_auth_key();
    if (!s_base[0] || !key[0] || strcmp(s_bound_key, key) != 0) return ESP_ERR_INVALID_STATE;
    if (!s_bound_agent.id) return ESP_ERR_NOT_FOUND;
    *agent = s_bound_agent;
    return ESP_OK;
}

extern "C" esp_err_t agent_link_platform_agents(int, agent_link_platform_agent_t*, size_t,
                                               size_t* count, int* total) {
    if (count) *count = 0;
    if (total) *total = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t agent_link_platform_bind_agent(int) { return ESP_ERR_NOT_SUPPORTED; }

namespace {
bool BoundAgent(int id) {
    agent_link_platform_agent_t agent;
    return agent_link_platform_bound_agent(&agent) == ESP_OK && agent.id == id;
}

// The application voice worker owns tool registration and the cached PUT body.
struct ToolRegistration {
    int agent_id;
    char session_id[80];
    char name[65];
    char* json;
};
ToolRegistration s_registration{};

const cJSON* FindTool(const cJSON* tools, const char* name) {
    if (!cJSON_IsArray(tools)) return nullptr;
    for (const cJSON* item = tools->child; item; item = item->next) {
        const char* item_name = String(item, "name");
        if (item_name && strcmp(item_name, name) == 0) return item;
    }
    return nullptr;
}

bool MatchToolTopics(const char* down, const char* up, const char* scope, const char* id) {
    char expected_down[128], expected_up[128];
    snprintf(expected_down, sizeof expected_down, "%s/%s/tool/down", scope, id);
    snprintf(expected_up, sizeof expected_up, "%s/%s/tool/up", scope, id);
    return strcmp(down, expected_down) == 0 && strcmp(up, expected_up) == 0;
}

bool ValidToolTopics(const char* down, const char* up, const char* sn, const char* session_id) {
    return down && up && (MatchToolTopics(down, up, "device", sn) || MatchToolTopics(down, up, "session", session_id));
}

esp_err_t FetchToolCredentials(const char* tools_path, cJSON** root) {
    char path[224];
    snprintf(path, sizeof path, "%s/mqtt-config", tools_path);
    return Exchange("issue device tool MQTT credentials", HTTP_METHOD_POST, path, nullptr, kSmallBytes, root, nullptr);
}

esp_err_t StartToolClient(const cJSON* root, const char* sn, const char* session_id, const char** client_id) {
    const cJSON* mqtt = Field(Data(root), "mqtt");
    const cJSON* channels = Field(mqtt, "tool_channels");
    if (!cJSON_IsObject(channels)) channels = Field(mqtt, "channels");
    const cJSON* port = Field(mqtt, "broker_port");
    const cJSON* ttl = Field(mqtt, "expires_in");
    const CloudToolCredentials credentials{
        .protocol = String(mqtt, "protocol"), .host = String(mqtt, "broker_url"), .path = String(mqtt, "path"),
        .client_id = String(mqtt, "client_id"), .username = String(mqtt, "username"), .password = String(mqtt, "password"),
        .down = String(channels, "down"), .up = String(channels, "up"),
        .port = cJSON_IsNumber(port) ? port->valueint : 0,
        .expires_in = cJSON_IsNumber(ttl) && ttl->valueint > 0 ? static_cast<uint32_t>(ttl->valueint) : 0,
    };
    if (!credentials.protocol || !credentials.host || !credentials.host[0] ||
        !credentials.client_id || !credentials.client_id[0] || !credentials.username || !credentials.password ||
        !credentials.password[0] || !credentials.expires_in || credentials.port < 0 || credentials.port > 65535 ||
        !ValidToolTopics(credentials.down, credentials.up, sn, session_id)) {
        ESP_LOGW(TAG, "invalid tool MQTT config (protocol=%u host=%u credentials=%u tool_topics=%u ttl=%u)",
                 credentials.protocol != nullptr, credentials.host != nullptr,
                 credentials.client_id && credentials.username && credentials.password,
                 ValidToolTopics(credentials.down, credentials.up, sn, session_id),
                 static_cast<unsigned>(credentials.expires_in));
        return ESP_ERR_INVALID_RESPONSE;
    }
    *client_id = credentials.client_id;
    return cloud_tools_start(credentials);
}

esp_err_t PutToolRegistration(const ToolRegistration& registration, const char* stage) {
    if (!BoundAgent(registration.agent_id)) return ESP_ERR_INVALID_STATE;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/conversations/%s/tools", registration.agent_id,
             registration.session_id);
    cJSON* response = nullptr;
    const esp_err_t err = Exchange(stage, HTTP_METHOD_PUT, path, registration.json, kListBytes, &response, nullptr);
    if (err != ESP_OK) return err;
    const cJSON* data = Data(response);
    const char* sn = dev_identity_sn();
    const char* down = String(data, "down_topic");
    const char* up = String(data, "up_topic");
    const char* returned_id = String(data, "session_id");
    const bool same_session = (!returned_id || strcmp(returned_id, registration.session_id) == 0) &&
        ValidToolTopics(down, up, sn, registration.session_id) && cloud_tools_topics_match(down, up);
    const cJSON* installed = FindTool(Field(data, "tools"), registration.name);
    const bool active = BoundAgent(registration.agent_id) && cJSON_IsTrue(Field(data, "connected"));
    const cJSON* generation = Field(data, "generation");
    ESP_LOGI(TAG, "%s: agent=%d tool=%s active=%d session_match=%d generation=%d",
             stage, registration.agent_id, installed ? registration.name : "missing", active, same_session,
             cJSON_IsNumber(generation) ? generation->valueint : -1);
    cJSON_Delete(response);
    return active && same_session && installed ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
bool EncodeCursor(const char* value, char* out, size_t capacity) {
    constexpr char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        const bool plain = std::isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (used + (plain ? 1u : 3u) >= capacity) return false;
        if (plain) out[used++] = static_cast<char>(*p);
        else { out[used++] = '%'; out[used++] = hex[*p >> 4]; out[used++] = hex[*p & 15]; }
    }
    out[used] = '\0';
    return true;
}
}  // namespace

extern "C" esp_err_t agent_link_platform_conversations(int agent_id, size_t offset,
                                                           agent_link_platform_conversation_t* entries,
                                                           size_t capacity, size_t* count, size_t* total) {
    if (agent_id <= 0 || !entries || !count || !total || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0; *total = 0;
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    char cursor[160] = {};
    size_t skipped = 0;
    for (unsigned page = 0; page < 128; ++page) {
        char encoded[480] = {}, path[640];
        if (!EncodeCursor(cursor, encoded, sizeof encoded)) return ESP_ERR_INVALID_SIZE;
        const int length = snprintf(path, sizeof path,
            "/api/agents/%d/actions/realtime_voice/conversations?limit=%u%s%s",
            agent_id, kListPageSize, cursor[0] ? "&cursor=" : "", encoded);
        if (length < 0 || length >= static_cast<int>(sizeof path)) return ESP_ERR_INVALID_SIZE;
        cJSON* root = nullptr;
        esp_err_t result = Exchange("list conversations", HTTP_METHOD_GET, path, nullptr,
                                    kListBytes, &root, nullptr);
        if (result != ESP_OK) return result;
        const cJSON* data = Data(root);
        const cJSON* rows = Items(data);
        if (!cJSON_IsArray(rows) || !BoundAgent(agent_id)) {
            cJSON_Delete(root);
            *count = 0;
            return ESP_ERR_INVALID_RESPONSE;
        }
        const int n = cJSON_GetArraySize(rows);
        for (int i = 0; i < n; ++i) {
            const cJSON* row = cJSON_GetArrayItem(rows, i);
            const char* id = String(row, "conversation_id");
            if (!id) id = String(row, "session_id");
            if (!SafeId(id)) continue;
            if (skipped++ < offset) continue;
            if (*count >= capacity) continue;
            auto& entry = entries[(*count)++];
            Copy(entry.id, sizeof entry.id, id);
            Copy(entry.name, sizeof entry.name, String(row, "name"));
            Copy(entry.created_at, sizeof entry.created_at, String(row, "created_at"));
        }
        const bool more = cJSON_IsTrue(Field(data, "has_more"));
        const char* next = String(data, "next_cursor");
        if (!more || *count >= capacity) {
            *total = skipped + (more ? 1u : 0u);
            cJSON_Delete(root);
            return ESP_OK;
        }
        if (!n || !next || !next[0] || strlen(next) >= sizeof cursor || strcmp(next, cursor) == 0) {
            cJSON_Delete(root);
            *count = 0;
            return ESP_ERR_INVALID_RESPONSE;
        }
        Copy(cursor, sizeof cursor, next);
        cJSON_Delete(root);
    }
    *count = 0;
    return ESP_ERR_INVALID_SIZE;
}

extern "C" esp_err_t agent_link_platform_create_conversation(int agent_id, char* id, size_t id_size) {
    if (agent_id <= 0 || !id || id_size < 40) return ESP_ERR_INVALID_ARG;
    id[0] = '\0';
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    char path[160];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/conversations", agent_id);
    cJSON* root = nullptr;
    esp_err_t result = Exchange("create conversation", HTTP_METHOD_POST, path, nullptr, kSmallBytes,
                                &root, nullptr);
    if (result != ESP_OK) return result;
    if (!BoundAgent(agent_id)) { cJSON_Delete(root); return ESP_ERR_INVALID_STATE; }
    const cJSON* data = Data(root);
    const char* session = String(data, "conversation_id");
    if (!session) session = String(data, "session_id");
    if (!session) session = String(Field(data, "conversation"), "conversation_id");
    if (SafeId(session) && strlen(session) < id_size) Copy(id, id_size, session);
    cJSON_Delete(root);
    return result;
}

extern "C" esp_err_t agent_link_platform_delete_conversation(int agent_id,
                                                                 const char* conversation_id) {
    if (agent_id <= 0 || !SafeId(conversation_id)) return ESP_ERR_INVALID_ARG;
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/conversations/%s",
             agent_id, conversation_id);
    cJSON* root = nullptr;
    const esp_err_t result = Exchange("delete conversation", HTTP_METHOD_DELETE, path, nullptr,
                                      kSmallBytes, &root, nullptr);
    cJSON_Delete(root);
    return result;
}

extern "C" esp_err_t agent_link_platform_attach_tool(int agent_id, const char* session_id,
                                                       const char* definition_json) {
    if (agent_id <= 0 || !SafeId(session_id) || !definition_json) return ESP_ERR_INVALID_ARG;
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    const char* sn = dev_identity_sn();
    if (!SafeId(sn)) return ESP_ERR_INVALID_STATE;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/conversations/%s/tools", agent_id, session_id);
    cJSON* status = nullptr;
    esp_err_t err = Exchange("get session tools", HTTP_METHOD_GET, path, nullptr, kListBytes, &status, nullptr);
    if (err != ESP_OK) return err;
    const cJSON* data = Data(status);
    const bool active = cJSON_IsTrue(Field(data, "connected"));
    const cJSON* current = Field(data, "tools");
    if (!cJSON_IsArray(current)) {
        cJSON_Delete(status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* definition = cJSON_Parse(definition_json);
    const char* name = String(definition, "name");
    if (!name || !*name || strlen(name) >= sizeof(ToolRegistration::name) ||
        !cJSON_IsObject(Field(definition, "parameters"))) {
        cJSON_Delete(definition);
        cJSON_Delete(status);
        return ESP_ERR_INVALID_ARG;
    }
    int index = 0;
    const cJSON* item = current->child;
    for (; item; item = item->next, ++index) {
        const char* item_name = String(item, "name");
        if (item_name && strcmp(item_name, name) == 0) break;
    }
    if (!BoundAgent(agent_id)) {
        cJSON_Delete(definition);
        cJSON_Delete(status);
        return ESP_ERR_INVALID_STATE;
    }
    if (active && (!item ||
        !ValidToolTopics(String(data, "down_topic"), String(data, "up_topic"), sn, session_id))) {
        ESP_LOGW(TAG, "existing session tool channel reserved (registered tools=%d, requested tool present=%d)",
                 cJSON_GetArraySize(current), item != nullptr);
        cJSON_Delete(definition);
        cJSON_Delete(status);
        return ESP_ERR_NOT_ALLOWED;
    }
    ToolRegistration registration{.agent_id = agent_id};
    Copy(registration.session_id, sizeof registration.session_id, session_id);
    Copy(registration.name, sizeof registration.name, name);
    const bool replace = item != nullptr;
    cJSON* tools = cJSON_Duplicate(current, true);
    cJSON_Delete(status);
    const bool inserted = tools && (replace ? cJSON_ReplaceItemInArray(tools, index, definition)
                                            : cJSON_AddItemToArray(tools, definition));
    if (!inserted) cJSON_Delete(definition);
    cJSON* body = cJSON_CreateObject();
    const bool owns_tools = inserted && cJSON_AddItemToObject(body, "tools", tools);
    if (!owns_tools) cJSON_Delete(tools);
    if (!owns_tools) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    cJSON* credentials = nullptr;
    const char* mqtt_client_id = nullptr;
    err = FetchToolCredentials(path, &credentials);
    if (err == ESP_OK) err = StartToolClient(credentials, sn, session_id, &mqtt_client_id);
    char* json = nullptr;
    if (err == ESP_OK && cJSON_AddStringToObject(body, "mqtt_client_id", mqtt_client_id)) json = cJSON_PrintUnformatted(body);
    cJSON_Delete(credentials);
    cJSON_Delete(body);
    if (err != ESP_OK) return err;
    if (!json) {
        agent_link_platform_detach_tool();
        return ESP_ERR_NO_MEM;
    }

    registration.json = json;
    err = PutToolRegistration(registration, "mount voice tool");
    if (err == ESP_OK) {
        cJSON_free(s_registration.json);
        s_registration = registration;
        cloud_tools_enable();
    } else {
        cJSON_free(json);
        agent_link_platform_detach_tool();
    }
    return err;
}

extern "C" esp_err_t agent_link_platform_refresh_tool(int agent_id, const char* session_id) {
    if (!session_id || !s_registration.json || s_registration.agent_id != agent_id ||
        strcmp(s_registration.session_id, session_id) != 0 || !agent_link_platform_tool_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return PutToolRegistration(s_registration, "hot-update voice tool");
}

esp_err_t cloud_platform_grant(int agent_id, const char* session_id, bool refresh,
                               char* url, size_t url_size, char* token, size_t token_size,
                               agentlink::realtime::AudioFormat* audio, int* http_status) {
    if (http_status) *http_status = 0;
    if (agent_id <= 0 || !SafeId(session_id) || !url || !token || !audio) return ESP_ERR_INVALID_ARG;
    if (!BoundAgent(agent_id)) return ESP_ERR_INVALID_STATE;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/%s", agent_id,
             refresh ? "realtime-refresh" : "realtime-grant");
    char body[512];
#if CONFIG_AGENT_LINK_REALTIME_OPUS
    snprintf(body, sizeof body, "{\"session_id\":\"%s\",\"audio_config\":%s}", session_id,
             agentlink::realtime::kOpusAudioConfig);
#else
    snprintf(body, sizeof body, "{\"session_id\":\"%s\",\"audio_config\":%s}", session_id,
             agentlink::realtime::kPcmAudioConfig);
#endif
    cJSON* root = nullptr;
    esp_err_t result = Exchange(refresh ? "refresh voice grant" : "request voice grant",
                                HTTP_METHOD_POST, path, body, kSmallBytes, &root, http_status);
    if (result != ESP_OK) return result;
    if (!BoundAgent(agent_id)) { cJSON_Delete(root); return ESP_ERR_INVALID_STATE; }
    const cJSON* data = Data(root);
    const char* granted_session = String(data, "session_id");
    const bool same_session = !granted_session || strcmp(granted_session, session_id) == 0;
    ESP_LOGI(TAG, "Voice grant conversation: agent=%d session_match=%s", agent_id,
             !granted_session ? "not_reported" : same_session ? "yes" : "no");
    if (!same_session) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* grant = Field(data, "grant");
    if (cJSON_IsObject(grant)) data = grant;
    const char* ws = GrantString(data, "websocket_url", "websocketUrl", "ws_url");
    const char* access = GrantString(data, "connect_token", "connectToken", "token");
    if (!ws || !access || strlen(ws) >= url_size || strlen(access) >= token_size ||
        strncmp(ws, "wss://", 6) != 0) result = ESP_ERR_INVALID_RESPONSE;
    else {
        Copy(url, url_size, ws);
        Copy(token, token_size, access);
        if (!agentlink::realtime::ReadAudioFormat(Data(root), audio, true)) {
            result = ESP_ERR_NOT_SUPPORTED;
        }
#if !CONFIG_AGENT_LINK_REALTIME_OPUS
        if (audio->input_opus || audio->output_opus) result = ESP_ERR_NOT_SUPPORTED;
#else
        if (result == ESP_OK && (!audio->input_opus || !audio->output_opus))
            ESP_LOGD(TAG, "grant metadata reports PCM16; awaiting session.ready audio settings");
#endif
    }
    cJSON_Delete(root);
    return result;
}

void cloud_platform_end_grant(int agent_id, const char* session_id) {
    if (agent_id <= 0 || !SafeId(session_id)) return;
    char path[192];
    snprintf(path, sizeof path, "/api/agents/%d/actions/realtime_voice/realtime-grant/%s",
             agent_id, session_id);
    cJSON* root = nullptr;
    (void)Exchange("end voice grant", HTTP_METHOD_DELETE, path, nullptr, kSmallBytes, &root,
                   nullptr);
    cJSON_Delete(root);
}
