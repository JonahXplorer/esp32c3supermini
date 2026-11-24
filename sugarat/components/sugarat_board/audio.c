#include "audio.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_attr.h"

enum { AMP_ENABLE = GPIO_NUM_3, PLAYBACK_VOLUME = 35 };

static const char *TAG = "sugarat_audio";
static i2s_chan_handle_t mic;
static i2s_chan_handle_t speaker;
static int32_t previous_input;
static int32_t highpass;
static DRAM_ATTR atomic_uint speaker_completed;
static DRAM_ATTR atomic_uint speaker_empty_slots;

static bool IRAM_ATTR speaker_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle;
    (void)event;
    (void)context;
    atomic_fetch_add_explicit(&speaker_completed, 1, memory_order_relaxed);
    return false;
}

static bool IRAM_ATTR speaker_empty(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle;
    (void)event;
    (void)context;
    atomic_fetch_add_explicit(&speaker_empty_slots, 1, memory_order_relaxed);
    return false;
}

sugarat_speaker_dma_t sugarat_speaker_dma(void)
{
    return (sugarat_speaker_dma_t){
        .completed = atomic_load_explicit(&speaker_completed, memory_order_relaxed),
        .empty_slots = atomic_load_explicit(&speaker_empty_slots, memory_order_relaxed),
    };
}

static esp_err_t init_mic(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, NULL, &mic), TAG, "Create microphone RX");
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SUGARAT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = GPIO_NUM_NC, .bclk = GPIO_NUM_16, .ws = GPIO_NUM_2,
                     .dout = GPIO_NUM_NC, .din = GPIO_NUM_17},
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(mic, &config), TAG, "Initialize microphone RX");
    return i2s_channel_enable(mic);
}

static esp_err_t init_speaker(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear_after_cb = true;
    channel.dma_desc_num = SUGARAT_SPEAKER_DMA_DESCRIPTORS;
    channel.dma_frame_num = SUGARAT_SPEAKER_DMA_SAMPLES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &speaker, NULL), TAG, "Create speaker TX");
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SUGARAT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_15, .bclk = GPIO_NUM_48, .ws = GPIO_NUM_38,
                     .dout = GPIO_NUM_47, .din = GPIO_NUM_NC},
    };
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(speaker, &config), TAG, "Initialize speaker TX");
    const i2s_event_callbacks_t callbacks = {.on_sent = speaker_sent, .on_send_q_ovf = speaker_empty};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(speaker, &callbacks, NULL), TAG, "Watch speaker DMA");
    return i2s_channel_enable(speaker);
}

esp_err_t sugarat_audio_init(void)
{
    const gpio_config_t amplifier = {.pin_bit_mask = 1ULL << AMP_ENABLE, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&amplifier), TAG, "Configure amplifier");
    sugarat_speaker_enable(false);
    esp_err_t err = init_mic();
    if (err == ESP_OK) {
        err = init_speaker();
    }
    if (err != ESP_OK) {
        sugarat_audio_deinit();
    }
    return err;
}

void sugarat_audio_deinit(void)
{
    sugarat_speaker_enable(false);
    if (mic) {
        (void)i2s_channel_disable(mic);
        (void)i2s_del_channel(mic);
        mic = NULL;
    }
    if (speaker) {
        (void)i2s_channel_disable(speaker);
        (void)i2s_del_channel(speaker);
        speaker = NULL;
    }
}

esp_err_t sugarat_mic_read(int16_t *pcm, size_t *samples)
{
    int32_t raw[SUGARAT_AUDIO_FRAME];
    size_t bytes = 0;
    *samples = 0;
    const esp_err_t err = i2s_channel_read(mic, raw, sizeof(raw), &bytes, 60);
    if (err != ESP_OK && !(err == ESP_ERR_TIMEOUT && bytes != 0)) {
        return err;
    }
    *samples = bytes / sizeof(raw[0]);
    for (size_t i = 0; i < *samples; ++i) {
        int32_t input = raw[i] >> 14;
        input = input > INT16_MAX ? INT16_MAX : input < INT16_MIN ? INT16_MIN : input;
        highpass = ((highpass + input - previous_input) * 32604LL) / 32768;
        previous_input = input;
        pcm[i] = highpass > INT16_MAX ? INT16_MAX : highpass < INT16_MIN ? INT16_MIN : highpass;
    }
    return ESP_OK;
}

esp_err_t sugarat_speaker_write(const int16_t *pcm, size_t samples)
{
    if (samples > SUGARAT_AUDIO_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }
    int16_t stereo[SUGARAT_AUDIO_FRAME * 2];
    for (size_t i = 0; i < samples; ++i) {
        stereo[i * 2] = stereo[i * 2 + 1] = (int32_t)pcm[i] * PLAYBACK_VOLUME / 100;
    }
    size_t bytes_written = 0;
    const size_t bytes = samples * 2 * sizeof(stereo[0]);
    const esp_err_t err = i2s_channel_write(speaker, stereo, bytes, &bytes_written, 100);
    return err == ESP_OK && bytes_written != bytes ? ESP_FAIL : err;
}

void sugarat_speaker_enable(bool enabled)
{
    (void)gpio_set_level(AMP_ENABLE, enabled);
}
