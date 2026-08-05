#ifndef SRC_MEDIA_COVER_DECODER_H
#define SRC_MEDIA_COVER_DECODER_H

// Album-art decoder: JPEG cover bytes (from media_metadata) -> RGB565
// bitmap for the LVGL now-playing card, downscaled at decode time via
// esp_new_jpeg so a 1000x1000 cover never allocates a full-size frame.
// PNG covers are not decoded yet (placeholder icon shows instead).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t stride;   // row byte pitch; 0 means width*2 (unpadded)
    uint8_t *rgb565;   // 16-byte-aligned allocation owned by this module
    size_t bytes;
    bool rgb565_uses_jpeg_allocator; // false for the normal PSRAM path
    bool rgb565_borrowed; // true: pixels live in a module-owned buffer; the
                          // bitmap is only valid until the next decode call
} CoverBitmap;

// Decode `jpeg` scaled to fit within max_dim x max_dim (aspect kept,
// dimensions rounded to the decoder's multiple-of-8 requirement). Returns
// false on parse/decode failure; *out is zeroed then.
bool COVER_DecodeJpeg(const uint8_t *jpeg, size_t jpeg_size, uint16_t max_dim, CoverBitmap *out);

// Hardware JPEG decode (ESP32-S31 JPEG codec peripheral): full-resolution
// RGB565 output, no downscaling. Decodes into ONE module-owned persistent
// buffer (borrowed semantics: rgb565_borrowed is set, the pointer is only
// valid until the next COVER_DecodeJpegHw call -- copy or scale it out
// immediately). This keeps PSRAM use at a single ~1.8 MB buffer instead of
// one allocation per frame. Returns false when the chip has no JPEG
// hardware, when the stream is invalid, or on resource failure; callers
// should fall back to COVER_DecodeJpeg().
bool COVER_DecodeJpegHw(const uint8_t *jpeg, size_t jpeg_size, CoverBitmap *out);

// Free the persistent hardware-decode buffer (call when the video UI is
// torn down so the ~1.8 MB of PSRAM is returned to the heap).
void COVER_HwDecoderRelease(void);

// Nearest-neighbour downscale of an RGB565 buffer to fit within a
// fit_w x fit_h box (aspect kept, never upscaled). Output is a freshly
// allocated packed bitmap (stride == width*2, owned through COVER_Free).
bool COVER_FitRgb565(const uint8_t *src, uint16_t src_w, uint16_t src_h,
                     uint32_t src_stride, uint16_t fit_w, uint16_t fit_h, CoverBitmap *out);

// Same as COVER_FitRgb565 with a square fit box.
bool COVER_ScaleRgb565(const uint8_t *src, uint16_t src_w, uint16_t src_h,
                       uint32_t src_stride, uint16_t max_dim, CoverBitmap *out);

// Free the bitmap (safe on a zeroed struct).
void COVER_Free(CoverBitmap *bitmap);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_COVER_DECODER_H
