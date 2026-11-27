#include "conversations.h"
#include "screen_port.h"
#include <string.h>

static bool requested;
static int agent;

esp_err_t screen_port_init(bool touch_ready)
{
    (void)touch_ready;
    return ESP_OK;
}

esp_err_t conversations_request(int agent_id, conversations_action_t action, size_t offset, const char *id)
{
    (void)action;
    (void)offset;
    (void)id;
    requested = true;
    agent = agent_id;
    return ESP_OK;
}

bool conversations_read(conversations_result_t *result)
{
    if (!requested) return false;
    requested = false;
    *result = (conversations_result_t){.agent_id = agent, .count = 2, .has_page = true, .result = ESP_OK};
    strcpy(result->rows[0].id, "main");
    strcpy(result->rows[0].name, "Voice session");
    strcpy(result->rows[1].id, "second");
    strcpy(result->rows[1].name, "Workshop conversation title");
    return true;
}
