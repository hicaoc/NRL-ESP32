#include "services/fmo_frame.h"

#include <esp_rom_crc.h>
#include <string.h>

namespace {

static uint16_t readLe16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8u);
}

static uint32_t readLe32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8u) |
           (static_cast<uint32_t>(data[2]) << 16u) |
           (static_cast<uint32_t>(data[3]) << 24u);
}

static void writeLe16(uint8_t *data, const uint16_t value)
{
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8u);
}

static void writeLe32(uint8_t *data, const uint32_t value)
{
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8u);
    data[2] = static_cast<uint8_t>(value >> 16u);
    data[3] = static_cast<uint8_t>(value >> 24u);
}

} // namespace

extern "C" bool FMO_FRAME_Parse(const uint8_t *frame, const size_t frame_size,
                                 FmoFrameInfo *info,
                                 const FmoFrameBlockHandler handler,
                                 void *context)
{
    static const uint8_t marker[5] = {0x3d, 0x14, 0x00, 0xe0, 0x3d};
    if (frame == nullptr || info == nullptr || frame_size < 64u ||
        frame[0] != 1u || readLe32(frame + 30u) != frame_size ||
        esp_rom_crc32_le(0, frame + 64u, frame_size - 64u) !=
            readLe32(frame + 36u)) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->session = readLe16(frame + 6u);
    memcpy(info->callsign, frame + 10u, 6u);
    info->callsign[6] = '\0';
    info->started_at = readLe32(frame + 22u);
    info->timestamp = readLe32(frame + 26u);
    info->block_count = readLe16(frame + 34u);
    info->buffer_depth = frame[40u];

    size_t offset = 64u;
    uint16_t blocks = 0u;
    while (offset < frame_size) {
        if (offset + 12u > frame_size) return false;
        const size_t block_size = readLe16(frame + offset + 2u);
        if (block_size < 16u || offset + block_size > frame_size) return false;
        const uint8_t *inner = frame + offset + 8u;
        const size_t inner_size = block_size - 8u;
        const size_t declared = readLe16(inner + 1u);
        if (declared != inner_size || inner_size < 8u ||
            memcmp(inner + 3u, marker, sizeof(marker)) != 0) {
            return false;
        }

        const FmoFrameCodec codec = static_cast<FmoFrameCodec>(inner[0]);
        const uint8_t *payload = inner + 8u;
        size_t payload_size = inner_size - 8u;
        int16_t initial_sample = 0;
        uint8_t initial_index = 0u;
        if (codec == FMO_FRAME_ADPCM) {
            if (payload_size != 328u) return false;
            initial_sample = static_cast<int16_t>(readLe16(payload + 2u));
            initial_index = payload[4u];
            payload += 8u;
            payload_size -= 8u;
        } else if (codec != FMO_FRAME_OPUS || payload_size == 0u) {
            offset += block_size;
            ++blocks;
            continue;
        }
        if (handler != nullptr &&
            !handler(context, codec, payload, payload_size, initial_sample,
                     initial_index)) {
            return false;
        }
        offset += block_size;
        ++blocks;
    }
    return offset == frame_size && blocks == info->block_count;
}

extern "C" size_t FMO_FRAME_BuildOpus(
    uint8_t *output, const size_t capacity, const char *callsign,
    const uint16_t session, const uint32_t started_at, const uint32_t timestamp,
    const uint8_t *const packets[], const size_t packet_sizes[],
    const size_t packet_count, const uint8_t buffer_depth)
{
    static const uint8_t marker[5] = {0x3d, 0x14, 0x00, 0xe0, 0x3d};
    if (output == nullptr || callsign == nullptr || packets == nullptr ||
        packet_sizes == nullptr || packet_count == 0u ||
        packet_count > UINT16_MAX || capacity < 64u) {
        return 0u;
    }
    size_t total = 64u;
    for (size_t i = 0; i < packet_count; ++i) {
        if (packets[i] == nullptr || packet_sizes[i] == 0u ||
            packet_sizes[i] > UINT16_MAX - 16u ||
            total > capacity - (16u + packet_sizes[i])) {
            return 0u;
        }
        total += 16u + packet_sizes[i];
    }

    memset(output, 0, total);
    output[0] = 1u;
    writeLe16(output + 6u, session);
    const size_t callsign_size = strnlen(callsign, 6u);
    for (size_t i = 0; i < callsign_size; ++i) {
        const char ch = callsign[i];
        output[10u + i] = static_cast<uint8_t>(
            ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch);
    }
    writeLe32(output + 22u, started_at);
    writeLe32(output + 26u, timestamp);
    writeLe32(output + 30u, static_cast<uint32_t>(total));
    writeLe16(output + 34u, static_cast<uint16_t>(packet_count));
    output[40u] = buffer_depth;
    output[41u] = 0xbf;
    output[42u] = 0x01;

    size_t offset = 64u;
    for (size_t i = 0; i < packet_count; ++i) {
        const size_t block_size = 16u + packet_sizes[i];
        output[offset] = static_cast<uint8_t>(i + 1u);
        writeLe16(output + offset + 2u, static_cast<uint16_t>(block_size));
        uint8_t *inner = output + offset + 8u;
        inner[0] = FMO_FRAME_OPUS;
        writeLe16(inner + 1u, static_cast<uint16_t>(8u + packet_sizes[i]));
        memcpy(inner + 3u, marker, sizeof(marker));
        memcpy(inner + 8u, packets[i], packet_sizes[i]);
        offset += block_size;
    }
    writeLe32(output + 36u,
              esp_rom_crc32_le(0, output + 64u, total - 64u));
    return total;
}
