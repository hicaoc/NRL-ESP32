#include "audio/voice_resampler.h"

#include <stdlib.h>
#include <string.h>

#include "esp_asrc.h"

namespace {
constexpr uint32_t kVoiceRateHz = 8000u;
// Hardware-path frame timeout. Generous: even a 8192-frame 8 kHz chunk is
// ~1 s of audio, and the ASRC peripheral converts far faster than real time.
constexpr int32_t kAsrcTimeoutMs = 500;

// Grow `*buf`/`*cap` to at least `need` bytes with the alignment the ASRC
// hardware path requires. Returns false on allocation failure.
bool grow_aligned(uint8_t **buf, size_t *cap, const size_t need,
                  const uint32_t addr_align, const uint32_t size_align)
{
    if (need <= *cap) {
        return true;
    }
    uint32_t allocated = 0;
    void *p = esp_asrc_align_alloc(static_cast<uint32_t>(need), addr_align,
                                   size_align, &allocated);
    if (p == nullptr) {
        return false;
    }
    free(*buf);
    *buf = static_cast<uint8_t *>(p);
    *cap = allocated;
    return true;
}
} // namespace

extern "C" void VOICE_RESAMPLER_Deinit(VoiceResampler *rs)
{
    if (rs == nullptr) {
        return;
    }
    if (rs->asrc != nullptr) {
        esp_asrc_close(rs->asrc);
        rs->asrc = nullptr;
    }
    free(rs->in_buf);
    rs->in_buf = nullptr;
    rs->in_cap_bytes = 0;
    free(rs->out_buf);
    rs->out_buf = nullptr;
    rs->out_cap_bytes = 0;
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

    // AUTO: hardware ASRC on ESP32-S31, optimized software elsewhere. The
    // NULL weight makes the stereo->mono downmix (L+R)/2, same as before.
    esp_asrc_cfg_t cfg = {};
    cfg.src_info.sample_rate = in_rate_hz;
    cfg.src_info.channel = channels;
    cfg.src_info.bits_per_sample = 16u;
    cfg.dest_info.sample_rate = kVoiceRateHz;
    cfg.dest_info.channel = 1u;
    cfg.dest_info.bits_per_sample = 16u;
    cfg.perf_type = ESP_ASRC_PERF_TYPE_AUTO;
    cfg.complexity = 1u; // voice-grade 8 kHz telephony output (software path)
    cfg.timeout_ms = kAsrcTimeoutMs;
    if (esp_asrc_open(&cfg, &rs->asrc) != ESP_ASRC_ERR_OK) {
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
        rs->asrc == nullptr || out_capacity == 0u) {
        return 0;
    }

    esp_asrc_buffer_alignment_t align = {};
    if (esp_asrc_get_buffer_alignment(&align) != ESP_ASRC_ERR_OK) {
        return 0;
    }
    const size_t in_bytes = in_frames * rs->channels * sizeof(int16_t);
    uint32_t out_frames_max = 0;
    if (esp_asrc_get_out_sample_num(rs->asrc, static_cast<uint32_t>(in_frames),
                                    &out_frames_max) != ESP_ASRC_ERR_OK) {
        return 0;
    }
    const size_t out_bytes = static_cast<size_t>(out_frames_max) * sizeof(int16_t);
    if (!grow_aligned(&rs->in_buf, &rs->in_cap_bytes, in_bytes,
                      align.inbuf_addr_align, align.inbuf_size_align) ||
        !grow_aligned(&rs->out_buf, &rs->out_cap_bytes, out_bytes,
                      align.outbuf_addr_align, align.outbuf_size_align)) {
        return 0;
    }

    memcpy(rs->in_buf, in, in_bytes);
    uint32_t produced = out_frames_max;
    if (esp_asrc_process(rs->asrc, rs->in_buf, static_cast<uint32_t>(in_frames),
                         rs->out_buf, &produced) != ESP_ASRC_ERR_OK) {
        return 0;
    }
    size_t n = produced;
    if (n > out_capacity) {
        n = out_capacity; // caller sized out per the header contract; clamp defensively
    }
    memcpy(out, rs->out_buf, n * sizeof(int16_t));
    return n;
}
