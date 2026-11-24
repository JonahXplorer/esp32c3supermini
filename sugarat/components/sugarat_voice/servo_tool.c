#include "servo_tool.h"

#include <string.h>
#include <strings.h>

#include "agent_link_platform.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// An idle C3 can be scanning for Sugarat when the first voice command arrives.
enum { ACK_TIMEOUT_MS = 5500, TOOL_WINDOW_MS = 15000, MAX_RECENT_COMMANDS = 16 };

static const char *TAG = "servo_tool";
static const char *ACTION = "set_servo_power";
static const char DEFINITION[] =
    "{\"name\":\"set_servo_power\","
    "\"description\":\"控制 Sugarat 上由两只舵机联动的开合机构。舵机、面具、面罩都指同一个机构。"
    "用户要求开、打开、开启或掀开时调用本工具，传 power=true 到 B 点。要求关、关闭、关上或合上时传 power=false 到 A 点。"
    "例如打开面罩、关闭面具、把面具掀开、把面罩合上。仅在用户明确要求实际开合时调用。"
    "收到工具成功回执后再确认，moving=true 表示仍在转动。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"power\":{\"type\":\"boolean\","
    "\"description\":\"true 打开舵机、面具或面罩到 B 点，false 关闭到 A 点\"}},\"required\":[\"power\"]},\"timeout\":15}";

typedef enum { SOURCE_TRANSCRIPT = 1, SOURCE_MQTT = 2 } command_source_t;

typedef struct {
    servo_tool_request_t request;
    int64_t match_until_us;
    uint32_t call;
    uint8_t sources;
    bool dispatched;
    bool complete;
    esp_err_t error;
    bool moving;
} servo_command_t;

static StaticSemaphore_t completion_control;
static SemaphoreHandle_t completion;
static servo_command_t commands[MAX_RECENT_COMMANDS];
static uint32_t sequence;
static uint32_t call;
static portMUX_TYPE command_lock = portMUX_INITIALIZER_UNLOCKED;
static bool accepting_transcripts;

static bool take_phrase(const char **text, const char *phrase)
{
    const size_t length = strlen(phrase);
    if (strncmp(*text, phrase, length) != 0) return false;
    *text += length;
    *text += strspn(*text, " \t\r\n");
    return true;
}

static bool take_one_of(const char **text, const char *const *phrases)
{
    for (; *phrases; ++phrases) {
        if (take_phrase(text, *phrases)) return true;
    }
    return false;
}

static bool parse_power_command(const char *text, bool *open)
{
    static const char *const wake_punctuation[] = {"，", "。", "：", ",", ".", ":", "！", "!", NULL};
    static const char *const polite_prefixes[] = {"请帮我", "请帮忙", "帮我", "帮忙", "请", NULL};
    static const char *const targets[] = {"舵机", "面具", "面罩", NULL};
    static const char *const open_verbs[] = {"打开", "开启", "掀开", "开", NULL};
    static const char *const close_verbs[] = {"关闭", "关掉", "关上", "合上", "关", NULL};
    static const char *const end_punctuation[] = {"。", "！", ".", "!", NULL};

    text += strspn(text, " \t\r\n");
    // The wake-word audio can be transcribed together with the first command.
    if (strncasecmp(text, "Jarvis", sizeof("Jarvis") - 1) == 0) {
        text += sizeof("Jarvis") - 1;
        text += strspn(text, " \t\r\n");
        (void)take_one_of(&text, wake_punctuation);
    } else if (take_phrase(&text, "贾维斯")) {
        (void)take_one_of(&text, wake_punctuation);
    }
    (void)take_one_of(&text, polite_prefixes);
    const bool target_first = take_phrase(&text, "把");
    if (target_first && !take_one_of(&text, targets)) return false;

    const bool enabled = take_one_of(&text, open_verbs);
    if (!enabled && !take_one_of(&text, close_verbs)) return false;
    (void)take_phrase(&text, "一下");
    if (!target_first && !take_one_of(&text, targets)) return false;
    (void)take_phrase(&text, "一下");
    (void)take_phrase(&text, "吧");
    while (take_one_of(&text, end_punctuation)) {}
    if (*text != '\0') return false;
    *open = enabled;
    return true;
}

static const char *source_name(command_source_t source)
{
    return source == SOURCE_TRANSCRIPT ? "transcript" : "MQTT";
}

// The two transports do not carry a shared utterance ID. Merge opposite
// sources by target within the same call and platform tool timeout.
static esp_err_t request_position(uint8_t position, command_source_t source, servo_tool_request_t *request)
{
    const int64_t now = esp_timer_get_time();
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
        if (entry->call != call || entry->match_until_us <= now) continue;
        if (!latest || entry->request.id > latest->request.id) latest = entry;
        const bool other_source = source == SOURCE_TRANSCRIPT ? entry->sources == SOURCE_MQTT
                                                               : (entry->sources & SOURCE_TRANSCRIPT) != 0;
        if (entry->request.position == position && other_source &&
            (!match || entry->request.id > match->request.id)) {
            match = entry;
        }
    }
    // A fresh transcript for B after A is a new action, even if an older
    // MQTT-only B exists. A delayed MQTT for the older B can still use its result.
    if (source == SOURCE_TRANSCRIPT && latest && latest->request.position != position) match = NULL;
    if (match) {
        match->sources |= source;
        *request = match->request;
    } else if (free_slot) {
        *free_slot = (servo_command_t){
            .request = {.id = ++sequence, .deadline_us = now + ACK_TIMEOUT_MS * 1000, .position = position},
            .match_until_us = now + TOOL_WINDOW_MS * 1000,
            .call = call,
            .sources = source,
        };
        *request = free_slot->request;
    }
    portEXIT_CRITICAL(&command_lock);
    if (!match && !free_slot) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Voice target %c %s (source=%s, id=%lu)", 'A' + position,
             match ? "joined existing ESP-NOW command" : "queued for ESP-NOW", source_name(source),
             (unsigned long)request->id);
    return ESP_OK;
}

static void on_user_transcript(const char *text)
{
    bool open;
    if (!parse_power_command(text, &open)) {
        if (strstr(text, "舵机") || strstr(text, "面具") || strstr(text, "面罩")) {
            ESP_LOGI(TAG, "Final user transcript mentions the mechanism without an explicit open/close command");
        }
        return;
    }

    servo_tool_request_t request;
    const esp_err_t err = request_position(open ? 1 : 0, SOURCE_TRANSCRIPT, &request);
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

static esp_err_t await_confirmation(const servo_tool_request_t *request, bool *moving)
{
    for (;;) {
        esp_err_t error = ESP_ERR_TIMEOUT;
        portENTER_CRITICAL(&command_lock);
        const servo_command_t *entry = find_command(request->id);
        const bool done = entry && entry->complete;
        if (done) {
            error = entry->error;
            *moving = entry->moving;
        }
        portEXIT_CRITICAL(&command_lock);
        const int64_t remaining_us = request->deadline_us - esp_timer_get_time();
        if (done || !entry || remaining_us <= 0) return error;
        const TickType_t ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        (void)xSemaphoreTake(completion, ticks ? ticks : 1);
    }
}

static esp_err_t set_power(const char *action, const cJSON *params, cJSON *result, void *context)
{
    (void)context;
    if (strcmp(action, ACTION) != 0) return ESP_ERR_NOT_SUPPORTED;
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(params, "power");
    if (!cJSON_IsBool(power)) return ESP_ERR_INVALID_ARG;
    const bool enabled = cJSON_IsTrue(power);
    servo_tool_request_t request;
    esp_err_t err = request_position(enabled ? 1 : 0, SOURCE_MQTT, &request);
    if (err != ESP_OK) return err;

    bool moving = false;
    err = await_confirmation(&request, &moving);
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(result, "executed", true);
        cJSON_AddBoolToObject(result, "power", enabled);
        cJSON_AddStringToObject(result, "position", enabled ? "B" : "A");
        cJSON_AddBoolToObject(result, "moving", moving);
    }
    return err;
}

esp_err_t servo_tool_init(void)
{
    completion = xSemaphoreCreateBinaryStatic(&completion_control);
    if (!completion) return ESP_ERR_NO_MEM;
    agent_link_platform_set_tool_callback(set_power, NULL);
    agent_link_platform_set_user_transcript_callback(on_user_transcript);
    return ESP_OK;
}

esp_err_t servo_tool_attach(int agent_id, const char *session_id)
{
    if (!completion) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = agent_link_platform_attach_tool(agent_id, session_id, DEFINITION);
    ESP_LOGI(TAG, "Session power tool: %s", esp_err_to_name(err));
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

void servo_tool_complete(const servo_tool_request_t *request, esp_err_t error, bool moving)
{
    portENTER_CRITICAL(&command_lock);
    servo_command_t *entry = find_command(request->id);
    const bool notify = entry && !entry->complete;
    if (notify) {
        entry->error = error;
        entry->moving = moving;
        entry->complete = true;
    }
    portEXIT_CRITICAL(&command_lock);
    if (!notify) return;

    xSemaphoreGive(completion);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "C3 confirmed voice target %c (id=%lu, moving=%u)",
                 'A' + request->position, (unsigned long)request->id, (unsigned)moving);
    } else {
        ESP_LOGW(TAG, "ESP-NOW voice target %c failed (id=%lu): %s",
                 'A' + request->position, (unsigned long)request->id, esp_err_to_name(error));
    }
}
