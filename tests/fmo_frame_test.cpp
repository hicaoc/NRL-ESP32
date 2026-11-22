// Host unit test for FMO/RAW frame build/parse (src/services/fmo_frame.cpp):
// - V4 header uid roundtrip (offset 6..9 little-endian u32);
// - inner 5-byte sync marker: legacy CONST5 and the all-zero marker emitted
//   by V4 devices (live captures: identical frames except zeroed marker)
//   must both parse; any other marker value is still rejected.
//
// Build & run (from the repo root, MSYS2/MinGW g++ works):
//   g++ -std=c++17 -Wall -Wextra -I src -I tests/shims
//       tests/fmo_frame_test.cpp src/services/fmo_frame.cpp
//       -o .tmp/fmo_frame_test.exe
//   ./.tmp/fmo_frame_test.exe

#include "services/fmo_frame.h"

#include <esp_rom_crc.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {

struct Capture {
    int calls = 0;
    size_t payload_bytes = 0;
};

bool onBlock(void *context, const FmoFrameCodec codec, const uint8_t *data,
             const size_t data_size, int16_t, uint8_t)
{
    assert(codec == FMO_FRAME_OPUS);
    assert(data != nullptr && data_size > 0u);
    // Opus TOC of the fixture packets is 0x10 (SILK NB 40 ms).
    assert(data[0] == 0x10u);
    Capture *capture = static_cast<Capture *>(context);
    ++capture->calls;
    capture->payload_bytes += data_size;
    return true;
}

void fixCrc(uint8_t *frame, const size_t frame_size)
{
    const uint32_t crc = esp_rom_crc32_le(0u, frame + 64u, frame_size - 64u);
    for (int i = 0; i < 4; ++i) {
        frame[36u + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
}

// Overwrite every block's inner 5-byte marker (at inner+3 == offset+8+3).
void setMarkers(uint8_t *frame, const size_t frame_size, const uint8_t value[5])
{
    size_t offset = 64u;
    while (offset < frame_size) {
        const size_t block_size =
            static_cast<size_t>(frame[offset + 2u]) |
            (static_cast<size_t>(frame[offset + 3u]) << 8u);
        memcpy(frame + offset + 11u, value, 5u);
        offset += block_size;
    }
    fixCrc(frame, frame_size);
}

void testLegacyMarkerParses()
{
    const uint8_t packet0[24] = {0x10u};
    const uint8_t packet1[32] = {0x10u};
    const uint8_t *packets[] = {packet0, packet1};
    const size_t sizes[] = {sizeof(packet0), sizeof(packet1)};
    uint8_t frame[256];
    const size_t frame_size = FMO_FRAME_BuildOpus(
        frame, sizeof(frame), "bi4umd", 5200u, 1700000000u, 1700000001u,
        packets, sizes, 2u, 3u);
    assert(frame_size == 64u + 16u + sizeof(packet0) + 16u + sizeof(packet1));

    FmoFrameInfo info = {};
    Capture capture;
    assert(FMO_FRAME_Parse(frame, frame_size, &info, onBlock, &capture));
    assert(info.uid == 5200u); // V4 header uid roundtrip, not a session id
    assert(strcmp(info.callsign, "BI4UMD") == 0);
    assert(info.block_count == 2u);
    assert(capture.calls == 2);
    assert(capture.payload_bytes == sizeof(packet0) + sizeof(packet1));

    // V4 device variant: same frame with the sync markers zeroed must parse
    // identically (this used to fail the CONST5 hard check).
    const uint8_t zeros[5] = {};
    setMarkers(frame, frame_size, zeros);
    Capture zero_capture;
    assert(FMO_FRAME_Parse(frame, frame_size, &info, onBlock, &zero_capture));
    assert(zero_capture.calls == 2);
    assert(zero_capture.payload_bytes == capture.payload_bytes);

    // Garbage markers are still rejected.
    const uint8_t garbage[5] = {0xdeu, 0xadu, 0xbeu, 0xefu, 0x01u};
    setMarkers(frame, frame_size, garbage);
    assert(!FMO_FRAME_Parse(frame, frame_size, &info, onBlock, nullptr));

    printf("ok: legacy CONST5 + all-zero V4 markers parse, garbage rejected\n");
}

} // namespace

int main()
{
    testLegacyMarkerParses();
    printf("all fmo_frame tests passed\n");
    return 0;
}
