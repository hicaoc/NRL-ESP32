#ifndef SRC_MEDIA_MEDIA_METADATA_H
#define SRC_MEDIA_MEDIA_METADATA_H

// Tag/metadata parser for the media player (docs/architecture.md 3.3):
// reads title/artist/album and the embedded cover image from ID3v2 (MP3),
// Vorbis Comment + PICTURE blocks (FLAC), with an ID3v1 fallback. All text
// is normalised to UTF-8 (ID3v2 UTF-16 tags -- the common case for Chinese
// songs -- are converted). Output feeds the player UI and the nanny
// "now playing" display; the cover bytes go to esp_lv_decoder (JPEG/PNG)
// for rendering.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_COVER_NONE = 0,
    MEDIA_COVER_JPEG,
    MEDIA_COVER_PNG,
} MediaCoverType_t;

typedef struct {
    char title[96];
    char artist[96];
    char album[96];
    uint32_t duration_ms;      // 0 = unknown
    uint8_t *cover_data;       // PSRAM allocation, freed by MEDIA_META_Free
    size_t cover_size;
    MediaCoverType_t cover_type;
} MediaTrackInfo;

// Parse tags of `path`. Fields that are absent stay empty/0; the function
// returns true if at least one field was filled. `want_cover` skips the
// image extraction (playlist scans don't need it).
bool MEDIA_META_Read(const char *path, MediaTrackInfo *out_info, bool want_cover);

// Free the cover allocation (safe on a zeroed struct).
void MEDIA_META_Free(MediaTrackInfo *info);

// Free ONLY the embedded cover (keeps title/artist/album): the UI copies the
// JPEG bytes out for async decode, after which holding the original (up to
// 512 KB of PSRAM) for the rest of the track is pure waste.
void MEDIA_META_ReleaseCover(MediaTrackInfo *info);

// Converts unknown 8-bit text to UTF-8, accepting ASCII/UTF-8 first and GBK
// second. Used by legacy radio/network metadata as well as media tags.
void MEDIA_TEXT_8BitToUtf8(const uint8_t *data, size_t bytes,
                           char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_MEDIA_METADATA_H
