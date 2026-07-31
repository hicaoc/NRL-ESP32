#ifndef SRC_AUDIO_VOICE_RESAMPLER_H
#define SRC_AUDIO_VOICE_RESAMPLER_H

// Stateful media -> voice-domain resampler (docs/architecture.md 3.2 SRC):
// arbitrary-rate stereo/mono PCM16 (8k-192k) down to 8 kHz mono for the NRL
// G.711 uplink. Built on esp_audio_effects: stereo is downmixed with
// esp_ae_ch_cvt first, then band-limited resampled with esp_ae_rate_cvt
// (replaces the old hand-written linear interpolator -- proper anti-alias
// filtering now, and less custom DSP to maintain). Voice-grade target by
// design: the network path is 8 kHz telephony anyway.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t in_rate_hz;
    uint8_t channels;      // 1 or 2 (interleaved)
    void *ch_cvt;          // esp_ae_ch_cvt_handle_t, stereo downmix (NULL if mono)
    void *rate_cvt;        // esp_ae_rate_cvt_handle_t
    int16_t *mono_buf;     // downmix scratch, grown on demand (stereo only)
    size_t mono_cap_frames;
} VoiceResampler;

// Initialise / re-initialise for an input format. Idempotent: safe to call
// on an already-initialised struct (existing handles are closed first).
// Returns 0 on bad args or handle allocation failure.
int VOICE_RESAMPLER_Init(VoiceResampler *rs, uint32_t in_rate_hz, uint8_t channels);

// Convert one interleaved PCM16 chunk. in_frames counts frames (sample
// groups), not samples. Returns the number of 8 kHz mono samples written to
// out (bounded by out_capacity; size out for in_frames * 8000 / in_rate + 16).
size_t VOICE_RESAMPLER_Process(VoiceResampler *rs,
                               const int16_t *in, size_t in_frames,
                               int16_t *out, size_t out_capacity);

// Release the esp_audio_effects handles and scratch buffer. Safe on a
// zero-initialised struct.
void VOICE_RESAMPLER_Deinit(VoiceResampler *rs);

#ifdef __cplusplus
}
#endif

#endif // SRC_AUDIO_VOICE_RESAMPLER_H
