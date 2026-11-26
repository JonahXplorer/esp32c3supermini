#include "audio.h"

#include <stdatomic.h>
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_aec.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { PA_PIN = GPIO_NUM_46, TDM_SLOTS = 4, MIC1_SLOT = 0, REFERENCE_SLOT = 1,
       MIC2_SLOT = 2, PHYSICAL_MIC_MASK = 0x07, DIAGNOSTIC_US = 5000000,
       CAPTURE_RUN_BUDGET_US = 100000 };
static const char *TAG = "amoled_audio";
static i2s_chan_handle_t mic, speaker;
static const audio_codec_data_if_t *data_if;
static const audio_codec_gpio_if_t *gpio_if;
static const audio_codec_ctrl_if_t *dac_ctrl, *adc_ctrl;
static const audio_codec_if_t *dac_if, *adc_if;
static esp_codec_dev_handle_t dac, adc;
static aec_handle_t *aec;
static int16_t *raw, *near_end, *reference, *clean;
static size_t chunk_samples, raw_filled, output_at, output_remaining;
static int64_t next_capture_yield_us;
static atomic_uint volume = SUGARAT_VOLUME_DEFAULT;
static DRAM_ATTR atomic_uint completed, empty_slots, rx_overflows;
static struct {
    int64_t started_us;
    uint64_t mic_abs, ref_abs, clean_abs, total_aec_us;
    unsigned samples, mic_clipped, ref_clipped, max_aec_us;
    unsigned aec_blocks, over_budget_blocks, capture_yields;
} diagnostic;

static bool IRAM_ATTR speaker_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle; (void)event; (void)context;
    atomic_fetch_add_explicit(&completed, 1, memory_order_relaxed);
    return false;
}

static bool IRAM_ATTR speaker_empty(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle; (void)event; (void)context;
    atomic_fetch_add_explicit(&empty_slots, 1, memory_order_relaxed);
    return false;
}

static bool IRAM_ATTR mic_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle; (void)event; (void)context;
    atomic_fetch_add_explicit(&rx_overflows, 1, memory_order_relaxed);
    return false;
}

sugarat_speaker_dma_t sugarat_speaker_dma(void)
{
    return (sugarat_speaker_dma_t){atomic_load(&completed), atomic_load(&empty_slots)};
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = SUGARAT_SPEAKER_DMA_DESCRIPTORS;
    channel.dma_frame_num = SUGARAT_SPEAKER_DMA_SAMPLES;
    // Transmit zeroes on underrun, keeping the physical reference honest in silence.
    channel.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &speaker, &mic), TAG, "Duplex I2S");
    i2s_std_config_t tx = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SUGARAT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_42, .bclk = GPIO_NUM_9, .ws = GPIO_NUM_45,
                     .dout = GPIO_NUM_8, .din = GPIO_NUM_NC},
    };
    i2s_tdm_config_t rx = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SUGARAT_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_STEREO, I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {.mclk = GPIO_NUM_42, .bclk = GPIO_NUM_9, .ws = GPIO_NUM_45,
                     .dout = GPIO_NUM_NC, .din = GPIO_NUM_10},
    };
    tx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx.clk_cfg.bclk_div = 8;
    rx.slot_cfg.total_slot = TDM_SLOTS;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(speaker, &tx), TAG, "Speaker STD");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(mic, &rx), TAG, "Microphone TDM");
    const i2s_event_callbacks_t tx_cb = {.on_sent = speaker_sent, .on_send_q_ovf = speaker_empty};
    const i2s_event_callbacks_t rx_cb = {.on_recv_q_ovf = mic_overflow};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(speaker, &tx_cb, NULL), TAG, "TX callbacks");
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(mic, &rx_cb, NULL), TAG, "RX callbacks");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(speaker), TAG, "Enable TX clocks");
    return i2s_channel_enable(mic);
}

static esp_err_t init_codecs(void)
{
    audio_codec_i2s_cfg_t i2s = {.port = I2S_NUM_0, .rx_handle = mic, .tx_handle = speaker};
    data_if = audio_codec_new_i2s_data(&i2s);
    gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t control = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = amoled_i2c_bus(),
    };
    dac_ctrl = audio_codec_new_i2c_ctrl(&control);
    control.addr = ES7210_CODEC_DEFAULT_ADDR;
    adc_ctrl = audio_codec_new_i2c_ctrl(&control);
    if (!data_if || !gpio_if || !dac_ctrl || !adc_ctrl) return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t dac_config = {
        .ctrl_if = dac_ctrl, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_PIN, .pa_reverted = false, .master_mode = false, .use_mclk = true,
        .hw_gain = {.pa_voltage = 3.3, .codec_dac_voltage = 3.3},
    };
    // Physical inputs 1/2 are microphones; input 3 is analog playback reference.
    es7210_codec_cfg_t adc_config = {.ctrl_if = adc_ctrl, .mic_selected = PHYSICAL_MIC_MASK};
    dac_if = es8311_codec_new(&dac_config);
    adc_if = es7210_codec_new(&adc_config);
    if (!dac_if || !adc_if) return ESP_ERR_NO_MEM;
    esp_codec_dev_cfg_t device = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac_if, .data_if = data_if};
    dac = esp_codec_dev_new(&device);
    device.dev_type = ESP_CODEC_DEV_TYPE_IN;
    device.codec_if = adc_if;
    adc = esp_codec_dev_new(&device);
    if (!dac || !adc) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t output = {
        .sample_rate = SUGARAT_SAMPLE_RATE, .bits_per_sample = 16, .channel = 2, .mclk_multiple = 256,
    };
    esp_codec_dev_sample_info_t input = {
        .sample_rate = SUGARAT_SAMPLE_RATE, .bits_per_sample = 16, .channel = TDM_SLOTS,
        .channel_mask = 0x0f, .mclk_multiple = 256,
    };
    // Open TX before RX; esp_codec_dev expands TX slots to 32 bits so both sides
    // share a 64-fs BCLK (2x32 STD TX / 4x16 TDM RX), without changing PCM packing.
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(dac, &output), TAG, "Open ES8311");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(dac, 100), TAG, "DAC reference level");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_mute(dac, false), TAG, "Unmute DAC");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(adc, &input), TAG, "Open ES7210");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_channel_gain(adc, 0x03, CONFIG_AMOLED_MIC_GAIN_DB), TAG, "Mic gain");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_channel_gain(adc, 0x04, CONFIG_AMOLED_REFERENCE_GAIN_DB), TAG, "Reference gain");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_mute(adc, false), TAG, "Unmute microphones");
    sugarat_speaker_enable(false);
    return ESP_OK;
}

static esp_err_t init_aec(void)
{
    aec_config_t config = {
        .mic_num = 1, .ref_num = 1, .out_num = 1, .filter_length = 4,
        .sample_rate = SUGARAT_SAMPLE_RATE, .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode = AEC_MODE_FD_LOW_COST, .nlp_level = AEC_NLP_LEVEL_NORMAL,
    };
    aec = aec_create_from_config(&config);
    if (!aec) return ESP_ERR_NO_MEM;
    const int chunk = aec_get_chunksize(aec);
    if (chunk <= 0 || chunk > 1024) return ESP_ERR_NOT_SUPPORTED;
    chunk_samples = chunk;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    raw = heap_caps_aligned_alloc(16, chunk_samples * TDM_SLOTS * sizeof(int16_t), caps);
    near_end = heap_caps_aligned_alloc(16, chunk_samples * sizeof(int16_t), caps);
    reference = heap_caps_aligned_alloc(16, chunk_samples * sizeof(int16_t), caps);
    clean = heap_caps_aligned_alloc(16, chunk_samples * sizeof(int16_t), caps);
    if (!raw || !near_end || !reference || !clean) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "AEC full duplex ready: mode=FD_LOW_COST, 16kHz, dual-mic mix, analog reference, chunk=%u, mic_gain=%ddB ref_gain=%ddB",
             (unsigned)chunk_samples, CONFIG_AMOLED_MIC_GAIN_DB, CONFIG_AMOLED_REFERENCE_GAIN_DB);
    return ESP_OK;
}

esp_err_t sugarat_audio_init(void)
{
    if (aec || mic || speaker) return ESP_ERR_INVALID_STATE;
    esp_err_t err = amoled_board_init();
    if (err == ESP_OK) err = init_i2s();
    if (err == ESP_OK) err = init_codecs();
    if (err == ESP_OK) err = init_aec();
    if (err != ESP_OK) sugarat_audio_deinit();
    return err;
}

void sugarat_audio_deinit(void)
{
    sugarat_speaker_enable(false);
    if (adc) { (void)esp_codec_dev_close(adc); esp_codec_dev_delete(adc); adc = NULL; }
    if (dac) { (void)esp_codec_dev_close(dac); esp_codec_dev_delete(dac); dac = NULL; }
    if (adc_if) { (void)audio_codec_delete_codec_if(adc_if); adc_if = NULL; }
    if (dac_if) { (void)audio_codec_delete_codec_if(dac_if); dac_if = NULL; }
    if (adc_ctrl) { (void)audio_codec_delete_ctrl_if(adc_ctrl); adc_ctrl = NULL; }
    if (dac_ctrl) { (void)audio_codec_delete_ctrl_if(dac_ctrl); dac_ctrl = NULL; }
    if (gpio_if) { (void)audio_codec_delete_gpio_if(gpio_if); gpio_if = NULL; }
    if (data_if) { (void)audio_codec_delete_data_if(data_if); data_if = NULL; }
    if (mic) { (void)i2s_channel_disable(mic); (void)i2s_del_channel(mic); mic = NULL; }
    if (speaker) { (void)i2s_channel_disable(speaker); (void)i2s_del_channel(speaker); speaker = NULL; }
    if (aec) { aec_destroy(aec); aec = NULL; }
    heap_caps_free(raw); raw = NULL;
    heap_caps_free(near_end); near_end = NULL;
    heap_caps_free(reference); reference = NULL;
    heap_caps_free(clean); clean = NULL;
    raw_filled = output_at = output_remaining = chunk_samples = 0;
    next_capture_yield_us = 0;
    memset(&diagnostic, 0, sizeof(diagnostic));
}

static void schedule_capture(void)
{
    const int64_t now = esp_timer_get_time();
    if (!next_capture_yield_us) next_capture_yield_us = now + CAPTURE_RUN_BUDGET_US;
    if (now < next_capture_yield_us) return;
    // A queued I2S read and a buffered AEC output can both return immediately.
    // A real blocking tick lets lower-priority/idle tasks run even while catching
    // up. DMA, ADC, shared clocks and the adaptive filter remain running; no PCM
    // is discarded or replaced with silence. At the current 100Hz rate a tick
    // is 10ms; the budget is checked between reads, about once per 100ms.
    vTaskDelay(1);
    next_capture_yield_us = esp_timer_get_time() + CAPTURE_RUN_BUDGET_US;
    ++diagnostic.capture_yields;
}

static unsigned absolute(int16_t sample)
{
    return sample < 0 ? -(int32_t)sample : sample;
}

static void process_microphones(void)
{
    for (size_t i = 0; i < chunk_samples; ++i) {
        const int16_t first = raw[i * TDM_SLOTS + MIC1_SLOT];
        const int16_t second = raw[i * TDM_SLOTS + MIC2_SLOT];
        near_end[i] = ((int32_t)first + second) / 2;
        reference[i] = raw[i * TDM_SLOTS + REFERENCE_SLOT];
        diagnostic.mic_abs += absolute(near_end[i]);
        diagnostic.ref_abs += absolute(reference[i]);
        diagnostic.mic_clipped += absolute(first) >= 32760 || absolute(second) >= 32760;
        diagnostic.ref_clipped += absolute(reference[i]) >= 32760;
    }
    const int64_t started = esp_timer_get_time();
    // Always process real microphone/reference samples, including during playback
    // and silence. Never reset the adaptive filter at a reply/call boundary.
    aec_process(aec, near_end, reference, clean);
    const unsigned elapsed = (unsigned)(esp_timer_get_time() - started);
    diagnostic.total_aec_us += elapsed;
    ++diagnostic.aec_blocks;
    if (elapsed > chunk_samples * 1000000ULL / SUGARAT_SAMPLE_RATE) ++diagnostic.over_budget_blocks;
    if (elapsed > diagnostic.max_aec_us) diagnostic.max_aec_us = elapsed;
    for (size_t i = 0; i < chunk_samples; ++i) diagnostic.clean_abs += absolute(clean[i]);
    diagnostic.samples += chunk_samples;
    const int64_t now = esp_timer_get_time();
    if (!diagnostic.started_us) diagnostic.started_us = now;
    if (now - diagnostic.started_us >= DIAGNOSTIC_US) {
        ESP_LOGI(TAG, "AEC continuous: mode=FD_LOW_COST mic=%u ref=%u clean=%u clipped_mic=%u clipped_ref=%u samples=%u "
                      "avg_aec_us=%u max_aec_us=%u over_budget=%u/%u capture_yields=%u rx_overflows=%u",
                 (unsigned)(diagnostic.mic_abs / diagnostic.samples),
                 (unsigned)(diagnostic.ref_abs / diagnostic.samples),
                 (unsigned)(diagnostic.clean_abs / diagnostic.samples), diagnostic.mic_clipped,
                 diagnostic.ref_clipped, diagnostic.samples,
                 (unsigned)(diagnostic.total_aec_us / diagnostic.aec_blocks), diagnostic.max_aec_us,
                 diagnostic.over_budget_blocks, diagnostic.aec_blocks, diagnostic.capture_yields,
                 atomic_exchange(&rx_overflows, 0));
        memset(&diagnostic, 0, sizeof(diagnostic));
        diagnostic.started_us = now;
    }
}

esp_err_t sugarat_mic_read(int16_t *pcm, size_t *samples)
{
    if (!pcm || !samples) return ESP_ERR_INVALID_ARG;
    *samples = 0;
    if (!aec || !mic) return ESP_ERR_INVALID_STATE;
    schedule_capture();
    if (!output_remaining) {
        const size_t bytes = chunk_samples * TDM_SLOTS * sizeof(*raw);
        size_t received = 0;
        const esp_err_t err = i2s_channel_read(mic, (uint8_t *)raw + raw_filled,
                                              bytes - raw_filled, &received, 60);
        raw_filled += received;
        // Retain partial reads, preserving the reference/microphone slot alignment.
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            raw_filled = 0;
            return err;
        }
        if (raw_filled != bytes) return ESP_OK;
        raw_filled = 0;
        process_microphones();
        output_at = 0;
        output_remaining = chunk_samples;
    }
    *samples = output_remaining < SUGARAT_AUDIO_FRAME ? output_remaining : SUGARAT_AUDIO_FRAME;
    memcpy(pcm, clean + output_at, *samples * sizeof(*pcm));
    output_at += *samples;
    output_remaining -= *samples;
    return ESP_OK;
}

esp_err_t sugarat_speaker_write(const int16_t *pcm, size_t samples)
{
    if (!speaker) return ESP_ERR_INVALID_STATE;
    if (!pcm || samples > SUGARAT_AUDIO_FRAME) return ESP_ERR_INVALID_ARG;
    int16_t stereo[SUGARAT_AUDIO_FRAME * 2];
    const unsigned level = atomic_load(&volume);
    for (size_t i = 0; i < samples; ++i) {
        stereo[i * 2] = stereo[i * 2 + 1] = (int32_t)pcm[i] * (int32_t)level / 100;
    }
    size_t written = 0;
    const size_t bytes = samples * 2 * sizeof(*stereo);
    const esp_err_t err = i2s_channel_write(speaker, stereo, bytes, &written, 100);
    return err == ESP_OK && written != bytes ? ESP_FAIL : err;
}

void sugarat_speaker_set_volume(unsigned percent)
{
    // The ADC reference captures the actual DAC output after this volume scaling.
    atomic_store(&volume, percent > 100 ? 100 : percent);
}

void sugarat_speaker_enable(bool enabled)
{
    // Keep codecs, all shared clocks, and RX running; only gate the output PA.
    if (gpio_if) (void)gpio_if->set(PA_PIN, enabled);
}
