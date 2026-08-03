#ifndef SRC_SERVICES_MAP_TILES_H
#define SRC_SERVICES_MAP_TILES_H

// Slippy-Map tile service for the S31 map page (z/x/y Web-Mercator tiles):
//
//  - Tile math: lon/lat (degrees) <-> global pixel coordinates at a zoom
//    level, standard Web-Mercator formulas.
//  - Async loading: a worker task pulls tiles from the TF card first
//    ({SD}/tiles/{z}/{x}/{y}.jpg, pre-packed JPEG), falling back to a direct
//    HTTP(S) download. User-provided /tiles packs remain read-only; downloads
//    write through to a separate versioned cache on the card (re-encoded to
//    JPEG), so cache invalidation cannot hide offline packs.
//  - Decoded 256x256 RGB565 tiles live in a PSRAM LRU cache; consumers poll
//    a bump-counter revision to know when a repaint is worthwhile.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_TILES_TILE_PX 256u

// Web-Mercator projection: the world at zoom z is 256 * 2^z pixels square.
void MAP_TILES_LonLatToPixel(double lon, double lat, uint8_t zoom, double *px, double *py);
void MAP_TILES_PixelToLonLat(double px, double py, uint8_t zoom, double *lon, double *lat);

// Queue a tile for background loading. Deduplicated; a no-op when the tile
// is already cached, in flight, or failed recently.
void MAP_TILES_Request(uint8_t z, int32_t x, int32_t y);

// Cached 256x256 RGB565 pixels (MAP_TILES_TILE_PX * MAP_TILES_TILE_PX * 2
// bytes). The pointer stays owned by the cache -- never free it; it remains
// valid until the LRU slot is reused by another tile. Returns NULL while the
// tile is missing and queues a load automatically.
const uint8_t *MAP_TILES_Get(uint8_t z, int32_t x, int32_t y);

// Bump counter, incremented every time a tile becomes ready.
uint32_t MAP_TILES_Revision(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_MAP_TILES_H
