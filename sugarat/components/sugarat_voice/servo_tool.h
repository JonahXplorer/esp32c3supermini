#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { SERVO_TOOL_SET_POSITION, SERVO_TOOL_GET_STATUS } servo_tool_operation_t;

typedef struct {
    uint32_t id;
    int64_t deadline_us;
    servo_tool_operation_t operation;
    uint8_t position;
} servo_tool_request_t;

typedef struct {
    uint8_t target;
    bool moving;
} servo_tool_state_t;

esp_err_t servo_tool_init(void);
esp_err_t servo_tool_attach(int agent_id, const char *session_id);
void servo_tool_accept_transcripts(bool accept);
bool servo_tool_receive(servo_tool_request_t *request);
void servo_tool_radio_queued(const servo_tool_request_t *request);
void servo_tool_complete(const servo_tool_request_t *request, const servo_tool_state_t *state);
