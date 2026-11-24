#include "audio.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_timer.h"

enum { AMP_ENABLE = GPIO_NUM_3, MIC_GAIN_NUM = 3, MIC_GAIN_DEN = 2, MIC_REPORT_US = 4000000 };

static const char *TAG = "sugarat_audio";
static i2s_chan_handle_t mic;
static i2s_chan_handle_t speaker;
static int32_t previous_input;
static int32_t highpass;
static atomic_uint speaker_volume = SUGARAT_VOLUME_DEFAULT;
static DRAM_ATTR atomic_uint speaker_completed;
static DRAM_ATTR atomic_uint speaker_empty_slots;
static struct {
    int64_t started_us;
    uint32_t samples;
    uint64_t absolute_sum;
    uint32_t peak;
    uint32_t clipped;
} mic_stats;

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
    if (err == ESP_OK) ESP_LOGI(TAG, "Microphone digital gain %u/%u", MIC_GAIN_NUM, MIC_GAIN_DEN);
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
    if (*samples && !mic_stats.started_us) mic_stats.started_us = esp_timer_get_time();
    uint32_t frame_sum = 0;
    for (size_t i = 0; i < *samples; ++i) {
        const int32_t input = raw[i] >> 14;
        highpass = ((highpass + input - previous_input) * 32604LL) / 32768;
        previous_input = input;
        const int32_t amplified = highpass * MIC_GAIN_NUM / MIC_GAIN_DEN;
        const uint32_t absolute = highpass < 0 ? (uint32_t)-highpass : (uint32_t)highpass;
        frame_sum += absolute;
        if (absolute > mic_stats.peak) mic_stats.peak = absolute;
        if (amplified > INT16_MAX || amplified < INT16_MIN) ++mic_stats.clipped;
        pcm[i] = amplified > INT16_MAX ? INT16_MAX : amplified < INT16_MIN ? INT16_MIN : amplified;
    }
    mic_stats.absolute_sum += frame_sum;
    mic_stats.samples += *samples;
    const int64_t now_us = esp_timer_get_time();
    if (mic_stats.samples && now_us - mic_stats.started_us >= MIC_REPORT_US) {
        ESP_LOGI(TAG, "Mic level before gain: mean_abs=%lu peak=%lu; clipped_after_gain=%lu/%lu",
                 (unsigned long)(mic_stats.absolute_sum / mic_stats.samples), (unsigned long)mic_stats.peak,
                 (unsigned long)mic_stats.clipped, (unsigned long)mic_stats.samples);
        mic_stats.started_us = now_us;
        mic_stats.samples = mic_stats.absolute_sum = mic_stats.peak = mic_stats.clipped = 0;
    }
    return ESP_OK;
}

esp_err_t sugarat_speaker_write(const int16_t *pcm, size_t samples)
{
    if (samples > SUGARAT_AUDIO_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int32_t volume = (int32_t)atomic_load_explicit(&speaker_volume, memory_order_relaxed);
    int16_t stereo[SUGARAT_AUDIO_FRAME * 2];
    for (size_t i = 0; i < samples; ++i) {
        stereo[i * 2] = stereo[i * 2 + 1] = (int32_t)pcm[i] * volume / 100;
    }
    size_t bytes_written = 0;
    const size_t bytes = samples * 2 * sizeof(stereo[0]);
    const esp_err_t err = i2s_channel_write(speaker, stereo, bytes, &bytes_written, 100);
    return err == ESP_OK && bytes_written != bytes ? ESP_FAIL : err;
}

void sugarat_speaker_set_volume(unsigned percent)
{
    atomic_store_explicit(&speaker_volume, percent <= 100 ? percent : 100, memory_order_relaxed);
}

void sugarat_speaker_enable(bool enabled)
{
    (void)gpio_set_level(AMP_ENABLE, enabled);
}
