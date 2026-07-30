#include "driver/es8389.h"

#include "driver/audio_passthrough.h"
#include "driver/board_pins.h"
#include "driver/external_radio.h"
#include "driver/i2c1.h"

#if defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_common.h>
#include <driver/i2s_types.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_err.h>
#include <esp_log.h>

static const char *TAG = "ES8389";

namespace {

constexpr int kI2sPort = I2S_NUM_0;
constexpr int kSampleRate = 16000;
constexpr int kChannels = 2;
constexpr int kChannelMask = 0x03;
constexpr int kDefaultOutVolume = 70;
constexpr float kDefaultInGain = 30.0f;

static bool s_es8389_ready = false;
static bool s_hifi_active = false;
static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static const audio_codec_ctrl_if_t *s_ctrl_if = nullptr;
static const audio_codec_data_if_t *s_data_if = nullptr;
static const audio_codec_gpio_if_t *s_gpio_if = nullptr;
static const audio_codec_if_t *s_codec_if = nullptr;
static esp_codec_dev_handle_t s_codec = nullptr;

static bool es8389_init_i2c(void) {
    if (s_i2c_bus != nullptr) {
        return true;
    }

    if (!I2C_MasterGetBus(&s_i2c_bus)) {
        ESP_LOGE(TAG, "shared I2C bus unavailable");
        return false;
    }
    return true;
}

static bool es8389_create_codec(i2s_chan_handle_t tx_handle, i2s_chan_handle_t rx_handle) {
    if (s_codec != nullptr) {
        return true;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_0;
    i2c_cfg.addr = ES8389_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = s_i2c_bus;
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == nullptr) {
        ESP_LOGE(TAG, "create I2C control interface failed");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = static_cast<uint8_t>(kI2sPort);
    i2s_cfg.tx_handle = tx_handle;
    i2s_cfg.rx_handle = rx_handle;
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == nullptr) {
        ESP_LOGE(TAG, "create I2S data interface failed");
        return false;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == nullptr) {
        ESP_LOGE(TAG, "create GPIO interface failed");
        return false;
    }

    esp_codec_dev_hw_gain_t gain = {};
    gain.pa_voltage = 5.0f;
    gain.codec_dac_voltage = 3.3f;

    es8389_codec_cfg_t es8389_cfg = {};
    es8389_cfg.ctrl_if = s_ctrl_if;
    es8389_cfg.gpio_if = s_gpio_if;
    es8389_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8389_cfg.pa_pin = static_cast<int16_t>(NRL_PIN_PA_EN);
    es8389_cfg.pa_reverted = false;
    es8389_cfg.master_mode = false;
    es8389_cfg.use_mclk = false;
    es8389_cfg.digital_mic = false;
    es8389_cfg.invert_mclk = false;
    es8389_cfg.invert_sclk = false;
    es8389_cfg.hw_gain = gain;
    es8389_cfg.no_dac_ref = false;

    s_codec_if = es8389_codec_new(&es8389_cfg);
    if (s_codec_if == nullptr) {
        ESP_LOGE(TAG, "create ES8389 codec interface failed");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    // The audio passthrough owns the ES8389 RX channel directly. Keep the
    // codec-dev wrapper output-only so hi-fi playback can reconfigure TX
    // without also changing RX to a music rate and breaking mic capture.
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = s_codec_if;
    dev_cfg.data_if = s_data_if;
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (s_codec == nullptr) {
        ESP_LOGE(TAG, "create codec device failed");
        return false;
    }

    return true;
}

// Re-apply the persisted volume after every esp_codec_dev_open (the codec-dev
// layer resets its notion of volume on close).
static void es8389_apply_config_levels(void) {
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config != nullptr) {
        ES8389_SetOutputVolume(config->line_out_volume);
        ES8389_SetInputGain(config->mic_volume);
    } else {
        (void)esp_codec_dev_set_out_vol(s_codec, kDefaultOutVolume);
        if (s_codec_if != nullptr && s_codec_if->set_mic_gain != nullptr) {
            (void)s_codec_if->set_mic_gain(s_codec_if, kDefaultInGain);
        }
    }
}

static bool i2s_channel_enabled(const i2s_chan_handle_t channel) {
    if (channel == nullptr) {
        return false;
    }
    i2s_chan_info_t info = {};
    return i2s_channel_get_info(channel, &info) == ESP_OK && info.is_enabled;
}

static void es8389_enable_i2s_for_codec_open(void) {
    i2s_chan_handle_t tx_handle = nullptr;
    i2s_chan_handle_t rx_handle = nullptr;
    if (!AUDIO_GetI2SHandles(&tx_handle, &rx_handle)) {
        return;
    }
    if (tx_handle != nullptr && !i2s_channel_enabled(tx_handle)) {
        (void)i2s_channel_enable(tx_handle);
    }
    if (rx_handle != nullptr && !i2s_channel_enabled(rx_handle)) {
        (void)i2s_channel_enable(rx_handle);
    }
}

static bool es8389_open_codec(void) {
    // esp_codec_dev_open()->set_fmt() unconditionally disables the I2S
    // channels before reconfiguring them. After a prior esp_codec_dev_close()
    // they are already disabled, which otherwise prints noisy
    // "channel has not been enabled yet" errors for both TX and RX.
    es8389_enable_i2s_for_codec_open();

    esp_codec_dev_sample_info_t sample_cfg = {};
    sample_cfg.bits_per_sample = 16;
    sample_cfg.channel = kChannels;
    sample_cfg.channel_mask = kChannelMask;
    sample_cfg.sample_rate = kSampleRate;
    sample_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (esp_codec_dev_open(s_codec, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "open codec failed");
        return false;
    }
    if (esp_codec_dev_set_out_vol(s_codec, kDefaultOutVolume) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set output volume failed");
        return false;
    }
    return true;
}

} // namespace

extern "C" bool ES8389_Init(void) {
    if (s_es8389_ready) {
        return AUDIO_StartPassthrough();
    }

    AUDIO_SetMode(AUDIO_MODE_RECEIVE);

    if (NRL_PIN_PA_EN >= 0) {
        gpio_reset_pin(static_cast<gpio_num_t>(NRL_PIN_PA_EN));
        gpio_set_direction(static_cast<gpio_num_t>(NRL_PIN_PA_EN), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_PA_EN), 1);
    }

    if (!AUDIO_SetupI2S()) {
        ESP_LOGE(TAG, "I2S setup failed");
        return false;
    }

    i2s_chan_handle_t tx_handle = nullptr;
    i2s_chan_handle_t rx_handle = nullptr;
    if (!AUDIO_GetI2SHandles(&tx_handle, &rx_handle)) {
        ESP_LOGE(TAG, "I2S handles unavailable");
        return false;
    }

    if (!es8389_init_i2c() || !es8389_create_codec(tx_handle, rx_handle) || !es8389_open_codec()) {
        return false;
    }

    if (!AUDIO_StartPassthrough()) {
        ESP_LOGE(TAG, "start passthrough failed");
        return false;
    }

    s_es8389_ready = true;
    ESP_LOGI(TAG, "ready: i2c=0x%02X", ES8389_CODEC_DEFAULT_ADDR);
    return true;
}

extern "C" bool ES8389_IsReady(void) {
    return s_es8389_ready;
}

extern "C" bool ES8389_SetReceiveMode(void) {
    return ES8389_Init();
}

extern "C" bool ES8389_SetOutputVolume(const uint8_t value) {
    if (s_codec == nullptr) {
        return false;
    }
    const int percent = (static_cast<int>(value) * 100 + 127) / 255;
    return esp_codec_dev_set_out_vol(s_codec, percent) == ESP_CODEC_DEV_OK;
}

extern "C" bool ES8389_SetInputGain(const uint8_t value) {
    if (s_codec_if == nullptr || s_codec_if->set_mic_gain == nullptr) {
        return false;
    }
    // Keep codec-dev output-only so hi-fi playback can change TX clocks
    // without reconfiguring the live RX channel. The ES8389 hardware
    // interface is nevertheless opened in BOTH mode, so program its ADC PGA
    // directly instead of using esp_codec_dev_set_in_gain(), which rejects an
    // output-only device handle.
    const unsigned step = (static_cast<unsigned>(value) * 14u + 127u) / 255u;
    const float gain_db = static_cast<float>(step) * 3.0f;
    const bool ok = s_codec_if->set_mic_gain(s_codec_if, gain_db) == ESP_CODEC_DEV_OK;
    ESP_LOGI(TAG, "mic gain: volume=%u -> %.1f dB%s",
             static_cast<unsigned>(value),
             static_cast<double>(gain_db),
             ok ? "" : " FAILED");
    return ok;
}

extern "C" bool ES8389_HifiAcquire(const uint32_t sample_rate_hz,
                                   const uint8_t bits_per_sample,
                                   const uint8_t channels) {
    if (!s_es8389_ready || s_codec == nullptr || s_hifi_active) {
        return false;
    }
    if (sample_rate_hz < 8000u || sample_rate_hz > 192000u ||
        (bits_per_sample != 16u && bits_per_sample != 24u && bits_per_sample != 32u) ||
        (channels != 1u && channels != 2u)) {
        return false;
    }

    // Voice passthrough owns the I2S DMA while it runs; stop it before the
    // codec-dev reopen reconfigures the channel clocks underneath it.
    AUDIO_StopPassthrough();

    if (esp_codec_dev_close(s_codec) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "hifi: close codec failed");
        (void)AUDIO_StartPassthrough();
        return false;
    }

    esp_codec_dev_sample_info_t sample_cfg = {};
    sample_cfg.bits_per_sample = bits_per_sample;
    sample_cfg.channel = channels;
    sample_cfg.channel_mask = (channels == 2u) ? 0x03 : 0x01;
    sample_cfg.sample_rate = sample_rate_hz;
    sample_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    es8389_enable_i2s_for_codec_open();
    if (esp_codec_dev_open(s_codec, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "hifi: open %luHz/%ubit/%uch failed",
                 static_cast<unsigned long>(sample_rate_hz),
                 static_cast<unsigned>(bits_per_sample),
                 static_cast<unsigned>(channels));
        // Try to fall back to the voice format so the radio keeps working.
        (void)es8389_open_codec();
        es8389_apply_config_levels();
        (void)AUDIO_StartPassthrough();
        return false;
    }
    es8389_apply_config_levels();

    s_hifi_active = true;
    ESP_LOGI(TAG, "hifi: acquired %luHz/%ubit/%uch",
             static_cast<unsigned long>(sample_rate_hz),
             static_cast<unsigned>(bits_per_sample),
             static_cast<unsigned>(channels));
    return true;
}

extern "C" bool ES8389_HifiWrite(const void *pcm, const size_t bytes) {
    if (!s_hifi_active || s_codec == nullptr || pcm == nullptr || bytes == 0u) {
        return false;
    }
    return esp_codec_dev_write(s_codec, const_cast<void *>(pcm), bytes) == ESP_CODEC_DEV_OK;
}

extern "C" bool ES8389_HifiRelease(void) {
    if (!s_hifi_active) {
        return true;
    }
    s_hifi_active = false;

    (void)esp_codec_dev_close(s_codec);
    const bool reopened = es8389_open_codec();
    if (!reopened) {
        ESP_LOGE(TAG, "hifi: restore voice format failed");
    }
    es8389_apply_config_levels();
    const bool restarted = AUDIO_StartPassthrough();
    ESP_LOGI(TAG, "hifi: released (voice path %s)",
             (reopened && restarted) ? "restored" : "RESTORE FAILED");
    return reopened && restarted;
}

extern "C" bool ES8389_HifiActive(void) {
    return s_hifi_active;
}

#else

extern "C" bool ES8389_Init(void) {
    return false;
}

extern "C" bool ES8389_IsReady(void) {
    return false;
}

extern "C" bool ES8389_SetReceiveMode(void) {
    return false;
}

extern "C" bool ES8389_SetOutputVolume(uint8_t) {
    return false;
}

extern "C" bool ES8389_SetInputGain(uint8_t) {
    return false;
}

extern "C" bool ES8389_HifiAcquire(uint32_t, uint8_t, uint8_t) {
    return false;
}

extern "C" bool ES8389_HifiWrite(const void *, size_t) {
    return false;
}

extern "C" bool ES8389_HifiRelease(void) {
    return true;
}

extern "C" bool ES8389_HifiActive(void) {
    return false;
}

#endif
