#ifndef SRC_SERVICES_SSTV_SERVICE_H
#define SRC_SERVICES_SSTV_SERVICE_H

// SSTV transmit service (all boards):
//
//  - SSTV_SERVICE_SendJpeg(path, mode) loads a JPEG from the filesystem
//    (typically /sdcard/...), decodes + cover-crops it to the mode's frame
//    geometry and starts the transmission in a dedicated worker task.
//  - The worker generates 10 ms PCM chunks with sstv_codec and pushes them to
//    the NRL uplink (over the air) and the local speaker (sidetone) through
//    the audio router, paced by a 10 ms tick -- sample-accurate line timing
//    lives entirely inside the codec.
//  - One transmission at a time; a second SendJpeg while busy is rejected.
//    SSTV_SERVICE_Stop() aborts mid-frame.
//
// Triggered via AT (AT+SSTV=ROBOT36,/sdcard/pic.jpg / AT+SSTV? / AT+SSTV=STOP)
// for now; a UI page is planned later.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../lib/sstv_codec.h"
#include "../lib/sstv_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSTV_STATE_IDLE = 0,
    SSTV_STATE_PREPARING, // decoding/scaling the JPEG
    SSTV_STATE_SENDING,
    SSTV_STATE_DONE,      // last frame finished (or was stopped)
    SSTV_STATE_ERROR,
} SstvState;

typedef enum {
    SSTV_SOURCE_MIC = 0, // radio audio from the onboard mic tap
    SSTV_SOURCE_NRL,     // NRL network downlink
} SstvRxSource;

typedef struct {
    SstvState state;
    SSTV_Mode mode;
    uint8_t progress_percent;
    char path[96];
    char error[32];
    uint32_t revision; // bump counter so UIs can skip redundant redraws
    // RX side (independent from TX; TX busy blocks StartRx and vice versa).
    bool rx_active;
    SstvRxSource rx_source;
    SstvRxState rx_state;
    SSTV_Mode rx_mode;        // valid once rx_state >= SSTV_RX_LINES
    uint16_t rx_lines;        // image lines received so far
    uint16_t rx_lines_total;  // 240 (Robot 36) / 256 (Martin M1)
    uint8_t rx_quality;       // 0..100 sync tone strength
    uint32_t rx_revision;     // bumps on every received image line
} SstvSnapshot;

void SSTV_SERVICE_Init(void);

// True when the worker and synchronization primitives were created.
bool SSTV_SERVICE_IsReady(void);

// Resolve the shared TF-card SSTV image directory ("<mount>/sstv"), creating
// it on demand. TX pickers and RX saves use this single location.
bool SSTV_SERVICE_GetImageDirectory(char *out_path, size_t out_path_size);

// Queue a transmission. Returns false when another frame is busy or the
// arguments are unusable; decode errors surface later via the snapshot.
bool SSTV_SERVICE_SendJpeg(const char *path, SSTV_Mode mode);

// Same, from an in-memory JPEG (e.g. a camera frame). The bytes are copied
// out before returning, so the caller's buffer can be recycled immediately.
bool SSTV_SERVICE_SendJpegBuffer(const uint8_t *jpeg, size_t jpeg_size, SSTV_Mode mode);

// Abort the running/queued transmission. Returns false when nothing was
// active.
bool SSTV_SERVICE_Stop(void);

// Start/stop listening for SSTV on one audio source. StartRx returns false
// while a TX is active (or vice versa, SendJpeg returns false during RX);
// starting RX twice switches source and resets the decoder.
bool SSTV_SERVICE_StartRx(SstvRxSource source);
bool SSTV_SERVICE_StopRx(void);

// Clear the live RX framebuffer without stopping or resetting the decoder.
// A newly detected VIS header also clears the buffer automatically.
bool SSTV_SERVICE_ClearRxImage(void);

// Feed the unprocessed 16 kHz microphone tap. The audio capture task calls
// this before speech HPF/AEC/AI noise suppression so SSTV calibration tones
// cannot be removed by voice processing. Ignored unless MIC RX is active.
void SSTV_SERVICE_FeedRawMic(const int16_t *samples, size_t sample_count);

// Received frame so far (320 wide, rx_lines rows valid; do NOT free) plus its
// bump counter. NULL when the frame buffer allocation failed at boot.
const uint16_t *SSTV_SERVICE_RxImage(void);
uint32_t SSTV_SERVICE_RxImageRevision(void);

// Encode the received lines as JPEG to /sdcard/sstv/sstv_rx_<tick>.jpg (TF card
// must be mounted). Writes the path into out_path (may be NULL). Returns
// false when nothing was received or the card/encoder failed.
bool SSTV_SERVICE_SaveRxJpeg(char *out_path, size_t out_path_size);

void SSTV_SERVICE_GetSnapshot(SstvSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_SSTV_SERVICE_H
