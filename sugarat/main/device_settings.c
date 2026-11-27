#include "device_settings.h"

#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "sugarat_settings";
static const char *NAMESPACE = "sugarat_ui";
static device_settings_t saved;

device_settings_t device_settings_load(void)
{
    saved = (device_settings_t){.volume = SUGARAT_VOLUME_DEFAULT, .brightness = SUGARAT_BRIGHTNESS_DEFAULT};
    esp_err_t err = nvs_flash_init();
    nvs_handle_t handle;
    if (err == ESP_OK) err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t value;
        if (nvs_get_u8(handle, "volume", &value) == ESP_OK && value <= 100) saved.volume = value;
        if (nvs_get_u8(handle, "brightness", &value) == ESP_OK &&
            value >= SUGARAT_BRIGHTNESS_MIN && value <= 100) saved.brightness = value;
        if (nvs_get_u8(handle, "voice_china", &value) == ESP_OK) saved.voice_china = value == 1;
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Using default settings: %s", esp_err_to_name(err));
    }
    return saved;
}

esp_err_t device_settings_save(device_settings_t settings)
{
    const bool volume_changed = settings.volume != saved.volume;
    const bool brightness_changed = settings.brightness != saved.brightness;
    const bool voice_server_changed = settings.voice_china != saved.voice_china;
    if (!volume_changed && !brightness_changed && !voice_server_changed) return ESP_OK;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (volume_changed) err = nvs_set_u8(handle, "volume", settings.volume);
    if (err == ESP_OK && brightness_changed) err = nvs_set_u8(handle, "brightness", settings.brightness);
    if (err == ESP_OK && voice_server_changed) err = nvs_set_u8(handle, "voice_china", settings.voice_china);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) saved = settings;
    return err;
}
