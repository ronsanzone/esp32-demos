// Microphone bring-up: ES7210 ADC (I2C 0x40) on I2S0, configured the way Waveshare's audio test does it
// (TDM, all four ES7210 inputs enabled, opened as 16-bit stereo). The ES8311 speaker DAC is left off.
#include <driver/i2s_tdm.h>
#include <esp_log.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "audio.h"
#include "app.h"

static const char *TAG = "audio";

#define PIN_MCLK 19
#define PIN_BCLK 20
#define PIN_WS   22
#define PIN_DOUT 23
#define PIN_DIN  21

static esp_codec_dev_handle_t mic;

bool audio_mic_open(int sample_rate, float gain_db)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    if (i2s_new_channel(&chan, NULL, &rx) != ESP_OK) return false;
    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO,
                                                         I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {.mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN},
    };
    tdm.slot_cfg.total_slot = 4;
    if (i2s_channel_init_tdm_mode(rx, &tdm) != ESP_OK) return false;
    i2s_channel_enable(rx);                       // the ES7210 needs MCLK running before it is configured

    audio_codec_i2s_cfg_t i2s_cfg = {.port = 0, .rx_handle = rx, .tx_handle = NULL};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = app_i2c_bus()};
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es = {.ctrl_if = ctrl, .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4};
    const audio_codec_if_t *codec = es7210_codec_new(&es);
    if (!data_if || !ctrl || !codec) {
        ESP_LOGE(TAG, "codec interfaces failed");
        return false;
    }
    esp_codec_dev_cfg_t dev = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if};
    mic = esp_codec_dev_new(&dev);
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 2, .sample_rate = sample_rate};
    if (!mic || esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open failed");
        return false;
    }
    esp_codec_dev_set_in_gain(mic, gain_db);
    ESP_LOGI(TAG, "mic open: %d Hz, gain %.0f dB", sample_rate, gain_db);
    return true;
}

int audio_mic_read(int16_t *stereo, int frames)
{
    return esp_codec_dev_read(mic, stereo, frames * 4) == ESP_CODEC_DEV_OK ? frames : 0;
}
