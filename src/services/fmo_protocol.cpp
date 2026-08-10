#include "services/fmo_protocol.h"

#include <sodium.h>
#include <string.h>

namespace {

struct CborReader {
    const uint8_t *data;
    size_t size;
    size_t offset;
};

static bool cborHead(CborReader *reader, uint8_t *major, uint64_t *value)
{
    if (reader->offset >= reader->size) return false;
    const uint8_t first = reader->data[reader->offset++];
    *major = first >> 5u;
    const uint8_t additional = first & 0x1fu;
    if (additional < 24u) {
        *value = additional;
        return true;
    }
    const size_t bytes = additional == 24u ? 1u : additional == 25u ? 2u :
                         additional == 26u ? 4u : additional == 27u ? 8u : 0u;
    if (bytes == 0u || reader->offset + bytes > reader->size) return false;
    uint64_t parsed = 0u;
    for (size_t i = 0; i < bytes; ++i) {
        parsed = (parsed << 8u) | reader->data[reader->offset++];
    }
    *value = parsed;
    return true;
}

static bool cborUint(CborReader *reader, uint64_t *value)
{
    uint8_t major = 0u;
    return cborHead(reader, &major, value) && major == 0u;
}

static bool cborBlob(CborReader *reader, const uint8_t expected_major,
                     const uint8_t **data, size_t *size)
{
    uint8_t major = 0u;
    uint64_t length = 0u;
    if (!cborHead(reader, &major, &length) || major != expected_major ||
        length > SIZE_MAX || reader->offset + static_cast<size_t>(length) > reader->size) {
        return false;
    }
    *data = reader->data + reader->offset;
    *size = static_cast<size_t>(length);
    reader->offset += static_cast<size_t>(length);
    return true;
}

static bool textEquals(const uint8_t *text, const size_t size,
                       const char *literal)
{
    const size_t expected = strlen(literal);
    return size == expected && memcmp(text, literal, expected) == 0;
}

} // namespace

extern "C" bool FMO_PROTOCOL_ParseBeaconCertificate(
    const char *base64url, FmoPublicCertificate *certificate)
{
    if (base64url == nullptr || certificate == nullptr || sodium_init() < 0) {
        return false;
    }
    uint8_t raw[512];
    size_t raw_size = 0u;
    if (sodium_base642bin(raw, sizeof(raw), base64url, strlen(base64url),
                          nullptr, &raw_size, nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        return false;
    }
    CborReader reader = {raw, raw_size, 0u};
    uint8_t major = 0u;
    uint64_t array_count = 0u;
    if (!cborHead(&reader, &major, &array_count) || major != 4u ||
        array_count != 10u) {
        return false;
    }
    const size_t first_item = reader.offset;
    const uint8_t *text = nullptr;
    const uint8_t *bytes = nullptr;
    size_t text_size = 0u;
    size_t bytes_size = 0u;
    uint64_t version = 0u, algorithm = 0u, uid = 0u, iat = 0u, exp = 0u;
    if (!cborBlob(&reader, 3u, &text, &text_size) ||
        !textEquals(text, text_size, "FMO") ||
        !cborUint(&reader, &version) || version != 4u ||
        !cborBlob(&reader, 3u, &text, &text_size) ||
        !textEquals(text, text_size, "userCert") ||
        !cborUint(&reader, &algorithm) || algorithm > UINT32_MAX ||
        !cborBlob(&reader, 3u, &text, &text_size) || text_size == 0u ||
        text_size >= sizeof(certificate->callsign)) {
        return false;
    }
    memset(certificate, 0, sizeof(*certificate));
    memcpy(certificate->callsign, text, text_size);
    if (!cborUint(&reader, &uid) || uid > UINT32_MAX ||
        !cborBlob(&reader, 2u, &bytes, &bytes_size) || bytes_size != 32u) {
        return false;
    }
    memcpy(certificate->public_key, bytes, 32u);
    if (!cborUint(&reader, &iat) || !cborUint(&reader, &exp)) return false;
    const size_t signature_item = reader.offset;
    if (!cborBlob(&reader, 2u, &bytes, &bytes_size) || bytes_size != 64u ||
        reader.offset != raw_size) {
        return false;
    }
    uint8_t tbs[512];
    const size_t payload = signature_item - first_item;
    if (payload + 1u > sizeof(tbs)) return false;
    tbs[0] = 0x89u;
    memcpy(tbs + 1u, raw + first_item, payload);
    crypto_hash_sha256(certificate->fingerprint, tbs, payload + 1u);
    certificate->uid = static_cast<uint32_t>(uid);
    certificate->algorithm = static_cast<uint32_t>(algorithm);
    certificate->issued_at = iat;
    certificate->expires_at = exp;
    return true;
}
