#include "media/opus_voice.h"

#include <esp_audio_types.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>

#include <string.h>

static const char *TAG = "OPUSV";

namespace {

// VBR working point per the voice-link spec (32-40 kbps effective).
constexpr int kBitrateBps = 40000;
constexpr int kComplexity = 10;

static esp_opus_enc_frame_duration_t enc_duration(const uint32_t frame_ms)
{
    switch (frame_ms) {
        case 20u: return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
        case 40u: return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
        case 60u: return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        default:  return ESP_OPUS_ENC_FRAME_DURATION_ARG;
    }
}

static esp_opus_dec_frame_duration_t dec_duration(const uint32_t frame_ms)
{
    switch (frame_ms) {
        case 20u: return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        case 40u: return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        case 60u: return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        default:  return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    }
}

} // namespace

struct OpusVoiceEnc {
    void *handle;
    size_t frame_samples;
};

struct OpusVoiceDec {
    void *handle;
};

extern "C" OpusVoiceEnc *OPUS_VOICE_EncOpenEx(const uint32_t sample_rate_hz,
                                               const uint32_t frame_ms,
                                               const uint32_t bitrate_bps)
{
    if ((sample_rate_hz != 8000u && sample_rate_hz != 16000u) ||
        bitrate_bps == 0u ||
        enc_duration(frame_ms) == ESP_OPUS_ENC_FRAME_DURATION_ARG) {
        return nullptr;
    }
    OpusVoiceEnc *enc = static_cast<OpusVoiceEnc *>(
        heap_caps_calloc(1, sizeof(OpusVoiceEnc), MALLOC_CAP_SPIRAM));
    if (enc == nullptr) {
        return nullptr;
    }

    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = sample_rate_hz;
    cfg.channel = ESP_AUDIO_MONO;
    cfg.bits_per_sample = ESP_AUDIO_BIT16;
    cfg.bitrate = static_cast<int>(bitrate_bps);
    cfg.frame_duration = enc_duration(frame_ms);
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = kComplexity;
    cfg.enable_fec = false;
    cfg.enable_dtx = false;
    cfg.enable_vbr = true;

    if (esp_opus_enc_open(&cfg, sizeof(cfg), &enc->handle) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "encoder open failed (%lums)", static_cast<unsigned long>(frame_ms));
        heap_caps_free(enc);
        return nullptr;
    }
    enc->frame_samples = (sample_rate_hz / 1000u) * frame_ms;
    ESP_LOGI(TAG, "encoder up: %luk mono %lums VOIP c%d VBR %lubps",
             static_cast<unsigned long>(sample_rate_hz / 1000u),
             static_cast<unsigned long>(frame_ms), kComplexity,
             static_cast<unsigned long>(bitrate_bps));
    return enc;
}

extern "C" OpusVoiceEnc *OPUS_VOICE_EncOpen(const uint32_t frame_ms)
{
    return OPUS_VOICE_EncOpenEx(OPUS_VOICE_SAMPLE_RATE, frame_ms,
                                static_cast<uint32_t>(kBitrateBps));
}

extern "C" int OPUS_VOICE_EncProcess(OpusVoiceEnc *enc, const int16_t *pcm, const size_t samples,
                                     uint8_t *out, const size_t out_capacity)
{
    if (enc == nullptr || enc->handle == nullptr || pcm == nullptr || out == nullptr ||
        samples != enc->frame_samples) {
        return -1;
    }
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t *>(const_cast<int16_t *>(pcm));
    in.len = static_cast<uint32_t>(samples * sizeof(int16_t));
    esp_audio_enc_out_frame_t out_frame = {};
    out_frame.buffer = out;
    out_frame.len = static_cast<uint32_t>(out_capacity);
    if (esp_opus_enc_process(enc->handle, &in, &out_frame) != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return static_cast<int>(out_frame.encoded_bytes);
}

extern "C" void OPUS_VOICE_EncClose(OpusVoiceEnc *enc)
{
    if (enc == nullptr) {
        return;
    }
    if (enc->handle != nullptr) {
        esp_opus_enc_close(enc->handle);
    }
    heap_caps_free(enc);
}

extern "C" OpusVoiceDec *OPUS_VOICE_DecOpenEx(const uint32_t sample_rate_hz,
                                               const uint32_t frame_ms)
{
    if ((sample_rate_hz != 8000u && sample_rate_hz != 16000u) ||
        dec_duration(frame_ms) == ESP_OPUS_DEC_FRAME_DURATION_INVALID) {
        return nullptr;
    }
    OpusVoiceDec *dec = static_cast<OpusVoiceDec *>(
        heap_caps_calloc(1, sizeof(OpusVoiceDec), MALLOC_CAP_SPIRAM));
    if (dec == nullptr) {
        return nullptr;
    }
    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate = sample_rate_hz;
    cfg.channel = ESP_AUDIO_MONO;
    cfg.frame_duration = dec_duration(frame_ms);
    cfg.self_delimited = false;
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec->handle) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed");
        heap_caps_free(dec);
        return nullptr;
    }
    return dec;
}

extern "C" OpusVoiceDec *OPUS_VOICE_DecOpen(const uint32_t frame_ms)
{
    return OPUS_VOICE_DecOpenEx(OPUS_VOICE_SAMPLE_RATE, frame_ms);
}

extern "C" int OPUS_VOICE_DecProcess(OpusVoiceDec *dec, const uint8_t *frame, const size_t frame_bytes,
                                     int16_t *pcm_out, const size_t out_capacity_samples)
{
    if (dec == nullptr || dec->handle == nullptr || frame == nullptr || pcm_out == nullptr) {
        return -1;
    }
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(frame);
    raw.len = static_cast<uint32_t>(frame_bytes);
    esp_audio_dec_out_frame_t out = {};
    out.buffer = reinterpret_cast<uint8_t *>(pcm_out);
    out.len = static_cast<uint32_t>(out_capacity_samples * sizeof(int16_t));
    esp_audio_dec_info_t info = {};
    if (esp_opus_dec_decode(dec->handle, &raw, &out, &info) != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return static_cast<int>(out.decoded_size / sizeof(int16_t));
}

extern "C" void OPUS_VOICE_DecClose(OpusVoiceDec *dec)
{
    if (dec == nullptr) {
        return;
    }
    if (dec->handle != nullptr) {
        esp_opus_dec_close(dec->handle);
    }
    heap_caps_free(dec);
}
