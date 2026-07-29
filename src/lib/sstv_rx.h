#ifndef SRC_LIB_SSTV_RX_H
#define SRC_LIB_SSTV_RX_H

// SSTV receive demodulator (pure DSP, no I/O), the reverse of sstv_codec:
//
//  - Feed 16 kHz mono PCM16 from any audio tap (mic / NRL downlink). Feed is
//    called from an audio task: per-sample work is O(1) table/DSP, no heap,
//    no blocking; callbacks fire from the feed context and must stay cheap.
//  - FM demod: quadrature discriminator (1900 Hz NCO, I/Q low-pass,
//    phase-difference -> instantaneous frequency). Sync/leader detect:
//    sliding Goertzel at 1200 Hz with Schmitt thresholds.
//  - VIS: 300 ms leader -> 30 ms slots (start, 7 data LSB-first, even parity,
//    stop), accepts VIS 8 (Robot 36) and 44 (Martin M1), ignores the rest.
//  - Lines: each scan line re-anchors on its own sync leading edge (no global
//    slant correction in phase 1), pixels sampled at segment-relative centers
//    using the TX timing table. Robot chroma channels are separated by the
//    even/odd separator position; Martin scans G-B-R per line.
//  - Starvation/loss: without a sync for two line periods the decoder drops
//    back to HUNT (the partial frame stays with the caller).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sstv_codec.h" // SSTV_Mode

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSTV_RX_HUNT = 0, // listening for a VIS leader
    SSTV_RX_VIS,      // reading VIS bits
    SSTV_RX_LINES,    // receiving image lines
    SSTV_RX_DONE,     // frame complete (still listening for the next leader)
} SstvRxState;

// All callbacks fire from the SSTV_RX_Feed context (audio task): no heap, no
// blocking, no LVGL. `rgb565_row` is 320 pixels owned by the decoder.
typedef void (*SstvRxVisCallback)(SSTV_Mode mode, void *user);
typedef void (*SstvRxLineCallback)(uint16_t y, const uint16_t *rgb565_row, void *user);
typedef void (*SstvRxDoneCallback)(void *user);

void SSTV_RX_Init(SstvRxVisCallback on_vis, SstvRxLineCallback on_line,
                  SstvRxDoneCallback on_done, void *user);

// Abort any partial frame and go back to listening.
void SSTV_RX_Reset(void);

void SSTV_RX_Feed(const int16_t *samples, size_t count);

SstvRxState SSTV_RX_GetState(void);
// Valid once onVis fired (LINES/DONE).
SSTV_Mode SSTV_RX_GetMode(void);
uint16_t SSTV_RX_LinesReceived(void);
uint16_t SSTV_RX_LinesTotal(void);
// 0..100: recent 1200 Hz tone strength vs total energy (sync quality).
uint8_t SSTV_RX_SignalQuality(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_LIB_SSTV_RX_H
