#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t id;
    int64_t deadline_us;
    uint8_t position;
    bool from_transcript;
} servo_tool_request_t;

esp_err_t servo_tool_init(void);
esp_err_t servo_tool_attach(int agent_id, const char *session_id);
void servo_tool_accept_transcripts(bool accept);
bool servo_tool_receive(servo_tool_request_t *request);
void servo_tool_complete(const servo_tool_request_t *request, esp_err_t error, bool moving);
