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
    uint8_t *rgb565;   // 16-byte-aligned allocation owned by this module
    size_t bytes;
    bool rgb565_uses_jpeg_allocator; // false for the normal PSRAM path
} CoverBitmap;

// Decode `jpeg` scaled to fit within max_dim x max_dim (aspect kept,
// dimensions rounded to the decoder's multiple-of-8 requirement). Returns
// false on parse/decode failure; *out is zeroed then.
bool COVER_DecodeJpeg(const uint8_t *jpeg, size_t jpeg_size, uint16_t max_dim, CoverBitmap *out);

// Free the bitmap (safe on a zeroed struct).
void COVER_Free(CoverBitmap *bitmap);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_COVER_DECODER_H
