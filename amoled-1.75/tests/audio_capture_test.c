// Exercise production audio.c with deterministic DMA, AEC and clock substitutes.
#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_stub.h"
#include "../components/sugarat_board/audio.c"

#define MOCK_TX ((void *)1)
#define MOCK_RX ((void *)2)
#define MOCK_DAC ((void *)3)
#define MOCK_ADC ((void *)4)
static int64_t clock_us, previous_yield_us;
static unsigned delay_calls, read_calls, disable_calls, aec_calls;
static unsigned aec_work_us = 8000, read_work_us = 100, input_mute_calls;
static size_t dma_byte_at, read_limit;
static bool rx_enabled, tx_enabled, input_muted, pa_enabled, fail_read;

int64_t esp_timer_get_time(void) { return clock_us; }
const char *esp_err_to_name(esp_err_t err) { return "mock error"; }
void vTaskDelay(unsigned ticks)
{
    assert(ticks == 1 && rx_enabled && tx_enabled && !input_muted);
    if (previous_yield_us) {
        // The budget is checked at read boundaries, with at most one processing
        // block of overshoot. A watchdog-scale continuous run must not occur.
        assert(clock_us - previous_yield_us <= 100000 + aec_work_us + read_work_us + 10000);
    }
    ++delay_calls;
    clock_us += 10000; // Current firmware tick is 100Hz.
    previous_yield_us = clock_us;
}
esp_err_t amoled_board_init(void) { return ESP_OK; }
i2c_master_bus_handle_t amoled_i2c_bus(void) { return (void *)5; }
esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    assert(cfg->dma_desc_num == 6 && cfg->dma_frame_num == 320 && cfg->auto_clear_after_cb);
    *tx = MOCK_TX; *rx = MOCK_RX; return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *cfg)
{ assert(h == MOCK_TX && cfg->clk_cfg.sample_rate_hz == 16000); return ESP_OK; }
esp_err_t i2s_channel_init_tdm_mode(i2s_chan_handle_t h, const i2s_tdm_config_t *cfg)
{ assert(h == MOCK_RX && cfg->slot_cfg.total_slot == 4); return ESP_OK; }
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t h, const i2s_event_callbacks_t *cb, void *ctx)
{ return ESP_OK; }
esp_err_t i2s_channel_enable(i2s_chan_handle_t h)
{ if (h == MOCK_RX) rx_enabled = true; else tx_enabled = true; return ESP_OK; }
esp_err_t i2s_channel_disable(i2s_chan_handle_t h)
{ ++disable_calls; if (h == MOCK_RX) rx_enabled = false; else tx_enabled = false; return ESP_OK; }
esp_err_t i2s_del_channel(i2s_chan_handle_t h) { return ESP_OK; }
static uint8_t dma_byte(size_t position)
{
    const unsigned word = (unsigned)(position / 2), slot = word % 4, frame = word / 4 % 10000;
    const int16_t value = slot == 0 ? 1000 + frame : slot == 1 ? 100 : slot == 2 ? 3000 + frame : -1234;
    return (uint16_t)value >> ((position & 1) * 8);
}
esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dest, size_t bytes, size_t *received, uint32_t timeout)
{
    assert(h == MOCK_RX && rx_enabled && !input_muted && timeout == 60);
    ++read_calls; clock_us += read_work_us; *received = 0;
    if (fail_read) return ESP_FAIL;
    *received = read_limit && read_limit < bytes ? read_limit : bytes;
    for (size_t i = 0; i < *received; ++i) ((uint8_t *)dest)[i] = dma_byte(dma_byte_at++);
    return *received < bytes ? ESP_ERR_TIMEOUT : ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *data, size_t bytes, size_t *written, uint32_t timeout)
{ assert(h == MOCK_TX && tx_enabled); *written = bytes; return ESP_OK; }
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps) { return _aligned_malloc(bytes, alignment); }
void heap_caps_free(void *data) { _aligned_free(data); }
aec_handle_t *aec_create_from_config(aec_config_t *config)
{
    assert(config->mode == AEC_MODE_FD_LOW_COST && config->nlp_level == AEC_NLP_LEVEL_NORMAL);
    assert(config->filter_length == 4 && config->mic_num == 1 && config->ref_num == 1 && config->out_num == 1);
    aec_handle_t *handle = malloc(sizeof(*handle));
    assert(handle); handle->frame_size = 512; handle->config = *config; return handle;
}
int aec_get_chunksize(const aec_handle_t *h) { return h->frame_size; }
void aec_process(const aec_handle_t *h, int16_t *near, int16_t *ref, int16_t *output)
{
    assert(rx_enabled && tx_enabled && !input_muted);
    ++aec_calls; clock_us += aec_work_us;
    for (int i = 0; i < h->frame_size; ++i) {
        assert(ref[i] == 100); // Real reference slot must survive partial reads.
        output[i] = near[i] - ref[i];
    }
}
void aec_destroy(aec_handle_t *h) { free(h); }
static int pa_set(int16_t pin, bool enabled) { assert(pin == 46); pa_enabled = enabled; return ESP_OK; }
static const audio_codec_gpio_if_t mock_gpio = {.set = pa_set};
static const audio_codec_data_if_t mock_data;
static const audio_codec_ctrl_if_t mock_ctrl;
static const audio_codec_if_t mock_codec;
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *cfg) { return &mock_data; }
const audio_codec_gpio_if_t *audio_codec_new_gpio(void) { return &mock_gpio; }
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *cfg) { return &mock_ctrl; }
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *cfg) { return &mock_codec; }
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *cfg) { assert(cfg->mic_selected == 7); return &mock_codec; }
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *cfg)
{ return cfg->dev_type == ESP_CODEC_DEV_TYPE_IN ? MOCK_ADC : MOCK_DAC; }
int esp_codec_dev_open(esp_codec_dev_handle_t h, esp_codec_dev_sample_info_t *cfg) { return ESP_OK; }
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int volume) { return ESP_OK; }
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t h, bool muted) { return ESP_OK; }
int esp_codec_dev_set_in_mute(esp_codec_dev_handle_t h, bool muted)
{ assert(h == MOCK_ADC); ++input_mute_calls; input_muted = muted; return ESP_OK; }
int esp_codec_dev_set_in_channel_gain(esp_codec_dev_handle_t h, uint16_t mask, float gain) { return ESP_OK; }
int esp_codec_dev_close(esp_codec_dev_handle_t h) { return ESP_OK; }
void esp_codec_dev_delete(esp_codec_dev_handle_t h) {}
int audio_codec_delete_codec_if(const audio_codec_if_t *h) { return ESP_OK; }
int audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *h) { return ESP_OK; }
int audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *h) { return ESP_OK; }
int audio_codec_delete_data_if(const audio_codec_data_if_t *h) { return ESP_OK; }

static void start_fixture(void)
{
    clock_us = 1000; previous_yield_us = 0;
    delay_calls = read_calls = disable_calls = aec_calls = input_mute_calls = 0;
    dma_byte_at = read_limit = 0; fail_read = false;
    aec_work_us = 8000; read_work_us = 100;
    assert(sugarat_audio_init() == ESP_OK);
    assert(rx_enabled && tx_enabled && !input_muted && input_mute_calls == 1);
}
static size_t read_expected(size_t *expected)
{
    int16_t pcm[320]; size_t count = 999;
    assert(sugarat_mic_read(pcm, &count) == ESP_OK && count <= 320);
    for (size_t i = 0; i < count; ++i) assert(pcm[i] == 1900 + (*expected + i) % 10000);
    *expected += count;
    assert(!disable_calls && !input_muted && input_mute_calls == 1);
    return count;
}
static void budget_boundaries(void)
{
    start_fixture(); size_t expected = 0;
    assert(read_expected(&expected) == 320 && !delay_calls);
    const int64_t deadline = next_capture_yield_us;
    clock_us = deadline - 1;
    assert(read_expected(&expected) == 192 && !delay_calls);
    clock_us = deadline;
    assert(read_expected(&expected) == 320 && delay_calls == 1);
    unsigned reads = read_calls, processed = aec_calls;
    clock_us = next_capture_yield_us;
    assert(read_expected(&expected) == 192 && delay_calls == 2);
    assert(read_calls == reads && aec_calls == processed); // Buffered tail preserved across a yield.
    assert(diagnostic.capture_yields == 2);
    sugarat_audio_deinit(); assert(!rx_enabled && !tx_enabled && !next_capture_yield_us);
    puts("PASS: capture budget boundaries, blocking tick and buffered audio tail");
}
static void queued_audio_and_playback(void)
{
    start_fixture(); size_t expected = 0;
    for (unsigned i = 0; i < 240; ++i) {
        sugarat_speaker_enable((i % 3) == 0);
        assert(pa_enabled == ((i % 3) == 0));
        sugarat_speaker_set_volume(i % 101);
        assert(read_expected(&expected) == (i % 2 ? 192 : 320));
    }
    assert(expected == 120 * 512 && delay_calls >= 8 && aec_calls == 120);
    assert(diagnostic.total_aec_us == (uint64_t)aec_calls * aec_work_us);
    assert(!diagnostic.over_budget_blocks);
    sugarat_audio_deinit();
    puts("PASS: queued DMA audio yields regularly; 61440 samples preserved during speaker/volume changes");
}
static void partial_reads_and_recovery(void)
{
    start_fixture(); size_t expected = 0;
    read_limit = 1000; // Partial timeout fragments must not shift TDM/reference slots.
    while (expected < 5 * 512) (void)read_expected(&expected);
    assert(expected == 5 * 512 && read_calls == 25 && aec_calls == 5);
    read_limit = 0; fail_read = true;
    int16_t pcm[320]; size_t count = 99;
    assert(sugarat_mic_read(pcm, &count) == ESP_FAIL && !count && !raw_filled);
    fail_read = false;
    assert(read_expected(&expected) == 320);
    assert(read_expected(&expected) == 192);
    aec_work_us = 50000;
    assert(read_expected(&expected) == 320 && diagnostic.over_budget_blocks == 1);
    assert(diagnostic.max_aec_us == 50000);
    sugarat_audio_deinit();
    puts("PASS: partial reads, reference alignment, read-error recovery and over-budget diagnostics");
}
int main(void)
{
    budget_boundaries();
    queued_audio_and_playback();
    partial_reads_and_recovery();
    return 0;
}
