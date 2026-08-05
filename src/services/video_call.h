#ifndef SRC_SERVICES_VIDEO_CALL_H
#define SRC_SERVICES_VIDEO_CALL_H

// Video call over the NRL protocol (docs/architecture.md 功能3): the DVP
// camera's native JPEG frames are fragmented into NRL packet type 13 and
// relayed by the server; received fragments reassemble into JPEG frames the
// LCD's video page decodes (reusing the album-art JPEG decoder). Audio rides
// the existing voice path unchanged.
//
// Payload layout (after the 48-byte NRL header):
//   [0..1] frame_seq (big-endian), [2] frag_idx, [3] frag_cnt, [4..] JPEG.
//
// S31-only (camera + LCD); stubs elsewhere.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Upper bound of one JPEG frame in either direction; sizes the destination
// buffer for VIDEO_CopyLocalFrame. The OV3660's only DVP JPEG mode is
// 1280x720, which lands around 40-90 KB per frame.
#define VIDEO_MAX_JPEG_BYTES (128 * 1024)

// Register the RX handler with the bridge. Call once at startup; reception
// is passive from then on (frames buffer whenever a peer transmits).
void VIDEO_Init(void);

// Camera transmission on/off (~5 fps 720p JPEG, sensor-side encode).
// Returns false when the camera fails to start (no sensor / wrong board).
bool VIDEO_SetTxEnabled(bool enabled);
bool VIDEO_TxEnabled(void);

// Latest completely received JPEG frame, copied into `dst` under the
// internal lock so the caller can decode at its own pace with no tear risk
// (the RX reassembly overwrites the ready buffer at any moment). Returns
// true when a frame newer than `*seq` was copied. Pattern: acquire -> decode
// -> render.
bool VIDEO_AcquireFrame(uint8_t *dst, size_t dst_cap, size_t *jpeg_size, uint32_t *seq);

// Latest locally captured JPEG frame (only while camera TX is enabled),
// copied into `dst` under the internal lock so the caller can decode at its
// own pace with no tear risk. Returns true when a frame newer than `*seq`
// was copied. Used by the LCD video page's self-view.
bool VIDEO_CopyLocalFrame(uint8_t *dst, size_t dst_cap, size_t *size, uint32_t *seq);

// True while frames arrived within the last couple of seconds.
bool VIDEO_Receiving(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_VIDEO_CALL_H
