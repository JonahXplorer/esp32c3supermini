#pragma once

#include "conversations.h"
#include "freertos/FreeRTOS.h"

void conversations_init(void);
bool conversations_service(TickType_t wait);
esp_err_t voice_change_conversation(int agent_id, conversations_action_t action, const char *id);
