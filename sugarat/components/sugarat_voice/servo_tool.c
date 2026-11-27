#include "servo_tool.h"

#include <string.h>

#include "agent_link_platform.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "servo_voice_command.h"

enum { ACK_TIMEOUT_MS = 1500, TOOL_WINDOW_MS = 15000, MAX_RECENT_COMMANDS = 16 };

static const char *TAG = "servo_tool";
#define SET_POWER "set_servo_power"
#define GET_STATUS "get_servo_status"
static const char DEFINITIONS[] =
    "[{\"name\":\"" SET_POWER "\","
    "\"description\":\"控制用户现实中的钢铁侠面罩。面罩、面具、脸甲、头盔面板、舵机、faceplate、visor、mask、helmet 和 servo 指同一个开合机构。"
    "用户明确要求实际开合时必须调用本工具。打开、掀开、升起、抬起对应 power=true，目标 B 点。关上、合上、放下、降下对应 power=false，目标 A 点。"
    "例如把脸甲抬起来、能帮我把面罩放下来吗。上一句明确在说面罩时，打开它和合上也可执行。"
    "英文 open/raise/switch on/turn on mask、visor 或 faceplate 去 B 点，close/lower/switch off/turn off 回 A 点。"
    "灯光、头盔电源、闲聊、引用、否定、询问如何操作或仅问状态时不调用。对象或方向不清时先问一句。"
    "即使设备已按转写词执行，也要调用本工具取得同一次动作的 C3 回执。source=transcript 表示由 Sugarat 本地规则发起。"
    "只有 c3_ack=true 且 executed=true 才确认，moving=true 时说正在开合。要核对刚才的执行结果，调用 " GET_STATUS " 并传 last_command_only=true。"
    "如果 c3_ack=false 或 state=unknown，告诉用户暂时无法确认面罩是否到位，继续对话，等待用户明确要求后才能再次操作。"
    "用户说没动或要检查当前是否到位时，调用 " GET_STATUS " 查当前状态，无法核实时如实说明。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"power\":{\"type\":\"boolean\","
    "\"description\":\"布尔值。true 抬起并打开面罩到 B 点，false 放下并关闭面罩到 A 点\"}},\"required\":[\"power\"]},\"timeout\":15},"
    "{\"name\":\"" GET_STATUS "\","
    "\"description\":\"只读查询用户的钢铁侠面罩、面具、脸甲、头盔面板或舵机。用户问现在开着吗、关好了没、到位了吗、刚才为什么没动或 MQTT 指令有没有发到设备时调用。"
    "默认实时询问 C3，不改变位置。state 为 open=已到 B、closed=已到 A、opening=去 B、closing=去 A、unknown=未收到 C3 状态。"
    "要确认设备刚才是否已执行本地转写指令，用 last_command_only=true，等待本次通话最近一次开合的回执，只读返回 last_command，不再发 ESP-NOW。"
    "last_command.source=transcript 表示本地发起，c3_ack=true 且 executed=true 才表示 C3 确认目标。moving 是回执当时的状态，age_ms 是动作距今时间。"
    "last_command=null 或没有回执时，如实说明无法确认并继续对话。查询当前状态时根据实时 C3 结果回答。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"last_command_only\":{\"type\":\"boolean\","
    "\"description\":\"true 读取本次通话最近一次开合的原始回执，默认查询当前状态\"}},\"additionalProperties\":false},\"timeout\":15}]";

typedef enum { SOURCE_TRANSCRIPT = 1, SOURCE_MQTT = 2 } command_source_t;

typedef struct {
    servo_tool_request_t request;
    int64_t match_until_us;
    uint32_t call;
    command_source_t source;
    uint8_t sources;
    bool dispatched;
    bool complete;
    bool esp_now_queued;
    bool c3_ack;
    servo_tool_state_t state;
} servo_command_t;

static StaticSemaphore_t completion_control;
static SemaphoreHandle_t completion;
static servo_command_t commands[MAX_RECENT_COMMANDS];
static servo_command_t last_action;
static uint32_t sequence;
static uint32_t call;
static portMUX_TYPE command_lock = portMUX_INITIALIZER_UNLOCKED;
static bool accepting_transcripts;

static const char *source_name(command_source_t source)
{
    if (source == SOURCE_TRANSCRIPT) return "transcript";
    if (source == SOURCE_MQTT) return "MQTT";
    return "unknown";
}

// The two transports do not carry a shared utterance ID. Merge opposite
// sources by target within the same call and platform tool timeout.
static esp_err_t request_command(servo_tool_operation_t operation, uint8_t position, command_source_t source,
                                 servo_tool_request_t *request)
{
    const int64_t now = esp_timer_get_time();
    const command_source_t other_source = source == SOURCE_TRANSCRIPT ? SOURCE_MQTT : SOURCE_TRANSCRIPT;
    servo_command_t *match = NULL, *latest = NULL, *free_slot = NULL;
    portENTER_CRITICAL(&command_lock);
    if (source == SOURCE_TRANSCRIPT && !accepting_transcripts) {
        portEXIT_CRITICAL(&command_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < MAX_RECENT_COMMANDS; ++i) {
        servo_command_t *entry = &commands[i];
        if (!entry->request.id || (entry->complete && entry->match_until_us <= now)) {
            if (!free_slot) free_slot = entry;
            continue;
        }
        if (operation != SERVO_TOOL_SET_POSITION || entry->request.operation != SERVO_TOOL_SET_POSITION ||
            entry->call != call || entry->match_until_us <= now) continue;
        if (!latest || entry->request.id > latest->request.id) latest = entry;
        if (entry->request.position == position && entry->sources == other_source &&
            (!match || entry->request.id > match->request.id)) {
            match = entry;
        }
    }
    if (latest && latest->request.position == position &&
        (source == SOURCE_TRANSCRIPT || (latest->sources & SOURCE_TRANSCRIPT)) &&
        (!latest->complete || (latest->c3_ack && latest->state.target == position))) {
        // Repeated final transcripts or tool calls for the current action share its result.
        match = latest;
    } else if (source == SOURCE_TRANSCRIPT && match != latest) {
        // A new transcript after an intervening target must start a new action.
        // A first, delayed MQTT counterpart can still join an older transcript.
        match = NULL;
    }
    if (match) {
        match->sources |= source;
        *request = match->request;
        if (last_action.request.id == request->id) last_action = *match;
    } else if (free_slot) {
        *free_slot = (servo_command_t){
            .request = {.id = ++sequence, .deadline_us = now + ACK_TIMEOUT_MS * 1000,
                        .operation = operation, .position = position},
            .match_until_us = now + TOOL_WINDOW_MS * 1000,
            .call = call,
            .source = source,
            .sources = source,
        };
        *request = free_slot->request;
        if (operation == SERVO_TOOL_SET_POSITION) last_action = *free_slot;
    }
    portEXIT_CRITICAL(&command_lock);
    if (!match && !free_slot) return ESP_ERR_NO_MEM;

    if (operation == SERVO_TOOL_GET_STATUS) {
        ESP_LOGI(TAG, "C3 state query requested via MQTT (id=%lu)", (unsigned long)request->id);
    } else {
        ESP_LOGI(TAG, "Voice target %c %s (source=%s, id=%lu)", 'A' + position,
                 match ? "joined existing ESP-NOW command" : "queued for ESP-NOW", source_name(source),
                 (unsigned long)request->id);
    }
    return ESP_OK;
}

static void on_user_transcript(const char *text)
{
    const servo_voice_command_t command = servo_voice_command_parse(text);
    if (command == SERVO_VOICE_NONE) {
        if (text && (strstr(text, "舵机") || strstr(text, "面具") || strstr(text, "面罩") || strstr(text, "脸甲"))) {
            ESP_LOGI(TAG, "Final user transcript mentions the mechanism without an explicit open/close command");
        }
        return;
    }

    servo_tool_request_t request;
    const uint8_t position = command == SERVO_VOICE_OPEN ? 1 : 0;
    const esp_err_t err = request_command(SERVO_TOOL_SET_POSITION, position, SOURCE_TRANSCRIPT, &request);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Transcript servo command rejected: %s", esp_err_to_name(err));
    }
}

void servo_tool_accept_transcripts(bool accept)
{
    portENTER_CRITICAL(&command_lock);
    if (accept && !accepting_transcripts) ++call;
    accepting_transcripts = accept;
    portEXIT_CRITICAL(&command_lock);
}

static servo_command_t *find_command(uint32_t id)
{
    for (size_t i = 0; i < MAX_RECENT_COMMANDS; ++i) {
        if (commands[i].request.id == id) return &commands[i];
    }
    return NULL;
}

static servo_command_t await_confirmation(const servo_tool_request_t *request)
{
    for (;;) {
        portENTER_CRITICAL(&command_lock);
        const servo_command_t *entry = find_command(request->id);
        const servo_command_t status = entry ? *entry : (servo_command_t){.request = *request};
        portEXIT_CRITICAL(&command_lock);
        const int64_t remaining_us = request->deadline_us - esp_timer_get_time();
        if (status.complete || !entry || remaining_us <= 0) return status;
        const TickType_t ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        (void)xSemaphoreTake(completion, ticks ? ticks : 1);
    }
}

static const char *position_name(uint8_t position)
{
    return position ? "B" : "A";
}

static void add_delivery(cJSON *result, const servo_command_t *command)
{
    cJSON_AddNumberToObject(result, "command_id", command->request.id);
    cJSON_AddBoolToObject(result, "mqtt_received", (command->sources & SOURCE_MQTT) != 0);
    cJSON_AddBoolToObject(result, "esp_now_queued", command->esp_now_queued);
    cJSON_AddBoolToObject(result, "c3_ack", command->c3_ack);
}

static bool target_confirmed(const servo_command_t *command)
{
    return command->c3_ack && command->state.target == command->request.position;
}

static const char *delivery_status(const servo_command_t *command, int64_t now)
{
    if (command->c3_ack) return target_confirmed(command) ? "confirmed" : "target_mismatch";
    if (!command->complete && now < command->request.deadline_us) return "pending";
    return command->esp_now_queued ? "no_c3_ack" : "esp_now_not_queued";
}

static esp_err_t set_power(const cJSON *params, cJSON *result)
{
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(params, "power");
    if (!cJSON_IsBool(power)) return ESP_ERR_INVALID_ARG;
    const bool enabled = cJSON_IsTrue(power);
    servo_tool_request_t request;
    const esp_err_t err = request_command(SERVO_TOOL_SET_POSITION, enabled ? 1 : 0, SOURCE_MQTT, &request);
    if (err != ESP_OK) return err;

    const servo_command_t command = await_confirmation(&request);
    const bool executed = target_confirmed(&command);
    add_delivery(result, &command);
    cJSON_AddStringToObject(result, "source", source_name(command.source));
    cJSON_AddBoolToObject(result, "executed", executed);
    if (executed) {
        cJSON_AddBoolToObject(result, "power", enabled);
        cJSON_AddStringToObject(result, "position", position_name(command.state.target));
        cJSON_AddBoolToObject(result, "moving", command.state.moving);
    }
    if (!command.c3_ack) {
        cJSON_AddStringToObject(result, "state", "unknown");
        cJSON_AddStringToObject(result, "status", delivery_status(&command, esp_timer_get_time()));
        // The tool finished. The payload reports the unconfirmed physical action.
        return ESP_OK;
    }
    return executed ? ESP_OK : ESP_FAIL;
}

static esp_err_t add_last_command(cJSON *result, const servo_command_t *last)
{
    cJSON *command = cJSON_AddObjectToObject(result, "last_command");
    if (!command) return ESP_ERR_NO_MEM;
    const int64_t now = esp_timer_get_time();
    add_delivery(command, last);
    cJSON_AddStringToObject(command, "target_position", position_name(last->request.position));
    cJSON_AddStringToObject(command, "source", source_name(last->source));
    cJSON_AddStringToObject(command, "status", delivery_status(last, now));
    cJSON_AddBoolToObject(command, "executed", target_confirmed(last));
    if (last->c3_ack) cJSON_AddBoolToObject(command, "moving", last->state.moving);
    cJSON_AddNumberToObject(command, "age_ms", (now - last->request.deadline_us) / 1000 + ACK_TIMEOUT_MS);
    return ESP_OK;
}

static esp_err_t get_last_command(cJSON *result)
{
    portENTER_CRITICAL(&command_lock);
    const servo_command_t last = last_action;
    const bool current_call = last.call == call;
    portEXIT_CRITICAL(&command_lock);
    if (!last.request.id || !current_call) {
        return cJSON_AddNullToObject(result, "last_command") ? ESP_OK : ESP_ERR_NO_MEM;
    }
    const servo_command_t confirmed = last.complete ? last : await_confirmation(&last.request);
    return add_last_command(result, &confirmed);
}

static esp_err_t get_status(const cJSON *params, cJSON *result)
{
    const cJSON *last_only = cJSON_GetObjectItemCaseSensitive(params, "last_command_only");
    if (last_only && !cJSON_IsBool(last_only)) return ESP_ERR_INVALID_ARG;
    if (cJSON_IsTrue(last_only)) return get_last_command(result);

    servo_tool_request_t request;
    const esp_err_t err = request_command(SERVO_TOOL_GET_STATUS, 0, SOURCE_MQTT, &request);
    if (err != ESP_OK) return err;
    const servo_command_t query = await_confirmation(&request);
    add_delivery(result, &query);
    const char *state = "unknown";
    if (query.c3_ack) {
        static const char *const states[2][2] = {{"closed", "closing"}, {"open", "opening"}};
        state = states[query.state.target][query.state.moving];
        cJSON_AddStringToObject(result, "target_position", position_name(query.state.target));
        cJSON_AddBoolToObject(result, "moving", query.state.moving);
    } else {
        cJSON_AddStringToObject(result, "status", delivery_status(&query, esp_timer_get_time()));
    }
    cJSON_AddStringToObject(result, "state", state);

    portENTER_CRITICAL(&command_lock);
    const servo_command_t last = last_action;
    portEXIT_CRITICAL(&command_lock);
    return last.request.id ? add_last_command(result, &last) : ESP_OK;
}

static esp_err_t handle_tool(const char *action, const cJSON *params, cJSON *result, void *context)
{
    (void)context;
    if (strcmp(action, SET_POWER) == 0) return set_power(params, result);
    if (strcmp(action, GET_STATUS) == 0) return get_status(params, result);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t servo_tool_init(void)
{
    completion = xSemaphoreCreateBinaryStatic(&completion_control);
    if (!completion) return ESP_ERR_NO_MEM;
    agent_link_platform_set_tool_callback(handle_tool, NULL);
    agent_link_platform_set_user_transcript_callback(SUGARAT_LOCAL_SERVO_COMMANDS ? on_user_transcript : NULL);
    ESP_LOGI(TAG, "Voice servo control: %s",
             SUGARAT_LOCAL_SERVO_COMMANDS ? "platform tools and local transcripts" : "platform tools only");
    return ESP_OK;
}

esp_err_t servo_tool_attach(int agent_id, const char *session_id)
{
    if (!completion) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = agent_link_platform_attach_tool(agent_id, session_id, DEFINITIONS);
    ESP_LOGI(TAG, "Session servo tools: %s", esp_err_to_name(err));
    return err;
}

bool servo_tool_receive(servo_tool_request_t *request)
{
    servo_command_t *next = NULL;
    portENTER_CRITICAL(&command_lock);
    for (size_t i = 0; i < MAX_RECENT_COMMANDS; ++i) {
        servo_command_t *entry = &commands[i];
        if (entry->request.id && !entry->dispatched &&
            (!next || entry->request.id < next->request.id)) {
            next = entry;
        }
    }
    if (next) {
        next->dispatched = true;
        *request = next->request;
    }
    portEXIT_CRITICAL(&command_lock);
    return next != NULL;
}

void servo_tool_radio_queued(const servo_tool_request_t *request)
{
    portENTER_CRITICAL(&command_lock);
    servo_command_t *entry = find_command(request->id);
    if (entry && !entry->esp_now_queued) {
        entry->esp_now_queued = true;
        if (last_action.request.id == request->id) last_action = *entry;
    }
    portEXIT_CRITICAL(&command_lock);
}

void servo_tool_complete(const servo_tool_request_t *request, const servo_tool_state_t *state)
{
    portENTER_CRITICAL(&command_lock);
    servo_command_t *entry = find_command(request->id);
    const bool notify = entry && !entry->complete;
    if (notify) {
        if (state) {
            entry->c3_ack = true;
            entry->state = *state;
        }
        entry->complete = true;
        if (last_action.request.id == request->id) last_action = *entry;
    }
    portEXIT_CRITICAL(&command_lock);
    if (!notify) return;

    xSemaphoreGive(completion);
    if (!state) {
        ESP_LOGW(TAG, "No C3 reply for voice %s (id=%lu)",
                 request->operation == SERVO_TOOL_GET_STATUS ? "state query" : "position command",
                 (unsigned long)request->id);
    } else {
        ESP_LOGI(TAG, "C3 voice %s reply (id=%lu, target=%c, moving=%u)",
                 request->operation == SERVO_TOOL_GET_STATUS ? "state query" : "position command",
                 (unsigned long)request->id, 'A' + state->target, (unsigned)state->moving);
    }
}
