#ifndef SRC_LIB_SSTV_CODEC_H
#define SRC_LIB_SSTV_CODEC_H

// SSTV transmit modulator (pure DSP, no I/O):
//
//  - Modes: Robot 36 (320x240, YUV 4:2:0, VIS 8) and Martin M1 (320x256,
//    G-B-R, VIS 44). Timing follows the de-facto MMSSTV line structure
//    (sync/porch/separator widths verified against published zero-crossing
//    analyses, see sstv_codec.cpp).
//  - 16 kHz mono PCM16, generated in 10 ms (160-sample) chunks. Every tone
//    and scan duration is an exact sample count, so line timing derives from
//    the sample position, never from wall-clock pacing; the caller's job is
//    only to push each chunk downstream roughly every 10 ms.
//  - Sub-carrier: phase-continuous NCO (1200 Hz sync, 1500 Hz black,
//    2300 Hz white, pixels linear in between), amplitude ~7000.
//
// Usage: SSTV_TxInit(mode), SSTV_TxSetImage(rgb565, ...), then call
// SSTV_TxFillChunk() until it returns false.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSTV_SAMPLE_RATE_HZ 16000u
#define SSTV_CHUNK_SAMPLES 160u // 10 ms per push tick
#define SSTV_IMAGE_WIDTH 320u

typedef enum {
    SSTV_MODE_ROBOT36 = 0, // 320x240, ~36.6 s airtime
    SSTV_MODE_MARTIN_M1,   // 320x256, ~114.9 s airtime
} SSTV_Mode;

// Reset the generator for a new frame. No audio comes out before
// SSTV_TxSetImage() accepts an image.
void SSTV_TxInit(SSTV_Mode mode);

// Hand over the frame buffer (RGB565, owned by the caller, must outlive the
// transmission). Expected geometry: 320x240 (Robot 36) or 320x256 (Martin
// M1); anything else is rejected and the generator stays silent.
bool SSTV_TxSetImage(const uint16_t *rgb565, uint16_t width, uint16_t height);

// Fill `out` with the next SSTV_CHUNK_SAMPLES of modulated audio. Returns
// false once the frame is fully sent (the tail of the final chunk and any
// later call are zero-filled).
bool SSTV_TxFillChunk(int16_t *out);

// Frame length / current position in samples, for progress reporting.
uint32_t SSTV_TxTotalSamples(void);
uint32_t SSTV_TxElapsedSamples(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_LIB_SSTV_CODEC_H
