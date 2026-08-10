#ifndef SRC_MEDIA_OPUS_VOICE_H
#define SRC_MEDIA_OPUS_VOICE_H

// Shared Opus voice codec (esp_audio_codec Opus, direct API): 16 kHz mono
// PCM16, OPUS_APPLICATION_VOIP, complexity 10, VBR at a 32-40 kbps working
// point. Consumers:
//   - NRL bridge packet type 8 (wideband voice, 20 ms / 320-sample frames)
//   - xiaozhi AI assistant link (same format at 60 ms frames)
// The frame duration is fixed per encoder/decoder instance at open time.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPUS_VOICE_SAMPLE_RATE 16000u
// Worst-case encoded frame bound used for TX buffers (VBR peaks well below).
#define OPUS_VOICE_MAX_FRAME_BYTES 640u

typedef struct OpusVoiceEnc OpusVoiceEnc;
typedef struct OpusVoiceDec OpusVoiceDec;

// frame_ms must be one of 20/40/60. Samples per frame = 16 * frame_ms.
OpusVoiceEnc *OPUS_VOICE_EncOpen(uint32_t frame_ms);

// Variant used by FMO-V4 narrowband voice. sample_rate_hz is 8000 or 16000;
// frame_ms is 20/40/60 and bitrate_bps must be positive.
OpusVoiceEnc *OPUS_VOICE_EncOpenEx(uint32_t sample_rate_hz,
                                   uint32_t frame_ms,
                                   uint32_t bitrate_bps);

// Encode exactly one frame (16 * frame_ms mono samples). Returns encoded
// byte count, or < 0 on error.
int OPUS_VOICE_EncProcess(OpusVoiceEnc *enc, const int16_t *pcm, size_t samples,
                          uint8_t *out, size_t out_capacity);

void OPUS_VOICE_EncClose(OpusVoiceEnc *enc);

OpusVoiceDec *OPUS_VOICE_DecOpen(uint32_t frame_ms);

OpusVoiceDec *OPUS_VOICE_DecOpenEx(uint32_t sample_rate_hz,
                                   uint32_t frame_ms);

// Decode one Opus packet. Returns decoded sample count (mono 16 kHz), or
// < 0 on error.
int OPUS_VOICE_DecProcess(OpusVoiceDec *dec, const uint8_t *frame, size_t frame_bytes,
                          int16_t *pcm_out, size_t out_capacity_samples);

void OPUS_VOICE_DecClose(OpusVoiceDec *dec);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_OPUS_VOICE_H
