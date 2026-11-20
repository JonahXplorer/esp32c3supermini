#include "wake_word.h"

#include <string.h>
#include "audio.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "sugarat_wake";
static srmodel_list_t *models;
static const esp_afe_sr_iface_t *afe;
static esp_afe_sr_data_t *detector;
static int16_t *input;
static size_t feed_samples;
static size_t fetch_samples;
static size_t filled;
static size_t pending_samples;

esp_err_t wake_word_init(void)
{
    models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, "jarvis") : NULL;
    if (!name) {
        ESP_LOGE(TAG, "Jarvis WakeNet model missing from model partition");
        wake_word_deinit();
        return ESP_ERR_NOT_FOUND;
    }
    afe_config_t *config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!config) {
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }
    config->aec_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->wakenet_init = true;
    config->wakenet_model_name = name;
    config->wakenet_mode = DET_MODE_95;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe = esp_afe_handle_from_config(config);
    detector = afe ? afe->create_from_config(config) : NULL;
    afe_config_free(config);
    if (!detector) {
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }
    const int feed = afe->get_feed_chunksize(detector);
    const int fetch = afe->get_fetch_chunksize(detector);
    if (feed <= 0 || fetch <= 0 || afe->get_samp_rate(detector) != SUGARAT_SAMPLE_RATE ||
        afe->get_feed_channel_num(detector) != 1 || afe->get_fetch_channel_num(detector) != 1) {
        wake_word_deinit();
        return ESP_ERR_NOT_SUPPORTED;
    }
    feed_samples = feed;
    fetch_samples = fetch;
    input = heap_caps_malloc(feed_samples * sizeof(*input), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!input) {
        wake_word_deinit();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Jarvis AFE ready (%s, 16 kHz mono, feed %d, fetch %d)", name, feed, fetch);
    return ESP_OK;
}

void wake_word_deinit(void)
{
    heap_caps_free(input);
    input = NULL;
    if (detector) {
        afe->destroy(detector);
        detector = NULL;
    }
    afe = NULL;
    if (models) {
        esp_srmodel_deinit(models);
        models = NULL;
    }
    filled = 0;
    pending_samples = 0;
}

bool wake_word_feed(const int16_t *pcm, size_t samples)
{
    while (samples != 0) {
        const size_t room = feed_samples - filled;
        const size_t copied = samples < room ? samples : room;
        memcpy(input + filled, pcm, copied * sizeof(*input));
        filled += copied;
        pcm += copied;
        samples -= copied;
        if (filled != feed_samples) {
            continue;
        }
        filled = 0;
        if (afe->feed(detector, input) < 0) {
            wake_word_reset();
            continue;
        }
        pending_samples += feed_samples;
        while (pending_samples >= fetch_samples) {
            // Keep audio capture moving if this feed has not produced a full result yet.
            const afe_fetch_result_t *result = afe->fetch_with_delay(detector, 0);
            if (!result || result->ret_value != ESP_OK) {
                break;
            }
            pending_samples -= fetch_samples;
            if (result->wakeup_state == WAKENET_DETECTED) {
                return true;
            }
        }
    }
    return false;
}

void wake_word_reset(void)
{
    filled = 0;
    pending_samples = 0;
    (void)afe->reset_buffer(detector);
}
