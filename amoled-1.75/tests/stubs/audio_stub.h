#pragma once
#include "idf_stub.h"

#define CONFIG_AMOLED_MIC_GAIN_DB 24
#define CONFIG_AMOLED_REFERENCE_GAIN_DB 0
enum { GPIO_NUM_NC = -1, GPIO_NUM_8 = 8, GPIO_NUM_9 = 9, GPIO_NUM_10 = 10,
       GPIO_NUM_42 = 42, GPIO_NUM_45 = 45, GPIO_NUM_46 = 46,
       I2C_NUM_0 = 0, I2S_NUM_0 = 0, I2S_ROLE_MASTER, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
       I2S_MCLK_MULTIPLE_256 = 256, I2S_TDM_SLOT0 = 1, I2S_TDM_SLOT1 = 2,
       I2S_TDM_SLOT2 = 4, I2S_TDM_SLOT3 = 8,
       MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4, MALLOC_CAP_DMA = 8 };
size_t heap_caps_get_free_size(uint32_t caps);
typedef void *i2s_chan_handle_t;
typedef struct { int unused; } i2s_event_data_t;
typedef struct { unsigned dma_desc_num, dma_frame_num; bool auto_clear_after_cb; } i2s_chan_config_t;
typedef struct { unsigned sample_rate_hz, mclk_multiple, bclk_div; } mock_clock_t;
typedef struct { unsigned total_slot; } mock_slots_t;
typedef struct { int mclk, bclk, ws, dout, din; } mock_pins_t;
typedef struct { mock_clock_t clk_cfg; mock_slots_t slot_cfg; mock_pins_t gpio_cfg; } i2s_std_config_t;
typedef i2s_std_config_t i2s_tdm_config_t;
typedef bool (*mock_i2s_callback_t)(i2s_chan_handle_t, i2s_event_data_t *, void *);
typedef struct { mock_i2s_callback_t on_sent, on_send_q_ovf, on_recv_q_ovf; } i2s_event_callbacks_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(...) {0}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {.sample_rate_hz = (rate)}
#define I2S_TDM_CLK_DEFAULT_CONFIG(rate) I2S_STD_CLK_DEFAULT_CONFIG(rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(...) {0}
#define I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(...) {0}
esp_err_t i2s_new_channel(const i2s_chan_config_t *, i2s_chan_handle_t *, i2s_chan_handle_t *);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t *);
esp_err_t i2s_channel_init_tdm_mode(i2s_chan_handle_t, const i2s_tdm_config_t *);
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t, const i2s_event_callbacks_t *, void *);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, uint32_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t, const void *, size_t, size_t *, uint32_t);
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps);
void heap_caps_free(void *data);

enum { AEC_MODE_FD_LOW_COST = 5, AEC_MODE_FD_HIGH_PERF = 6, AEC_NLP_LEVEL_NORMAL = 0 };
typedef struct {
    int mic_num, ref_num, out_num, filter_length, sample_rate;
    uint32_t caps;
    int mode, nlp_level;
} aec_config_t;
typedef struct { int frame_size; aec_config_t config; } aec_handle_t;
aec_handle_t *aec_create_from_config(aec_config_t *);
int aec_get_chunksize(const aec_handle_t *);
void aec_process(const aec_handle_t *, int16_t *, int16_t *, int16_t *);
void aec_destroy(aec_handle_t *);

typedef struct { int unused; } audio_codec_data_if_t;
typedef struct { int unused; } audio_codec_ctrl_if_t;
typedef struct { int unused; } audio_codec_if_t;
typedef struct { int (*set)(int16_t, bool); } audio_codec_gpio_if_t;
typedef void *esp_codec_dev_handle_t;
typedef struct { int port; i2s_chan_handle_t rx_handle, tx_handle; } audio_codec_i2s_cfg_t;
typedef struct { int port, addr; i2c_master_bus_handle_t bus_handle; } audio_codec_i2c_cfg_t;
typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    int codec_mode, pa_pin;
    bool pa_reverted, master_mode, use_mclk;
    struct { double pa_voltage, codec_dac_voltage; } hw_gain;
} es8311_codec_cfg_t;
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; int mic_selected; } es7210_codec_cfg_t;
typedef struct { int dev_type; const audio_codec_if_t *codec_if; const audio_codec_data_if_t *data_if; } esp_codec_dev_cfg_t;
typedef struct { int sample_rate, bits_per_sample, channel, mclk_multiple, channel_mask; } esp_codec_dev_sample_info_t;
enum { ES8311_CODEC_DEFAULT_ADDR = 0x30, ES7210_CODEC_DEFAULT_ADDR = 0x80,
       ESP_CODEC_DEV_WORK_MODE_DAC, ESP_CODEC_DEV_TYPE_IN, ESP_CODEC_DEV_TYPE_OUT };
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *);
const audio_codec_gpio_if_t *audio_codec_new_gpio(void);
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *);
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *);
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *);
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *);
int esp_codec_dev_open(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t *);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int);
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t, bool);
int esp_codec_dev_set_in_mute(esp_codec_dev_handle_t, bool);
int esp_codec_dev_set_in_channel_gain(esp_codec_dev_handle_t, uint16_t, float);
int esp_codec_dev_close(esp_codec_dev_handle_t);
void esp_codec_dev_delete(esp_codec_dev_handle_t);
int audio_codec_delete_codec_if(const audio_codec_if_t *);
int audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *);
int audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *);
int audio_codec_delete_data_if(const audio_codec_data_if_t *);
