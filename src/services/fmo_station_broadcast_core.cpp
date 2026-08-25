#include "services/fmo_station_broadcast_core.h"

#include "media/gbk_unicode_table.generated.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

// Deterministic (shortest-form) CBOR writer, same encoding rules as the one
// in fmo_cert_store.cpp: uint/text/bytes heads use the smallest additional
// information form.
struct CborWriter {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool ok;
};

void cborBytes(CborWriter *writer, const void *data, const size_t size)
{
    if (!writer->ok || writer->size + size > writer->capacity) {
        writer->ok = false;
        return;
    }
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
}

void cborHead(CborWriter *writer, const uint8_t major, const uint64_t value)
{
    uint8_t encoded[9];
    size_t size = 1u;
    if (value < 24u) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | value);
    } else if (value <= UINT8_MAX) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 24u);
        encoded[1] = static_cast<uint8_t>(value);
        size = 2u;
    } else if (value <= UINT16_MAX) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 25u);
        encoded[1] = static_cast<uint8_t>(value >> 8u);
        encoded[2] = static_cast<uint8_t>(value);
        size = 3u;
    } else if (value <= UINT32_MAX) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 26u);
        for (size_t i = 0; i < 4u; ++i) {
            encoded[1u + i] = static_cast<uint8_t>(value >> (24u - i * 8u));
        }
        size = 5u;
    } else {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 27u);
        for (size_t i = 0; i < 8u; ++i) {
            encoded[1u + i] = static_cast<uint8_t>(value >> (56u - i * 8u));
        }
        size = 9u;
    }
    cborBytes(writer, encoded, size);
}

void cborUint(CborWriter *writer, const uint64_t value)
{
    cborHead(writer, 0u, value);
}

void cborTextN(CborWriter *writer, const char *text, const size_t size)
{
    cborHead(writer, 3u, size);
    cborBytes(writer, text, size);
}

void cborText(CborWriter *writer, const char *text)
{
    cborTextN(writer, text, strlen(text));
}

void cborBlob(CborWriter *writer, const uint8_t *data, const size_t size)
{
    cborHead(writer, 2u, size);
    cborBytes(writer, data, size);
}

void upperAscii(char *text)
{
    for (char *p = text; *p != '\0'; ++p) {
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
    }
}

// Decode one UTF-8 code point. Returns the code point (U+FFFD on malformed
// input) and advances *next past the consumed bytes.
uint32_t utf8Next(const char **cursor)
{
    const auto *p = reinterpret_cast<const uint8_t *>(*cursor);
    uint32_t cp = 0xfffd;
    size_t used = 1u;
    if (p[0] < 0x80u) {
        cp = p[0];
    } else if ((p[0] & 0xe0u) == 0xc0u && (p[1] & 0xc0u) == 0x80u) {
        cp = (static_cast<uint32_t>(p[0] & 0x1fu) << 6u) | (p[1] & 0x3fu);
        used = 2u;
    } else if ((p[0] & 0xf0u) == 0xe0u && (p[1] & 0xc0u) == 0x80u &&
               (p[2] & 0xc0u) == 0x80u) {
        cp = (static_cast<uint32_t>(p[0] & 0x0fu) << 12u) |
             (static_cast<uint32_t>(p[1] & 0x3fu) << 6u) | (p[2] & 0x3fu);
        used = 3u;
    } else if ((p[0] & 0xf8u) == 0xf0u && (p[1] & 0xc0u) == 0x80u &&
               (p[2] & 0xc0u) == 0x80u && (p[3] & 0xc0u) == 0x80u) {
        cp = (static_cast<uint32_t>(p[0] & 0x07u) << 18u) |
             (static_cast<uint32_t>(p[1] & 0x3fu) << 12u) |
             (static_cast<uint32_t>(p[2] & 0x3fu) << 6u) | (p[3] & 0x3fu);
        used = 4u;
    }
    *cursor += used;
    return cp;
}

// Linear reverse lookup in the GBK->Unicode table. Returns the two-byte GBK
// code, or 0 when the code point is not representable in GBK.
uint16_t gbkFromUnicode(const uint32_t cp)
{
    if (cp == 0u || cp > 0xffffu) return 0u;
    constexpr size_t kSlots =
        (GBK_TABLE_LEAD_LAST - GBK_TABLE_LEAD_FIRST + 1u) * GBK_TABLE_TRAIL_SPAN;
    for (size_t i = 0; i < kSlots; ++i) {
        if (kGbkToUnicode[i] == cp) {
            const uint16_t lead = static_cast<uint16_t>(
                GBK_TABLE_LEAD_FIRST + i / GBK_TABLE_TRAIL_SPAN);
            const uint16_t trail = static_cast<uint16_t>(
                GBK_TABLE_TRAIL_FIRST + i % GBK_TABLE_TRAIL_SPAN);
            return static_cast<uint16_t>((lead << 8u) | trail);
        }
    }
    return 0u;
}

} // namespace

extern "C" bool FMO_STATION_CORE_BuildCertBlob(
    const FmoStationCertFields *cert, uint8_t *out, const size_t capacity,
    size_t *out_size)
{
    if (cert == nullptr || out == nullptr || out_size == nullptr) return false;
    CborWriter writer = {out, capacity, 0u, true};
    cborHead(&writer, 4u, 10u);
    cborText(&writer, "FMO");
    cborUint(&writer, 4u);
    cborText(&writer, "userCert");
    cborUint(&writer, cert->issuer_sn);
    cborText(&writer, cert->callsign);
    cborUint(&writer, cert->uid);
    cborBlob(&writer, cert->public_key, sizeof(cert->public_key));
    cborUint(&writer, cert->issued_at);
    cborUint(&writer, cert->expires_at);
    cborBlob(&writer, cert->signature, sizeof(cert->signature));
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

extern "C" bool FMO_STATION_CORE_BuildTbs(const FmoStationTbsParams *params,
                                          uint8_t *out, const size_t capacity,
                                          size_t *out_size)
{
    if (params == nullptr || out == nullptr || out_size == nullptr ||
        params->name_utf8 == nullptr || params->host == nullptr) {
        return false;
    }
    char call[sizeof(params->callsign)];
    snprintf(call, sizeof(call), "%s", params->callsign);
    upperAscii(call);
    char country[sizeof(params->country)];
    snprintf(country, sizeof(country), "%s", params->country);
    upperAscii(country);
    CborWriter writer = {out, capacity, 0u, true};
    cborHead(&writer, 4u, 16u);
    cborText(&writer, "FMO");
    cborUint(&writer, 4u);
    cborText(&writer, "STATION");
    cborText(&writer, call);
    cborUint(&writer, params->ssid);
    cborText(&writer, params->lat);
    cborText(&writer, params->lon);
    cborBlob(&writer, params->cert_blob_hash, sizeof(params->cert_blob_hash));
    cborText(&writer, country);
    cborText(&writer, params->name_utf8);
    cborText(&writer, params->host);
    cborUint(&writer, params->port);
    cborUint(&writer, params->cover_km);
    cborUint(&writer, params->online);
    cborUint(&writer, params->peak);
    cborUint(&writer, params->time_slot);
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

extern "C" bool FMO_STATION_CORE_BuildBeaconTbs(
    const FmoBeaconTbsParams *params, uint8_t *out, const size_t capacity,
    size_t *out_size)
{
    if (params == nullptr || out == nullptr || out_size == nullptr) {
        return false;
    }
    const bool has_rig = params->rig_utf8 != nullptr && params->rig_utf8[0] != '\0';
    const bool has_ant = params->ant_utf8 != nullptr && params->ant_utf8[0] != '\0';
    const uint64_t elements = 10u + (params->height_m > 0u ? 1u : 0u) +
                              (has_rig ? 1u : 0u) + (has_ant ? 1u : 0u);
    char call[sizeof(params->callsign)];
    snprintf(call, sizeof(call), "%s", params->callsign);
    upperAscii(call);
    CborWriter writer = {out, capacity, 0u, true};
    cborHead(&writer, 4u, elements);
    cborText(&writer, "FMO");
    cborUint(&writer, 4u);
    cborText(&writer, "BEACON");
    cborText(&writer, call);
    cborUint(&writer, params->ssid);
    cborText(&writer, params->lat);
    cborText(&writer, params->lon);
    cborBlob(&writer, params->cert_blob_hash, sizeof(params->cert_blob_hash));
    cborText(&writer, params->freq);
    if (params->height_m > 0u) {
        cborUint(&writer, params->height_m);
    }
    if (has_rig) {
        cborText(&writer, params->rig_utf8);
    }
    if (has_ant) {
        cborText(&writer, params->ant_utf8);
    }
    cborUint(&writer, params->time_slot);
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

extern "C" void FMO_STATION_CORE_FormatLat(double deg, char out[10])
{
    // Mirrors ParseAPRS::deg2lat exactly (floor degrees, truncated whole
    // minutes, rounded hundredths, 0.0 treated as southern hemisphere).
    char sign = 'S';
    if (deg > 0.0) {
        sign = 'N';
    } else {
        deg = -deg;
    }
    const unsigned id = static_cast<unsigned>(floor(deg));
    const unsigned im = static_cast<unsigned>((deg - static_cast<double>(id)) * 60.0);
    const unsigned imm = static_cast<unsigned>(
        round(((deg - static_cast<double>(id)) * 60.0 - static_cast<double>(im)) * 100.0));
    snprintf(out, 10u, "%02u%02u.%02u%c", id, im, imm, sign);
}

extern "C" void FMO_STATION_CORE_FormatLon(double deg, char out[11])
{
    // Mirrors ParseAPRS::deg2lon exactly.
    char sign = 'W';
    if (deg > 0.0) {
        sign = 'E';
    } else {
        deg = -deg;
    }
    const unsigned id = static_cast<unsigned>(floor(deg));
    const unsigned im = static_cast<unsigned>((deg - static_cast<double>(id)) * 60.0);
    const unsigned imm = static_cast<unsigned>(
        round(((deg - static_cast<double>(id)) * 60.0 - static_cast<double>(im)) * 100.0));
    snprintf(out, 11u, "%03u%02u.%02u%c", id, im, imm, sign);
}

extern "C" size_t FMO_STATION_CORE_Base64UrlEncode(
    const uint8_t *data, const size_t size, char *out, const size_t capacity)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (data == nullptr || out == nullptr) return 0u;
    const size_t needed = (size / 3u) * 4u + (size % 3u != 0u ? size % 3u + 1u : 0u);
    if (capacity < needed + 1u) return 0u;
    size_t o = 0u;
    size_t i = 0u;
    for (; i + 3u <= size; i += 3u) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16u) |
                           (static_cast<uint32_t>(data[i + 1u]) << 8u) | data[i + 2u];
        out[o++] = kAlphabet[v >> 18u];
        out[o++] = kAlphabet[(v >> 12u) & 63u];
        out[o++] = kAlphabet[(v >> 6u) & 63u];
        out[o++] = kAlphabet[v & 63u];
    }
    if (i < size) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16u |
                           (i + 1u < size ? static_cast<uint32_t>(data[i + 1u]) << 8u : 0u);
        out[o++] = kAlphabet[v >> 18u];
        out[o++] = kAlphabet[(v >> 12u) & 63u];
        if (i + 1u < size) out[o++] = kAlphabet[(v >> 6u) & 63u];
    }
    out[o] = '\0';
    return o;
}

extern "C" size_t FMO_STATION_CORE_Utf8ToGbk(const char *utf8, uint8_t *out,
                                             const size_t capacity)
{
    if (utf8 == nullptr || out == nullptr) return 0u;
    size_t used = 0u;
    const char *cursor = utf8;
    while (*cursor != '\0') {
        const uint32_t cp = utf8Next(&cursor);
        if (cp < 0x80u) {
            if (used + 1u > capacity) break;
            out[used++] = static_cast<uint8_t>(cp);
            continue;
        }
        const uint16_t gbk = gbkFromUnicode(cp);
        if (gbk != 0u) {
            if (used + 2u > capacity) break;
            out[used++] = static_cast<uint8_t>(gbk >> 8u);
            out[used++] = static_cast<uint8_t>(gbk & 0xffu);
        } else {
            if (used + 1u > capacity) break;
            out[used++] = '?';
        }
    }
    return used;
}

extern "C" bool FMO_STATION_CORE_GbkEncodedSize(const char *utf8,
                                                size_t *out_size)
{
    if (utf8 == nullptr || out_size == nullptr) return false;
    size_t used = 0u;
    const char *cursor = utf8;
    while (*cursor != '\0') {
        const uint32_t cp = utf8Next(&cursor);
        if (cp < 0x80u) {
            ++used;
            continue;
        }
        if (gbkFromUnicode(cp) == 0u) return false;
        used += 2u;
    }
    *out_size = used;
    return true;
}

namespace {

void rosterEvictExpired(FmoOnlineRoster *roster, const uint32_t now_s)
{
    size_t kept = 0u;
    for (size_t i = 0u; i < roster->count; ++i) {
        // Unsigned subtraction stays correct across a now_s wraparound.
        if (now_s - roster->entries[i].last_seen_s <
            FMO_STATION_ONLINE_WINDOW_S) {
            roster->entries[kept++] = roster->entries[i];
        }
    }
    roster->count = static_cast<uint32_t>(kept);
}

} // namespace

extern "C" void FMO_STATION_CORE_RosterReset(FmoOnlineRoster *roster)
{
    if (roster == nullptr) return;
    memset(roster, 0, sizeof(*roster));
}

extern "C" uint32_t FMO_STATION_CORE_RosterFeed(FmoOnlineRoster *roster,
                                                const uint32_t uid,
                                                const uint32_t now_s)
{
    if (roster == nullptr) return 0u;
    rosterEvictExpired(roster, now_s);
    for (size_t i = 0u; i < roster->count; ++i) {
        if (roster->entries[i].uid == uid) {
            roster->entries[i].last_seen_s = now_s;
            return roster->count;
        }
    }
    if (roster->count >= FMO_STATION_ROSTER_CAPACITY) {
        // Full: replace the oldest entry.
        size_t oldest = 0u;
        for (size_t i = 1u; i < roster->count; ++i) {
            if (roster->entries[i].last_seen_s <
                roster->entries[oldest].last_seen_s) {
                oldest = i;
            }
        }
        roster->entries[oldest].uid = uid;
        roster->entries[oldest].last_seen_s = now_s;
        return roster->count;
    }
    roster->entries[roster->count].uid = uid;
    roster->entries[roster->count].last_seen_s = now_s;
    ++roster->count;
    if (roster->count > roster->session_peak) {
        roster->session_peak = roster->count;
    }
    return roster->count;
}

extern "C" uint32_t FMO_STATION_CORE_RosterOnline(FmoOnlineRoster *roster,
                                                  const uint32_t now_s)
{
    if (roster == nullptr) return 0u;
    rosterEvictExpired(roster, now_s);
    return roster->count;
}

extern "C" uint32_t FMO_STATION_CORE_EffectiveCount(const uint32_t configured,
                                                    const uint32_t automatic)
{
    return configured != 0u ? configured : automatic;
}
