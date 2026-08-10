#ifndef SRC_SERVICES_FMO_FRAME_H
#define SRC_SERVICES_FMO_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMO_FRAME_OPUS = 1,
    FMO_FRAME_ADPCM = 2,
} FmoFrameCodec;

typedef struct {
    char callsign[7];
    uint16_t session;
    uint32_t started_at;
    uint32_t timestamp;
    uint16_t block_count;
    uint8_t buffer_depth;
} FmoFrameInfo;

typedef bool (*FmoFrameBlockHandler)(void *context, FmoFrameCodec codec,
                                     const uint8_t *data, size_t data_size,
                                     int16_t adpcm_sample, uint8_t adpcm_index);

bool FMO_FRAME_Parse(const uint8_t *frame, size_t frame_size,
                     FmoFrameInfo *info, FmoFrameBlockHandler handler,
                     void *context);

size_t FMO_FRAME_BuildOpus(uint8_t *output, size_t capacity,
                           const char *callsign, uint16_t session,
                           uint32_t started_at, uint32_t timestamp,
                           const uint8_t *const packets[],
                           const size_t packet_sizes[], size_t packet_count,
                           uint8_t buffer_depth);

#ifdef __cplusplus
}
#endif

#endif
