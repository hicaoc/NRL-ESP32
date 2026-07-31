#include "audio/voice_resampler.h"

#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

#include "esp_ae_ch_cvt.h"
#include "esp_ae_rate_cvt.h"

namespace {
constexpr uint32_t kVoiceRateHz = 8000u;
} // namespace

extern "C" void VOICE_RESAMPLER_Deinit(VoiceResampler *rs)
{
    if (rs == nullptr) {
        return;
    }
    if (rs->ch_cvt != nullptr) {
        esp_ae_ch_cvt_close(rs->ch_cvt);
        rs->ch_cvt = nullptr;
    }
    if (rs->rate_cvt != nullptr) {
        esp_ae_rate_cvt_close(rs->rate_cvt);
        rs->rate_cvt = nullptr;
    }
    free(rs->mono_buf);
    rs->mono_buf = nullptr;
    rs->mono_cap_frames = 0;
}

extern "C" int VOICE_RESAMPLER_Init(VoiceResampler *rs, const uint32_t in_rate_hz, const uint8_t channels)
{
    if (rs == nullptr || in_rate_hz < kVoiceRateHz || in_rate_hz > 192000u ||
        (channels != 1u && channels != 2u)) {
        return 0;
    }
    VOICE_RESAMPLER_Deinit(rs);
    rs->in_rate_hz = in_rate_hz;
    rs->channels = channels;

    if (channels == 2u) {
        // Default weights (1/src_ch per channel) give the same (L+R)/2
        // downmix the old hand-written resampler used.
        esp_ae_ch_cvt_cfg_t ch_cfg = {};
        ch_cfg.sample_rate = in_rate_hz;
        ch_cfg.bits_per_sample = 16u;
        ch_cfg.src_ch = 2u;
        ch_cfg.dest_ch = 1u;
        if (esp_ae_ch_cvt_open(&ch_cfg, &rs->ch_cvt) != ESP_AE_ERR_OK) {
            VOICE_RESAMPLER_Deinit(rs);
            return 0;
        }
    }

    // Internal SRAM is scarce (WiFi/BT/ESP-NOW/LVGL), so pick the low-IRAM
    // variant; the player task has CPU headroom on either core.
    esp_ae_rate_cvt_cfg_t rate_cfg = {};
    rate_cfg.src_rate = in_rate_hz;
    rate_cfg.dest_rate = kVoiceRateHz;
    rate_cfg.channel = 1u;
    rate_cfg.bits_per_sample = 16u;
    rate_cfg.complexity = 1u; // voice-grade 8 kHz telephony output
    rate_cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY;
    if (esp_ae_rate_cvt_open(&rate_cfg, &rs->rate_cvt) != ESP_AE_ERR_OK) {
        VOICE_RESAMPLER_Deinit(rs);
        return 0;
    }
    return 1;
}

extern "C" size_t VOICE_RESAMPLER_Process(VoiceResampler *rs,
                                          const int16_t *in, const size_t in_frames,
                                          int16_t *out, const size_t out_capacity)
{
    if (rs == nullptr || in == nullptr || out == nullptr || in_frames == 0u ||
        rs->rate_cvt == nullptr || out_capacity == 0u) {
        return 0;
    }

    const int16_t *mono = in;
    if (rs->channels == 2u) {
        if (in_frames > rs->mono_cap_frames) {
            int16_t *buf = static_cast<int16_t *>(
                heap_caps_malloc(in_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM));
            if (buf == nullptr) {
                return 0;
            }
            free(rs->mono_buf);
            rs->mono_buf = buf;
            rs->mono_cap_frames = in_frames;
        }
        if (esp_ae_ch_cvt_process(rs->ch_cvt, static_cast<uint32_t>(in_frames),
                                  const_cast<int16_t *>(in), rs->mono_buf) != ESP_AE_ERR_OK) {
            return 0;
        }
        mono = rs->mono_buf;
    }

    uint32_t produced = static_cast<uint32_t>(out_capacity);
    if (esp_ae_rate_cvt_process(rs->rate_cvt, const_cast<int16_t *>(mono),
                                static_cast<uint32_t>(in_frames), out,
                                &produced) != ESP_AE_ERR_OK) {
        return 0;
    }
    return produced;
}
