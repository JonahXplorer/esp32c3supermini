#pragma once

#include "agent_link_platform.h"

enum { SUGARAT_CONVERSATIONS_PAGE_SIZE = 8 };

typedef enum {
    CONVERSATIONS_LIST,
    CONVERSATIONS_CREATE,
    CONVERSATIONS_SELECT,
    CONVERSATIONS_DELETE,
} conversations_action_t;

typedef struct {
    uint32_t revision;
    int agent_id;
    size_t offset;
    size_t count;
    bool has_page;
    bool has_more;
    bool changed;
    esp_err_t result;
    agent_link_platform_conversation_t rows[SUGARAT_CONVERSATIONS_PAGE_SIZE];
} conversations_result_t;

esp_err_t conversations_request(int agent_id, conversations_action_t action, size_t offset, const char *id);
bool conversations_read(conversations_result_t *result);
