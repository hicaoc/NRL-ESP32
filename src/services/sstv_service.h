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

typedef struct {
    SstvState state;
    SSTV_Mode mode;
    uint8_t progress_percent;
    char path[96];
    char error[32];
    uint32_t revision; // bump counter so UIs can skip redundant redraws
} SstvSnapshot;

void SSTV_SERVICE_Init(void);

// Queue a transmission. Returns false when another frame is busy or the
// arguments are unusable; decode errors surface later via the snapshot.
bool SSTV_SERVICE_SendJpeg(const char *path, SSTV_Mode mode);

// Abort the running/queued transmission. Returns false when nothing was
// active.
bool SSTV_SERVICE_Stop(void);

void SSTV_SERVICE_GetSnapshot(SstvSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_SSTV_SERVICE_H
