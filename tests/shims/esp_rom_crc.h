// Host shim for ESP-IDF's esp_rom_crc.h: reflected CRC-32 (poly 0xEDB88320),
// same semantics as the ROM routine (~init in, inverted result out), so frames
// built and parsed on the host agree on the CRC field.
#ifndef TESTS_SHIMS_ESP_ROM_CRC_H
#define TESTS_SHIMS_ESP_ROM_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t esp_rom_crc32_le(uint32_t init, const uint8_t *data,
                                        size_t len)
{
    uint32_t crc = ~init;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

#ifdef __cplusplus
}
#endif

#endif // TESTS_SHIMS_ESP_ROM_CRC_H
