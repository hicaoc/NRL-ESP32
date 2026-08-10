#include "media/media_metadata.h"
#include "media/gbk_unicode_table.generated.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "MMETA";

namespace {

constexpr size_t kMaxCoverBytes = 512 * 1024; // sane cap for embedded art
constexpr uint32_t kMaxTextFrameBytes = 1024;

// ---- helpers ---------------------------------------------------------------

static uint32_t be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0] & 0x7Fu) << 21) |
           (static_cast<uint32_t>(p[1] & 0x7Fu) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7Fu) << 7) |
           (p[3] & 0x7Fu);
}

static bool read_exact(FILE *f, void *dst, const size_t n)
{
    return fread(dst, 1, n, f) == n;
}

// Append one Unicode code point as UTF-8 (bounded).
static void utf8_append(char *out, size_t *pos, const size_t cap, uint32_t cp)
{
    if (cp == 0u) {
        return;
    }
    if (cp < 0x80u) {
        if (*pos + 1u < cap) { out[(*pos)++] = static_cast<char>(cp); }
    } else if (cp < 0x800u) {
        if (*pos + 2u < cap) {
            out[(*pos)++] = static_cast<char>(0xC0u | (cp >> 6));
            out[(*pos)++] = static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    } else if (cp < 0x10000u) {
        if (*pos + 3u < cap) {
            out[(*pos)++] = static_cast<char>(0xE0u | (cp >> 12));
            out[(*pos)++] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out[(*pos)++] = static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    } else {
        if (*pos + 4u < cap) {
            out[(*pos)++] = static_cast<char>(0xF0u | (cp >> 18));
            out[(*pos)++] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            out[(*pos)++] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out[(*pos)++] = static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }
}

static void utf16_to_utf8(const uint8_t *data, size_t bytes, bool big_endian,
                          char *out, const size_t cap)
{
    size_t pos = 0;
    size_t i = 0;
    while (i + 1u < bytes) {
        uint32_t unit = big_endian
                            ? ((static_cast<uint32_t>(data[i]) << 8) | data[i + 1u])
                            : ((static_cast<uint32_t>(data[i + 1u]) << 8) | data[i]);
        i += 2u;
        if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1u < bytes) { // surrogate pair
            const uint32_t low = big_endian
                                     ? ((static_cast<uint32_t>(data[i]) << 8) | data[i + 1u])
                                     : ((static_cast<uint32_t>(data[i + 1u]) << 8) | data[i]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                i += 2u;
                unit = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
            }
        }
        utf8_append(out, &pos, cap, unit);
    }
    out[pos] = '\0';
}

static void latin1_to_utf8(const uint8_t *data, size_t bytes, char *out, const size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < bytes && data[i] != 0u; ++i) {
        utf8_append(out, &pos, cap, data[i]);
    }
    out[pos] = '\0';
}

// True when the (NUL-terminated portion of the) buffer is well-formed UTF-8
// with at least the given strictness; pure ASCII also passes.
static bool is_valid_utf8(const uint8_t *data, const size_t bytes)
{
    size_t i = 0;
    while (i < bytes && data[i] != 0u) {
        const uint8_t b = data[i];
        size_t follow = 0;
        if (b < 0x80u) {
            follow = 0;
        } else if ((b & 0xE0u) == 0xC0u && b >= 0xC2u) {
            follow = 1;
        } else if ((b & 0xF0u) == 0xE0u) {
            follow = 2;
        } else if ((b & 0xF8u) == 0xF0u && b <= 0xF4u) {
            follow = 3;
        } else {
            return false;
        }
        if (i + follow >= bytes) {
            return false;
        }
        for (size_t j = 1; j <= follow; ++j) {
            if ((data[i + j] & 0xC0u) != 0x80u) {
                return false;
            }
        }
        i += follow + 1u;
    }
    return true;
}

// Unicode code point for a GBK two-byte sequence, or 0 when unassigned.
static uint32_t gbk_lookup(const uint8_t lead, const uint8_t trail)
{
    if (lead < GBK_TABLE_LEAD_FIRST || lead > GBK_TABLE_LEAD_LAST ||
        trail < GBK_TABLE_TRAIL_FIRST || trail > GBK_TABLE_TRAIL_LAST) {
        return 0;
    }
    return kGbkToUnicode[(lead - GBK_TABLE_LEAD_FIRST) * GBK_TABLE_TRAIL_SPAN +
                         (trail - GBK_TABLE_TRAIL_FIRST)];
}

// True when every non-ASCII byte forms a mapped GBK pair.
static bool is_valid_gbk(const uint8_t *data, const size_t bytes)
{
    size_t i = 0;
    bool any_pair = false;
    while (i < bytes && data[i] != 0u) {
        if (data[i] < 0x80u) {
            ++i;
            continue;
        }
        if (i + 1u >= bytes || data[i + 1u] == 0u || gbk_lookup(data[i], data[i + 1u]) == 0u) {
            return false;
        }
        any_pair = true;
        i += 2u;
    }
    return any_pair;
}

static void gbk_to_utf8(const uint8_t *data, const size_t bytes, char *out, const size_t cap)
{
    size_t pos = 0;
    size_t i = 0;
    while (i < bytes && data[i] != 0u) {
        if (data[i] < 0x80u) {
            utf8_append(out, &pos, cap, data[i]);
            ++i;
            continue;
        }
        const uint32_t cp = (i + 1u < bytes) ? gbk_lookup(data[i], data[i + 1u]) : 0u;
        if (cp != 0u) {
            utf8_append(out, &pos, cap, cp);
            i += 2u;
        } else {
            utf8_append(out, &pos, cap, 0xFFFDu); // replacement char
            ++i;
        }
    }
    out[pos] = '\0';
}

// Decode 8-bit tag text of unknown real encoding. Old Chinese rips store GBK
// while the tag claims ISO-8859-1 (or none at all, ID3v1), and some taggers
// write UTF-8 regardless of the encoding byte. Order: ASCII/UTF-8 pass
// through, valid GBK converts, anything else falls back to Latin-1.
static void eightbit_text_to_utf8(const uint8_t *data, const size_t bytes,
                                  char *out, const size_t cap)
{
    if (is_valid_utf8(data, bytes)) {
        size_t copy = 0;
        while (copy < bytes && data[copy] != 0u && copy < cap - 1u) {
            out[copy] = static_cast<char>(data[copy]);
            ++copy;
        }
        out[copy] = '\0';
        return;
    }
    if (is_valid_gbk(data, bytes)) {
        gbk_to_utf8(data, bytes, out, cap);
        return;
    }
    latin1_to_utf8(data, bytes, out, cap);
}

// Decode an ID3v2 text frame payload (leading encoding byte) into UTF-8.
static void id3_text_to_utf8(const uint8_t *payload, const size_t bytes, char *out, const size_t cap)
{
    out[0] = '\0';
    if (bytes < 2u) {
        return;
    }
    const uint8_t enc = payload[0];
    const uint8_t *text = payload + 1u;
    size_t text_len = bytes - 1u;
    switch (enc) {
        case 0u: // ISO-8859-1 on paper; legacy Chinese rips put GBK here
            eightbit_text_to_utf8(text, text_len, out, cap);
            break;
        case 1u: { // UTF-16 with BOM
            bool be = false;
            if (text_len >= 2u && text[0] == 0xFEu && text[1] == 0xFFu) {
                be = true;
                text += 2u;
                text_len -= 2u;
            } else if (text_len >= 2u && text[0] == 0xFFu && text[1] == 0xFEu) {
                text += 2u;
                text_len -= 2u;
            }
            utf16_to_utf8(text, text_len, be, out, cap);
            break;
        }
        case 2u: // UTF-16BE without BOM
            utf16_to_utf8(text, text_len, true, out, cap);
            break;
        case 3u: // UTF-8 on paper; validate anyway (some taggers lie)
            eightbit_text_to_utf8(text, text_len, out, cap);
            break;
        default:
            break;
    }
    // Strip trailing whitespace/newlines some taggers leave behind.
    size_t len = strlen(out);
    while (len > 0u && (out[len - 1u] == ' ' || out[len - 1u] == '\r' || out[len - 1u] == '\n')) {
        out[--len] = '\0';
    }
}

static MediaCoverType_t cover_type_from_mime(const char *mime)
{
    if (strstr(mime, "png") != nullptr || strstr(mime, "PNG") != nullptr) {
        return MEDIA_COVER_PNG;
    }
    // "image/jpeg", "image/jpg", bare "JPG", or the legacy "image/" default.
    return MEDIA_COVER_JPEG;
}

// ---- ID3v2 (MP3) -----------------------------------------------------------

static bool parse_id3v2(FILE *f, MediaTrackInfo *info, const bool want_cover)
{
    uint8_t header[10];
    if (fseek(f, 0, SEEK_SET) != 0 || !read_exact(f, header, sizeof(header)) ||
        memcmp(header, "ID3", 3) != 0) {
        return false;
    }
    const uint8_t version = header[3]; // 3 = v2.3, 4 = v2.4
    const uint8_t flags = header[5];
    const uint32_t tag_size = syncsafe32(header + 6);
    if (version < 3u || (flags & 0x80u) != 0u) { // v2.2 or unsynchronised: skip
        return false;
    }
    long pos = 10;
    const long tag_end = 10 + static_cast<long>(tag_size);

    if ((flags & 0x40u) != 0u) { // extended header
        uint8_t ext[4];
        if (!read_exact(f, ext, sizeof(ext))) {
            return false;
        }
        const uint32_t ext_size = (version == 4u) ? syncsafe32(ext) : be32(ext);
        pos += static_cast<long>((version == 4u) ? ext_size : ext_size + 4u);
        if (fseek(f, pos, SEEK_SET) != 0) {
            return false;
        }
    }

    bool any = false;
    while (pos + 10 <= tag_end) {
        uint8_t fh[10];
        if (fseek(f, pos, SEEK_SET) != 0 || !read_exact(f, fh, sizeof(fh))) {
            break;
        }
        if (fh[0] == 0u) { // padding reached
            break;
        }
        const uint32_t frame_size = (version == 4u) ? syncsafe32(fh + 4) : be32(fh + 4);
        if (frame_size == 0u || pos + 10 + static_cast<long>(frame_size) > tag_end) {
            break;
        }

        char *text_dst = nullptr;
        size_t text_cap = 0;
        if (memcmp(fh, "TIT2", 4) == 0) {
            text_dst = info->title;
            text_cap = sizeof(info->title);
        } else if (memcmp(fh, "TPE1", 4) == 0) {
            text_dst = info->artist;
            text_cap = sizeof(info->artist);
        } else if (memcmp(fh, "TALB", 4) == 0) {
            text_dst = info->album;
            text_cap = sizeof(info->album);
        }

        if (text_dst != nullptr && frame_size <= kMaxTextFrameBytes) {
            uint8_t buf[kMaxTextFrameBytes];
            if (read_exact(f, buf, frame_size)) {
                id3_text_to_utf8(buf, frame_size, text_dst, text_cap);
                any = any || text_dst[0] != '\0';
            }
        } else if (want_cover && info->cover_data == nullptr && memcmp(fh, "APIC", 4) == 0 &&
                   frame_size <= kMaxCoverBytes) {
            // APIC: enc(1) mime\0 pic_type(1) desc\0(enc-dependent) data
            uint8_t *frame = static_cast<uint8_t *>(heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM));
            if (frame != nullptr && read_exact(f, frame, frame_size)) {
                const uint8_t enc = frame[0];
                size_t off = 1u;
                const char *mime = reinterpret_cast<const char *>(frame + off);
                while (off < frame_size && frame[off] != 0u) {
                    ++off;
                }
                ++off;           // mime NUL
                ++off;           // picture type
                if (enc == 1u || enc == 2u) { // UTF-16 description: NUL is 2 bytes
                    while (off + 1u < frame_size && (frame[off] != 0u || frame[off + 1u] != 0u)) {
                        off += 2u;
                    }
                    off += 2u;
                } else {
                    while (off < frame_size && frame[off] != 0u) {
                        ++off;
                    }
                    ++off;
                }
                if (off < frame_size) {
                    const size_t img_size = frame_size - off;
                    uint8_t *img = static_cast<uint8_t *>(heap_caps_malloc(img_size, MALLOC_CAP_SPIRAM));
                    if (img != nullptr) {
                        memcpy(img, frame + off, img_size);
                        info->cover_data = img;
                        info->cover_size = img_size;
                        info->cover_type = cover_type_from_mime(mime);
                        any = true;
                    }
                }
            }
            if (frame != nullptr) {
                heap_caps_free(frame);
            }
        }

        pos += 10 + static_cast<long>(frame_size);
    }
    return any;
}

// ---- ID3v1 fallback (often GBK on old Chinese rips; copied as-is) ----------

static bool parse_id3v1(FILE *f, MediaTrackInfo *info)
{
    if (fseek(f, -128, SEEK_END) != 0) {
        return false;
    }
    uint8_t tag[128];
    if (!read_exact(f, tag, sizeof(tag)) || memcmp(tag, "TAG", 3) != 0) {
        return false;
    }
    // ID3v1 has no encoding marker; old Chinese rips are GBK, others ASCII.
    if (info->title[0] == '\0') {
        eightbit_text_to_utf8(tag + 3, 30, info->title, sizeof(info->title));
    }
    if (info->artist[0] == '\0') {
        eightbit_text_to_utf8(tag + 33, 30, info->artist, sizeof(info->artist));
    }
    if (info->album[0] == '\0') {
        eightbit_text_to_utf8(tag + 63, 30, info->album, sizeof(info->album));
    }
    return info->title[0] != '\0' || info->artist[0] != '\0' || info->album[0] != '\0';
}

// ---- FLAC (Vorbis Comment / PICTURE / STREAMINFO) ---------------------------

static void vorbis_comment_assign(MediaTrackInfo *info, const char *kv, const size_t len)
{
    struct Key {
        const char *name;
        size_t name_len;
        char *dst;
        size_t cap;
    };
    const Key keys[] = {
        {"TITLE=", 6, info->title, sizeof(info->title)},
        {"ARTIST=", 7, info->artist, sizeof(info->artist)},
        {"ALBUM=", 6, info->album, sizeof(info->album)},
    };
    for (const Key &k : keys) {
        if (len > k.name_len && strncasecmp(kv, k.name, k.name_len) == 0) {
            const size_t vlen = len - k.name_len;
            const size_t copy = (vlen < k.cap - 1u) ? vlen : k.cap - 1u;
            memcpy(k.dst, kv + k.name_len, copy); // values are UTF-8 by spec
            k.dst[copy] = '\0';
            return;
        }
    }
}

static bool parse_flac(FILE *f, MediaTrackInfo *info, const bool want_cover)
{
    uint8_t magic[4];
    if (fseek(f, 0, SEEK_SET) != 0 || !read_exact(f, magic, sizeof(magic)) ||
        memcmp(magic, "fLaC", 4) != 0) {
        return false;
    }

    bool any = false;
    bool last = false;
    while (!last) {
        uint8_t bh[4];
        if (!read_exact(f, bh, sizeof(bh))) {
            break;
        }
        last = (bh[0] & 0x80u) != 0u;
        const uint8_t type = bh[0] & 0x7Fu;
        const uint32_t block_len = (static_cast<uint32_t>(bh[1]) << 16) |
                                   (static_cast<uint32_t>(bh[2]) << 8) | bh[3];
        const long next = ftell(f) + static_cast<long>(block_len);

        if (type == 0u && block_len >= 18u) { // STREAMINFO -> duration
            uint8_t si[18];
            if (read_exact(f, si, sizeof(si))) {
                const uint32_t sample_rate = (static_cast<uint32_t>(si[10]) << 12) |
                                             (static_cast<uint32_t>(si[11]) << 4) |
                                             (si[12] >> 4);
                const uint64_t total_samples = ((static_cast<uint64_t>(si[13]) & 0x0Fu) << 32) |
                                               (static_cast<uint64_t>(si[14]) << 24) |
                                               (static_cast<uint64_t>(si[15]) << 16) |
                                               (static_cast<uint64_t>(si[16]) << 8) |
                                               si[17];
                if (sample_rate > 0u && total_samples > 0u) {
                    info->duration_ms = static_cast<uint32_t>((total_samples * 1000ULL) / sample_rate);
                    any = true;
                }
            }
        } else if (type == 4u) { // VORBIS_COMMENT
            uint8_t lenbuf[4];
            if (read_exact(f, lenbuf, 4)) {
                const uint32_t vendor_len = le32(lenbuf);
                if (fseek(f, static_cast<long>(vendor_len), SEEK_CUR) == 0 &&
                    read_exact(f, lenbuf, 4)) {
                    uint32_t count = le32(lenbuf);
                    while (count-- > 0u) {
                        if (!read_exact(f, lenbuf, 4)) {
                            break;
                        }
                        const uint32_t kv_len = le32(lenbuf);
                        if (kv_len > 0u && kv_len <= kMaxTextFrameBytes) {
                            char kv[kMaxTextFrameBytes + 1];
                            if (!read_exact(f, kv, kv_len)) {
                                break;
                            }
                            kv[kv_len] = '\0';
                            vorbis_comment_assign(info, kv, kv_len);
                            any = true;
                        } else if (fseek(f, static_cast<long>(kv_len), SEEK_CUR) != 0) {
                            break;
                        }
                    }
                }
            }
        } else if (type == 6u && want_cover && info->cover_data == nullptr) { // PICTURE
            uint8_t buf[4];
            char mime[64] = {};
            if (read_exact(f, buf, 4)) {           // picture type (ignored)
                if (read_exact(f, buf, 4)) {       // mime length
                    const uint32_t mime_len = be32(buf);
                    const size_t copy = (mime_len < sizeof(mime) - 1u) ? mime_len : sizeof(mime) - 1u;
                    if (read_exact(f, mime, copy)) {
                        if (mime_len > copy) {
                            (void)fseek(f, static_cast<long>(mime_len - copy), SEEK_CUR);
                        }
                        if (read_exact(f, buf, 4)) { // description length
                            const uint32_t desc_len = be32(buf);
                            // skip description + width/height/depth/colors
                            if (fseek(f, static_cast<long>(desc_len) + 16, SEEK_CUR) == 0 &&
                                read_exact(f, buf, 4)) {
                                const uint32_t img_len = be32(buf);
                                if (img_len > 0u && img_len <= kMaxCoverBytes) {
                                    uint8_t *img = static_cast<uint8_t *>(
                                        heap_caps_malloc(img_len, MALLOC_CAP_SPIRAM));
                                    if (img != nullptr && read_exact(f, img, img_len)) {
                                        info->cover_data = img;
                                        info->cover_size = img_len;
                                        info->cover_type = cover_type_from_mime(mime);
                                        any = true;
                                    } else if (img != nullptr) {
                                        heap_caps_free(img);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (fseek(f, next, SEEK_SET) != 0) {
            break;
        }
    }
    return any;
}

} // namespace

extern "C" bool MEDIA_META_Read(const char *path, MediaTrackInfo *out_info, const bool want_cover)
{
    if (path == nullptr || out_info == nullptr) {
        return false;
    }
    memset(out_info, 0, sizeof(*out_info));

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }

    bool any = parse_flac(f, out_info, want_cover);
    if (!any) {
        any = parse_id3v2(f, out_info, want_cover);
        // v1 fills whatever v2 left empty.
        any = parse_id3v1(f, out_info) || any;
    }
    fclose(f);

    if (any) {
        ESP_LOGI(TAG, "%s: title='%s' artist='%s' album='%s' cover=%u bytes",
                 path, out_info->title, out_info->artist, out_info->album,
                 static_cast<unsigned>(out_info->cover_size));
    }
    return any;
}

extern "C" void MEDIA_META_Free(MediaTrackInfo *info)
{
    if (info == nullptr) {
        return;
    }
    if (info->cover_data != nullptr) {
        heap_caps_free(info->cover_data);
        info->cover_data = nullptr;
    }
    info->cover_size = 0;
    info->cover_type = MEDIA_COVER_NONE;
}

extern "C" void MEDIA_META_ReleaseCover(MediaTrackInfo *info)
{
    // Same cover cleanup as MEDIA_META_Free, without touching anything else.
    if (info == nullptr) {
        return;
    }
    if (info->cover_data != nullptr) {
        heap_caps_free(info->cover_data);
        info->cover_data = nullptr;
    }
    info->cover_size = 0;
    info->cover_type = MEDIA_COVER_NONE;
}

extern "C" void MEDIA_TEXT_8BitToUtf8(const uint8_t *data, const size_t bytes,
                                        char *out, const size_t capacity)
{
    if (out == nullptr || capacity == 0u) return;
    out[0] = '\0';
    if (data == nullptr || bytes == 0u) return;
    eightbit_text_to_utf8(data, bytes, out, capacity);
}
