// Host unit test for the FMO-V4 STATION broadcast encoders
// (src/services/fmo_station_broadcast_core.cpp) plus a decode/re-encode
// roundtrip of a real on-air CERT blob through src/services/fmo_protocol.cpp.
//
// The CERT/TBS reference vectors below come from a real-network capture
// (fmo-sim .tmp/aprs_fullfeed.log, station BD3QDG-1) and the independent
// Python verifier fmo-sim/.tmp/verify_tbs7.py (30/30 real stations verified).
//
// Build & run (from the repo root, MSYS2/MinGW g++ works):
//   g++ -std=c++17 -Wall -Wextra -I src -I tests/shims
//       tests/fmo_station_broadcast_test.cpp
//       src/services/fmo_station_broadcast_core.cpp
//       src/services/fmo_protocol.cpp
//       -o .tmp/fmo_station_broadcast_test.exe
//   ./.tmp/fmo_station_broadcast_test.exe

#include "services/fmo_protocol.h"
#include "services/fmo_station_broadcast_core.h"

#include <sodium.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Real capture: BD3QDG-1>APFMO4,TCPIP*:=3952.80NF11931.57EiFMO-V4,STATION,...
const char kCertB64[] =
    "imNGTU8EaHVzZXJDZXJ0GQPpZkJEM1FERxkFnFggxVxvNn4SH7Q_CvOxroc4vi0g20apUg"
    "BuG11QzS3b5o0aam7oOBpsUBu4WECNdBggJmbdaS4jXQ_YPrnpyttHOeX8emdDqH7B35RqB"
    "6dJWsCGQ1-bYyn5LeG2872pswpP-0wsnBv7xOSwqkwL";

size_t hexToBytes(const char *hex, uint8_t *out, const size_t capacity)
{
    size_t count = 0u;
    while (hex[0] != '\0' && hex[1] != '\0' && count < capacity) {
        unsigned value = 0u;
        (void)sscanf(hex, "%2x", &value);
        out[count++] = static_cast<uint8_t>(value);
        hex += 2;
    }
    return count;
}

void bytesToHex(const uint8_t *data, const size_t size, char *out)
{
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2u] = kDigits[data[i] >> 4u];
        out[i * 2u + 1u] = kDigits[data[i] & 15u];
    }
    out[size * 2u] = '\0';
}

// ---------------------------------------------------------------- SHA-256 ---
// Compact public-domain-style SHA-256 for the shim (test only).

struct Sha256 {
    uint32_t h[8];
    uint8_t block[64];
    size_t used;
    uint64_t total;
};

const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t rotr(const uint32_t x, const unsigned n) { return (x >> n) | (x << (32u - n)); }

void sha256Block(Sha256 *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    for (size_t i = 0; i < 16u; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4u]) << 24u) |
               (static_cast<uint32_t>(block[i * 4u + 1u]) << 16u) |
               (static_cast<uint32_t>(block[i * 4u + 2u]) << 8u) |
               block[i * 4u + 3u];
    }
    for (size_t i = 16u; i < 64u; ++i) {
        const uint32_t s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
        const uint32_t s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (size_t i = 0; i < 64u; ++i) {
        const uint32_t s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256Init(Sha256 *ctx)
{
    static const uint32_t kInit[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(ctx->h, kInit, sizeof(kInit));
    ctx->used = 0u;
    ctx->total = 0u;
}

void sha256Update(Sha256 *ctx, const uint8_t *data, size_t size)
{
    ctx->total += size;
    while (size > 0u) {
        const size_t take = 64u - ctx->used < size ? 64u - ctx->used : size;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        size -= take;
        if (ctx->used == 64u) {
            sha256Block(ctx, ctx->block);
            ctx->used = 0u;
        }
    }
}

void sha256Final(Sha256 *ctx, uint8_t out[32])
{
    const uint64_t bits = ctx->total * 8u;
    uint8_t pad = 0x80u;
    sha256Update(ctx, &pad, 1u);
    uint8_t zero = 0u;
    while (ctx->used != 56u) sha256Update(ctx, &zero, 1u);
    uint8_t length[8];
    for (size_t i = 0; i < 8u; ++i) length[i] = static_cast<uint8_t>(bits >> (56u - i * 8u));
    sha256Update(ctx, length, 8u);
    for (size_t i = 0; i < 8u; ++i) {
        out[i * 4u] = static_cast<uint8_t>(ctx->h[i] >> 24u);
        out[i * 4u + 1u] = static_cast<uint8_t>(ctx->h[i] >> 16u);
        out[i * 4u + 2u] = static_cast<uint8_t>(ctx->h[i] >> 8u);
        out[i * 4u + 3u] = static_cast<uint8_t>(ctx->h[i]);
    }
}

// ------------------------------------------------------------ shim bodies ---
int b64urlValue(const char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

} // namespace

int sodium_init(void) { return 0; }

int sodium_base642bin(unsigned char *bin, const size_t bin_maxlen,
                      const char *b64, const size_t b64_len, const char *,
                      size_t *bin_len, const char **b64_end, const int)
{
    size_t out = 0u;
    uint32_t acc = 0u;
    int bits = 0;
    for (size_t i = 0; i < b64_len; ++i) {
        const int v = b64urlValue(b64[i]);
        if (v < 0) return -1;
        acc = (acc << 6u) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= bin_maxlen) return -1;
            bin[out++] = static_cast<unsigned char>(acc >> bits);
        }
    }
    if (bin_len != nullptr) *bin_len = out;
    if (b64_end != nullptr) *b64_end = b64 + b64_len;
    return 0;
}

int crypto_hash_sha256(unsigned char *out, const unsigned char *in,
                       const unsigned long long inlen)
{
    Sha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, in, static_cast<size_t>(inlen));
    sha256Final(&ctx, out);
    return 0;
}

namespace {

size_t b64urlDecodeLocal(const char *text, uint8_t *out, const size_t capacity)
{
    size_t size = 0u;
    assert(sodium_base642bin(out, capacity, text, strlen(text), nullptr, &size,
                             nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) == 0);
    return size;
}

void testSha256Shim()
{
    uint8_t hash[32];
    char hex[65];
    crypto_hash_sha256(hash, reinterpret_cast<const uint8_t *>("abc"), 3u);
    bytesToHex(hash, 32u, hex);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    printf("ok: sha256 shim\n");
}

void testCoordinates()
{
    char lat[10], lon[11];
    FMO_STATION_CORE_FormatLat(39.88, lat);
    assert(strcmp(lat, "3952.80N") == 0);
    FMO_STATION_CORE_FormatLon(119.0 + 31.57 / 60.0, lon);
    assert(strcmp(lon, "11931.57E") == 0);
    FMO_STATION_CORE_FormatLat(-1.5, lat);
    assert(strcmp(lat, "0130.00S") == 0);
    FMO_STATION_CORE_FormatLon(-118.25, lon);
    assert(strcmp(lon, "11815.00W") == 0);
    // Truncation parity with the vendored ParseAPRS::deg2lat: whole minutes
    // truncate, hundredths round without carry.
    FMO_STATION_CORE_FormatLat(39.999999, lat);
    assert(strcmp(lat, "3959.100N") == 0);
    printf("ok: coordinate formatting\n");
}

void testBase64Url()
{
    const uint8_t edge[3] = {0xfbu, 0xffu, 0xfeu};
    char out[16];
    assert(FMO_STATION_CORE_Base64UrlEncode(edge, 3u, out, sizeof(out)) == 4u);
    assert(strcmp(out, "-__-") == 0);
    assert(FMO_STATION_CORE_Base64UrlEncode(edge, 1u, out, sizeof(out)) == 2u);
    assert(strcmp(out, "-w") == 0);
    assert(FMO_STATION_CORE_Base64UrlEncode(edge, 2u, out, sizeof(out)) == 3u);
    assert(strcmp(out, "-_8") == 0);
    printf("ok: base64url\n");
}

void testUtf8ToGbk()
{
    // The on-air text fields are UTF-8 now, so the broadcast path no longer
    // calls this; the retained helper is still exercised here. ASCII passes
    // through unchanged, mapped CJK converts, unmappable degrades to '?'.
    uint8_t out[64];
    size_t size = FMO_STATION_CORE_Utf8ToGbk("河北秦皇岛", out, sizeof(out));
    uint8_t expected[10];
    assert(hexToBytes("bad3b1b1c7d8bbcab5ba", expected, sizeof(expected)) == size);
    assert(memcmp(out, expected, size) == 0);
    size = FMO_STATION_CORE_Utf8ToGbk("AB中C", out, sizeof(out));
    assert(size == 5u);
    assert(out[0] == 'A' && out[1] == 'B' && out[2] == 0xd6u && out[3] == 0xd0u && out[4] == 'C');
    // Unmappable (emoji) degrades to '?'.
    size = FMO_STATION_CORE_Utf8ToGbk("\xf0\x9f\x98\x80", out, sizeof(out));
    assert(size == 1u && out[0] == '?');
    printf("ok: utf8->gbk (retained legacy helper)\n");
}

void testGbkEncodedSize()
{
    size_t size = 0;
    assert(FMO_STATION_CORE_GbkEncodedSize("AB中C", &size) && size == 5u);
    assert(FMO_STATION_CORE_GbkEncodedSize("", &size) && size == 0u);
    // 32 CJK characters = 64 GBK bytes.
    assert(FMO_STATION_CORE_GbkEncodedSize(
               "中中中中中中中中中中中中中中中中中中中中中中中中中中中中中中中中",
               &size) &&
           size == 64u);
    // Emoji has no GBK mapping and is reported unmappable.
    assert(!FMO_STATION_CORE_GbkEncodedSize("\xf0\x9f\x98\x80", &size));
    printf("ok: gbk encoded size (retained legacy helper)\n");
}

void testCertBlobRoundtrip()
{
    // Decode the real blob through the firmware decoder, re-encode with the
    // broadcast builder, and require byte equality.
    uint8_t raw[512];
    const size_t raw_size = b64urlDecodeLocal(kCertB64, raw, sizeof(raw));
    assert(raw_size == 138u);

    FmoPublicCertificate cert = {};
    assert(FMO_PROTOCOL_ParseBeaconCertificate(kCertB64, &cert));
    assert(strcmp(cert.callsign, "BD3QDG") == 0);
    assert(cert.uid == 1436u);
    assert(cert.algorithm == 1001u);
    assert(cert.issued_at == 1785653304ULL);
    assert(cert.expires_at == 1817189304ULL);

    // The 10th element (64-byte signature) is the tail: 0x58 0x40 <64B>.
    assert(raw_size >= 66u && raw[raw_size - 66u] == 0x58u &&
           raw[raw_size - 65u] == 0x40u);

    FmoStationCertFields fields = {};
    fields.issuer_sn = cert.algorithm;
    snprintf(fields.callsign, sizeof(fields.callsign), "%s", cert.callsign);
    fields.uid = cert.uid;
    memcpy(fields.public_key, cert.public_key, 32u);
    fields.issued_at = cert.issued_at;
    fields.expires_at = cert.expires_at;
    memcpy(fields.signature, raw + raw_size - 64u, 64u);

    uint8_t rebuilt[512];
    size_t rebuilt_size = 0u;
    assert(FMO_STATION_CORE_BuildCertBlob(&fields, rebuilt, sizeof(rebuilt),
                                          &rebuilt_size));
    assert(rebuilt_size == raw_size);
    assert(memcmp(rebuilt, raw, raw_size) == 0);

    // b64url of the rebuilt blob must reproduce the on-air CERT string.
    char encoded[720];
    assert(FMO_STATION_CORE_Base64UrlEncode(rebuilt, rebuilt_size, encoded,
                                            sizeof(encoded)) == strlen(kCertB64));
    assert(strcmp(encoded, kCertB64) == 0);
    printf("ok: cert blob decode/re-encode roundtrip\n");
}

void testTbsLayout()
{
    uint8_t raw[512];
    const size_t raw_size = b64urlDecodeLocal(kCertB64, raw, sizeof(raw));
    FmoStationTbsParams params = {};
    snprintf(params.callsign, sizeof(params.callsign), "%s", "bd3qdg"); // uppercased by builder
    params.ssid = 1u; // header callsign was BD3QDG-1
    snprintf(params.lat, sizeof(params.lat), "%s", "3952.80N");
    snprintf(params.lon, sizeof(params.lon), "%s", "11931.57E");
    crypto_hash_sha256(params.cert_blob_hash, raw, raw_size);
    snprintf(params.country, sizeof(params.country), "%s", "cn"); // uppercased by builder
    params.name_utf8 = "河北秦皇岛";
    params.host = "fmo.my.hellowrold.cn";
    params.port = 1883u;
    params.cover_km = 5000u;
    params.online = 2u;
    params.peak = 12u;
    params.time_slot = 2970000u;

    uint8_t tbs[512];
    size_t tbs_size = 0u;
    assert(FMO_STATION_CORE_BuildTbs(&params, tbs, sizeof(tbs), &tbs_size));

    // Reference bytes built with fmo-sim/.tmp/verify_tbs7.py's builder.
    uint8_t expected[512];
    const size_t expected_size = hexToBytes(
        "9063464d4f046753544154494f4e664244335144470168333935322e38304e6931"
        "313933312e3537455820cd588a2e67779dc3e6212ae219a4ae914fb30759f2d1c267"
        "9678f855e5f16b1e62434e6fe6b2b3e58c97e7a7a6e79a87e5b29b74666d6f2e6d79"
        "2e68656c6c6f77726f6c642e636e19075b191388020c1a002d5190",
        expected, sizeof(expected));
    assert(tbs_size == expected_size);
    if (memcmp(tbs, expected, tbs_size) != 0u) {
        char got_hex[1025], want_hex[1025];
        bytesToHex(tbs, tbs_size, got_hex);
        bytesToHex(expected, expected_size, want_hex);
        printf("TBS mismatch:\n got  %s\n want %s\n", got_hex, want_hex);
        assert(false);
    }

    // certBlobHash must be SHA-256 over the raw CERT blob bytes.
    char hash_hex[65];
    bytesToHex(params.cert_blob_hash, 32u, hash_hex);
    assert(strcmp(hash_hex,
                  "cd588a2e67779dc3e6212ae219a4ae914fb30759f2d1c2679678f855e5f16b1e") == 0);
    printf("ok: 16-element TBS layout\n");
}

} // namespace

void testBeaconTbs()
{
    // Reference vectors built with cbor2 using the official BEACON TBS
    // layout (fmo-sim .tmp/verify_beacon.py, 9/9 real captures verified);
    // generated by fmo-sim/.tmp/gen_beacon_vectors.py.
    uint8_t raw[512];
    const size_t raw_size = b64urlDecodeLocal(kCertB64, raw, sizeof(raw));
    FmoBeaconTbsParams params = {};
    snprintf(params.callsign, sizeof(params.callsign), "%s", "bd3qdg"); // uppercased by builder
    params.ssid = 1u; // header callsign was BD3QDG-1
    snprintf(params.lat, sizeof(params.lat), "%s", "3952.80N");
    snprintf(params.lon, sizeof(params.lon), "%s", "11931.57E");
    crypto_hash_sha256(params.cert_blob_hash, raw, raw_size);
    snprintf(params.freq, sizeof(params.freq), "%s", "439.1625");
    params.time_slot = 2970000u;

    uint8_t tbs[512];
    size_t tbs_size = 0u;
    uint8_t expected[512];

    // 10 elements: freq only, no height/rig/ant.
    params.height_m = 0u;
    params.rig_utf8 = "";
    params.ant_utf8 = nullptr; // NULL and empty both mean "omit"
    assert(FMO_STATION_CORE_BuildBeaconTbs(&params, tbs, sizeof(tbs), &tbs_size));
    const size_t size10 = hexToBytes(
        "8a63464d4f0466424541434f4e664244335144470168333935322e38304e69313139"
        "33312e3537455820cd588a2e67779dc3e6212ae219a4ae914fb30759f2d1c2679678f8"
        "55e5f16b1e683433392e313632351a002d5190",
        expected, sizeof(expected));
    assert(tbs_size == size10 && memcmp(tbs, expected, tbs_size) == 0u);

    // 11 elements: + height.
    params.height_m = 18u;
    assert(FMO_STATION_CORE_BuildBeaconTbs(&params, tbs, sizeof(tbs), &tbs_size));
    const size_t size11 = hexToBytes(
        "8b63464d4f0466424541434f4e664244335144470168333935322e38304e69313139"
        "33312e3537455820cd588a2e67779dc3e6212ae219a4ae914fb30759f2d1c2679678f8"
        "55e5f16b1e683433392e31363235121a002d5190",
        expected, sizeof(expected));
    assert(tbs_size == size11 && memcmp(tbs, expected, tbs_size) == 0u);

    // 13 elements: + height + rig + ant (TBS carries UTF-8; the wire RIG:/ANT:
    // fields carry the same UTF-8 bytes).
    params.rig_utf8 = "协谷 G90";
    params.ant_utf8 = "车载苗子";
    assert(FMO_STATION_CORE_BuildBeaconTbs(&params, tbs, sizeof(tbs), &tbs_size));
    const size_t size13 = hexToBytes(
        "8d63464d4f0466424541434f4e664244335144470168333935322e38304e69313139"
        "33312e3537455820cd588a2e67779dc3e6212ae219a4ae914fb30759f2d1c2679678f8"
        "55e5f16b1e683433392e31363235126ae58d8fe8b0b7204739306ce8bda6e8bdbde88b"
        "97e5ad901a002d5190",
        expected, sizeof(expected));
    assert(tbs_size == size13 && memcmp(tbs, expected, tbs_size) == 0u);
    printf("ok: 10/11/13-element BEACON TBS layout\n");
}

int main()
{
    testSha256Shim();
    testCoordinates();
    testBase64Url();
    testUtf8ToGbk();
    testGbkEncodedSize();
    testCertBlobRoundtrip();
    testTbsLayout();
    testBeaconTbs();
    printf("all fmo_station_broadcast tests passed\n");
    return 0;
}
