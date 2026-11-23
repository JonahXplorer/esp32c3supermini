#include "servo_tool.h"

#include <string.h>

#include "agent_link_platform.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum { ACK_TIMEOUT_MS = 1400, MQTT_GRACE_MS = 4000 };

static const char *TAG = "servo_tool";
static const char *ACTION = "set_servo_power";
static const char DEFINITION[] =
    "{\"name\":\"set_servo_power\","
    "\"description\":\"此会话对应的 Sugarat 硬件连接了两只联动舵机，可通过此工具执行真实开关。"
    "用户说打开舵机时立即调用 set_servo_power，传 power=true，到 B 点；说关闭舵机时传 power=false，到 A 点。"
    "收到工具成功回执后再确认，moving 为 true 表示舵机仍在转动。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"power\":{\"type\":\"boolean\","
    "\"description\":\"true 打开到 B 点，false 关闭到 A 点\"}},\"required\":[\"power\"]},\"timeout\":15}";

typedef struct {
    uint32_t id;
    esp_err_t error;
    bool moving;
} servo_tool_reply_t;

static StaticQueue_t request_control, reply_control;
static uint8_t request_storage[sizeof(servo_tool_request_t)], reply_storage[sizeof(servo_tool_reply_t)];
static QueueHandle_t requests, replies;
static uint32_t sequence;
static portMUX_TYPE transcript_lock = portMUX_INITIALIZER_UNLOCKED;
static bool accepting_transcripts;
static struct {
    uint32_t version;
    uint8_t position;
    int64_t due_us;
} transcript;

static bool matches_command(const char *text, const char *command)
{
    const size_t length = strlen(command);
    if (strncmp(text, command, length) != 0) return false;
    const char *tail = text + length;
    tail += strspn(tail, " \t\r\n");
    if (strncmp(tail, "。", strlen("。")) == 0 || strncmp(tail, "！", strlen("！")) == 0) {
        tail += strlen("。");
    } else if (*tail == '.' || *tail == '!') {
        ++tail;
    }
    return tail[strspn(tail, " \t\r\n")] == '\0';
}

static void on_user_transcript(const char *text)
{
    text += strspn(text, " \t\r\n");
    if (strncmp(text, "请", strlen("请")) == 0) text += strlen("请");
    const bool open = matches_command(text, "打开舵机");
    if (!open && !matches_command(text, "关闭舵机")) return;

    const int64_t due_us = esp_timer_get_time() + MQTT_GRACE_MS * 1000;
    portENTER_CRITICAL(&transcript_lock);
    const bool accept = accepting_transcripts;
    if (accept) {
        ++transcript.version;
        transcript.position = open ? 1 : 0;
        transcript.due_us = due_us;
    }
    portEXIT_CRITICAL(&transcript_lock);
    if (accept) ESP_LOGI(TAG, "User voice command for %c, waiting %d ms for MQTT", open ? 'B' : 'A', MQTT_GRACE_MS);
}

void servo_tool_accept_transcripts(bool accept)
{
    portENTER_CRITICAL(&transcript_lock);
    accepting_transcripts = accept;
    transcript.due_us = 0;
    portEXIT_CRITICAL(&transcript_lock);
}

static esp_err_t set_power(const char *action, const cJSON *params, cJSON *result, void *context)
{
    (void)context;
    if (strcmp(action, ACTION) != 0) return ESP_ERR_NOT_SUPPORTED;
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(params, "power");
    if (!cJSON_IsBool(power)) return ESP_ERR_INVALID_ARG;
    const bool enabled = cJSON_IsTrue(power);
    const servo_tool_request_t request = {
        .id = ++sequence, .deadline_us = esp_timer_get_time() + ACK_TIMEOUT_MS * 1000,
        .position = enabled ? 1 : 0,
    };
    portENTER_CRITICAL(&transcript_lock);
    const uint32_t transcript_version = transcript.version;
    portEXIT_CRITICAL(&transcript_lock);
    if (xQueueSend(requests, &request, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Voice requested %c (source=MQTT)", 'A' + request.position);

    TickType_t remaining = pdMS_TO_TICKS(ACK_TIMEOUT_MS);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);
    servo_tool_reply_t reply;
    while (xQueueReceive(replies, &reply, remaining) == pdTRUE) {
        if (reply.id == request.id) {
            if (reply.error == ESP_OK) {
                portENTER_CRITICAL(&transcript_lock);
                if (transcript.version == transcript_version && transcript.position == request.position) {
                    transcript.due_us = 0;
                }
                portEXIT_CRITICAL(&transcript_lock);
                cJSON_AddBoolToObject(result, "executed", true);
                cJSON_AddBoolToObject(result, "power", enabled);
                cJSON_AddStringToObject(result, "position", enabled ? "B" : "A");
                cJSON_AddBoolToObject(result, "moving", reply.moving);
                ESP_LOGI(TAG, "C3 confirmed voice target %c (source=MQTT, moving=%u)",
                         'A' + request.position, (unsigned)reply.moving);
            }
            return reply.error;
        }
        if (xTaskCheckForTimeOut(&timeout, &remaining) == pdTRUE) break;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t servo_tool_init(void)
{
    requests = xQueueCreateStatic(1, sizeof(servo_tool_request_t), request_storage, &request_control);
    replies = xQueueCreateStatic(1, sizeof(servo_tool_reply_t), reply_storage, &reply_control);
    if (!requests || !replies) return ESP_ERR_NO_MEM;
    agent_link_platform_set_tool_callback(set_power, NULL);
    agent_link_platform_set_user_transcript_callback(on_user_transcript);
    return ESP_OK;
}

esp_err_t servo_tool_attach(int agent_id, const char *session_id)
{
    if (!requests || !replies) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = agent_link_platform_attach_tool(agent_id, session_id, DEFINITION);
    ESP_LOGI(TAG, "Session power tool: %s", esp_err_to_name(err));
    return err;
}

bool servo_tool_receive(servo_tool_request_t *request)
{
    if (!requests) return false;
    if (xQueueReceive(requests, request, 0) == pdTRUE) return true;

    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&transcript_lock);
    const bool due = transcript.due_us && now >= transcript.due_us;
    if (due) {
        *request = (servo_tool_request_t){
            .deadline_us = now + ACK_TIMEOUT_MS * 1000,
            .position = transcript.position,
            .from_transcript = true,
        };
        transcript.due_us = 0;
    }
    portEXIT_CRITICAL(&transcript_lock);
    if (due) ESP_LOGI(TAG, "Voice requested %c (source=device fallback)", 'A' + request->position);
    return due;
}

void servo_tool_complete(const servo_tool_request_t *request, esp_err_t error, bool moving)
{
    if (request->from_transcript) {
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "C3 confirmed voice target %c (source=device fallback, moving=%u)",
                     'A' + request->position, (unsigned)moving);
        } else {
            ESP_LOGW(TAG, "Device fallback for %c failed: %s", 'A' + request->position, esp_err_to_name(error));
        }
        return;
    }
    const servo_tool_reply_t reply = {.id = request->id, .error = error, .moving = moving};
    (void)xQueueOverwrite(replies, &reply);
}
