#include "conversations_internal.h"

#include <string.h>
#include "esp_attr.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

typedef struct {
    int agent_id;
    conversations_action_t action;
    size_t offset;
    char id[sizeof(((agent_link_platform_conversation_t *)0)->id)];
} request_t;

static QueueHandle_t jobs;
static StaticQueue_t job_queue;
static uint8_t job_storage[sizeof(request_t)];
static SemaphoreHandle_t results;
static StaticSemaphore_t results_mutex;
static EXT_RAM_BSS_ATTR conversations_result_t published, pending;

void conversations_init(void)
{
    jobs = xQueueCreateStatic(1, sizeof(request_t), job_storage, &job_queue);
    results = xSemaphoreCreateMutexStatic(&results_mutex);
}

esp_err_t conversations_request(int agent_id, conversations_action_t action, size_t offset, const char *id)
{
    if (!jobs || agent_id <= 0) return ESP_ERR_INVALID_STATE;
    request_t job = {.agent_id = agent_id, .action = action, .offset = offset};
    if (action == CONVERSATIONS_SELECT || action == CONVERSATIONS_DELETE) {
        if (!id || !id[0] || strlen(id) >= sizeof(job.id)) return ESP_ERR_INVALID_ARG;
        strcpy(job.id, id);
    }
    return xQueueSend(jobs, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool conversations_read(conversations_result_t *result)
{
    if (!results || xSemaphoreTake(results, 0) != pdTRUE) return false;
    const bool updated = result->revision != published.revision;
    if (updated) *result = published;
    xSemaphoreGive(results);
    return updated;
}

static esp_err_t load_page(void)
{
    size_t total = 0;
    esp_err_t err = agent_link_platform_conversations(pending.agent_id, pending.offset, pending.rows,
                                                    SUGARAT_CONVERSATIONS_PAGE_SIZE, &pending.count, &total);
    if (err == ESP_OK && !pending.count && pending.offset) {
        // Another client may have removed the last page since we displayed it.
        pending.offset = 0;
        err = agent_link_platform_conversations(pending.agent_id, 0, pending.rows,
                                               SUGARAT_CONVERSATIONS_PAGE_SIZE, &pending.count, &total);
    }
    pending.has_page = err == ESP_OK;
    pending.has_more = err == ESP_OK && total > pending.offset + pending.count;
    return err;
}

bool conversations_service(TickType_t wait)
{
    request_t job;
    if (xQueueReceive(jobs, &job, wait) != pdTRUE) return false;
    memset(&pending, 0, sizeof(pending));
    pending.revision = published.revision + 1;
    pending.agent_id = job.agent_id;
    pending.offset = job.offset;
    if (job.action != CONVERSATIONS_LIST) {
        pending.result = voice_change_conversation(job.agent_id, job.action, job.id);
        pending.changed = pending.result == ESP_OK;
    }
    // Selecting a row changes the route without needing another request for the same list.
    if (pending.result == ESP_OK && job.action != CONVERSATIONS_SELECT) {
        if (job.action == CONVERSATIONS_CREATE) pending.offset = 0;
        pending.result = load_page();
    }

    xSemaphoreTake(results, portMAX_DELAY);
    published = pending;
    xSemaphoreGive(results);
    return true;
}
