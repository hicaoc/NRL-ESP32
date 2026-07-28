#include "media/media_decoder.h"

#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "lib/nrl_psram.h"
#include "services/smb_stream.h"
#include "services/smb_vfs.h"

static const char *TAG = "MDEC";

namespace {

// 32 KB: the TS demux (HLS) buffers considerably more input than the plain
// frame parsers before reporting progress; at 8 KB it stalled with a full
// buffer and playback ended after the first fraction of a second.
constexpr size_t kRawBufferBytes = 32 * 1024;
// The elementary FLAC parser is happier when fed like Espressif's simple
// decoder tests: small raw chunks, then keep passing the unconsumed window.
// Larger 32 KB chunks are still needed by TS/HLS demux.
constexpr size_t kFlacRawChunkBytes = 512;
// FLAC blocks can decode to ~64 KB of PCM at hi-res; start smaller and grow
// on ESP_AUDIO_ERR_BUFF_NOT_ENOUGH as the decoder reports its needed size.
constexpr size_t kOutputBufferBytes = 128 * 1024;

// Read-ahead ring for file sources (PSRAM). SMB reads are synchronous
// round-trips (~1.2 KB per request, see smb_vfs.cpp), so network jitter fed
// straight into the decode loop is audible as stutter. A filler task keeps
// this ring topped up; the decoder then reads from memory and rides out
// latency spikes. Reserve it at link time so display/cover/codec allocations
// cannot fragment the large contiguous block before playback starts:
// 2 MB ~= 50 s of 320 kbps MP3, or ~2-3 s of lossless FLAC -- enough to ride
// out SMB throughput dips (worsened here by the trimmed Wi-Fi buffers that free
// internal RAM for Bluetooth). read_source() decodes as soon as the first bytes
// land, so a larger ring adds no start-up delay -- the filler just keeps more
// ahead. The refill chunk is large so a drained ring recovers in fewer filler
// iterations (each source read is still split to ~1.2 KB inside smb_vfs).
constexpr size_t kRingBytes = 2 * 1024 * 1024;
// The dedicated SMB stream fills this request with a single 64 KB pread (see
// kWireReadBytes in smb_stream.cpp); local USB/TF files perform one normal
// sequential VFS read. Both paths commit into the same PSRAM ring without
// changing their decoders.
constexpr size_t kRingFillChunk = 64 * 1024;
// Direct HTTP radio is paced by the remote server. Accumulate one compressed
// input chunk before decoding so short Wi-Fi/TCP stalls do not immediately
// drain the tiny high-rate I2S DMA queue into audible gaps/noise. At 64 kbps
// this is about four seconds; at 320 kbps it is under one second.
constexpr size_t kHttpPrebufferBytes = 64 * 1024;
// Build enough SMB reserve before enabling I2S so a two-second reconnect can
// be hidden even for lossless audio. The filler continues toward the full
// 2 MB ring after playback begins.
constexpr size_t kSmbPrebufferBytes = 256 * 1024;
constexpr size_t kMinimumPrebufferBytes = 16 * 1024;
constexpr int64_t kHttpPrebufferMaxWaitUs = 3000000LL;
constexpr int64_t kSmbPrebufferMaxWaitUs = 8000000LL;
constexpr size_t kSmbRawChunkBytes = 32 * 1024;

constexpr size_t kHlsMaxSegments = 8;
constexpr size_t kHlsPlaylistBytes = 16 * 1024;

static bool s_default_registered = false;
NRL_PSRAM_BSS uint8_t s_media_ring[kRingBytes];
NRL_PSRAM_BSS uint8_t s_raw_buffer[kRawBufferBytes];
NRL_PSRAM_BSS uint8_t s_out_buffer[kOutputBufferBytes];
NRL_PSRAM_BSS char s_hls_playlist_buffer[kHlsPlaylistBytes];
MediaDecoder *s_ring_owner = nullptr;
TaskHandle_t s_ring_worker = nullptr;
bool s_media_buffers_in_use = false;

} // namespace

// HLS (m3u8) source: a media playlist of sequential segment URLs, reloaded
// periodically for live streams. Segments are MPEG-TS or raw AAC/MP3; the
// simple decoder handles all three, so this layer only concatenates them
// into one byte stream.
struct HlsStream {
    char playlist_url[512];
    esp_http_client_handle_t seg_http;  // current segment connection
    char (*seg_urls)[512];              // queued segment URLs (kHlsMaxSegments)
    size_t seg_count;
    size_t seg_next;
    uint64_t next_media_seq;            // first sequence not yet queued
    uint32_t target_duration_s;
    int64_t last_fetch_us;
    size_t seg_bytes;                   // bytes read from the current segment
    bool live;                          // no #EXT-X-ENDLIST seen
    bool insecure;                      // TLS fallback engaged for this host
    volatile bool *stop;                // owner's teardown flag
};

NRL_PSRAM_BSS static char s_hls_segment_urls[kHlsMaxSegments][512];
static bool s_hls_urls_in_use = false;

struct MediaDecoder {
    FILE *file;                      // local file source, or...
    SmbStream *smb;                  // dedicated SMB media connection, or...
    esp_http_client_handle_t http;   // ...HTTP(S) stream source (net radio)...
    HlsStream *hls;                  // ...or HLS (m3u8) segment stream
    esp_audio_simple_dec_handle_t handle;
    esp_audio_simple_dec_type_t type;
    uint8_t *raw_buffer;
    uint8_t *out_buffer;
    size_t out_capacity;
    size_t raw_fill;      // valid bytes in raw_buffer
    size_t raw_offset;    // consumed bytes in raw_buffer
    bool eof;
    bool info_valid;
    MediaDecoderInfo info;
    // Sniffed leading bytes for format detection on non-seekable HTTP
    // streams; replayed ahead of further source reads.
    uint8_t sniff[16];
    size_t sniff_len;
    size_t sniff_off;
    // Synthetic leading bytes for file sources. Used by FLAC to preserve
    // STREAMINFO while skipping large metadata/cover blocks before audio.
    uint8_t prefix[42];
    size_t prefix_len;
    size_t prefix_off;
    // Read-ahead ring (file + HLS sources): a filler task streams the source
    // into PSRAM so SMB/segment latency spikes don't starve the decode loop.
    uint8_t *ring;
    volatile size_t ring_head;   // filler writes
    volatile size_t ring_tail;   // consumer writes
    volatile bool ring_eof;
    volatile bool ring_stop;
    volatile bool ring_running;
    TaskHandle_t ring_task;
    bool source_is_smb;
    volatile bool source_error;
    const volatile bool *external_stop;
    bool ring_wait_full;         // block for a full read (file semantics)
    size_t ring_prebuffer_bytes; // compressed bytes required before playback starts
    // Underrun telemetry: the ring ran dry while the decoder still needed
    // bytes (audible gap on I2S). Counted per event, logged at 1 Hz at most,
    // summarized on close.
    unsigned underrun_events;
    unsigned underrun_total_ms;
    int64_t underrun_start_us;
    int64_t underrun_last_log_us;
};

namespace {

static size_t source_read_direct(MediaDecoder *d, uint8_t *dst, size_t size);

static bool decoder_stop_requested(const MediaDecoder *d)
{
    return d->ring_stop ||
           (d->external_stop != nullptr && *d->external_stop);
}

// Read for the decode loop: from the read-ahead ring when one is running,
// otherwise straight from the source (sniffed bytes replay first either way).
static size_t read_source(MediaDecoder *d, uint8_t *dst, const size_t size)
{
    if (d->ring == nullptr) {
        return source_read_direct(d, dst, size);
    }
    if (d->ring_prebuffer_bytes > 0u) {
        // The filler owns all source reads while the decoder waits, allowing
        // a useful reserve to form before I2S playback begins. NAS FLAC needs
        // a deeper gate than compressed radio because SMB is request/response
        // and short Wi-Fi stalls otherwise drain the I2S path immediately.
        const int64_t wait_started_us = esp_timer_get_time();
        while (!d->ring_eof && d->ring_running && !decoder_stop_requested(d)) {
            const size_t head = d->ring_head;
            const size_t tail = d->ring_tail;
            const size_t used = (head + kRingBytes - tail) % kRingBytes;
            const bool target_reached = used >= d->ring_prebuffer_bytes;
            const int64_t max_wait_us = d->source_is_smb
                                            ? kSmbPrebufferMaxWaitUs
                                            : kHttpPrebufferMaxWaitUs;
            const bool wait_expired = used >= kMinimumPrebufferBytes &&
                                      esp_timer_get_time() - wait_started_us >=
                                          max_wait_us;
            if (target_reached || wait_expired) {
                ESP_LOGI(TAG, "readahead: %s buffered %u bytes%s",
                         d->source_is_smb ? "SMB" : "HTTP radio",
                         static_cast<unsigned>(used),
                         target_reached ? "" : " (startup timeout)");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        d->ring_prebuffer_bytes = 0u;
    }
    size_t got = 0;
    while (got < size) {
        if (decoder_stop_requested(d)) {
            break;
        }
        const size_t head = d->ring_head;
        const size_t tail = d->ring_tail;
        const size_t used = (head + kRingBytes - tail) % kRingBytes;
        if (used == 0u) {
            if (d->ring_eof || !d->ring_running) {
                break; // true end of stream
            }
            if (!d->ring_wait_full && got > 0u) {
                break; // live stream: hand over what we have
            }
            if (d->underrun_start_us == 0) {
                d->underrun_start_us = esp_timer_get_time();
                ++d->underrun_events;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (d->underrun_start_us != 0) {
            // Recovered from starvation: account the gap, then log at 1 Hz at
            // most (a drip-fed ring would otherwise spam one line per poll).
            const int64_t now_us = esp_timer_get_time();
            d->underrun_total_ms +=
                static_cast<unsigned>((now_us - d->underrun_start_us) / 1000LL);
            d->underrun_start_us = 0;
            if (now_us - d->underrun_last_log_us >= 1000000LL) {
                d->underrun_last_log_us = now_us;
                ESP_LOGW(TAG, "underrun: ring dry (event #%u, total %u ms)",
                         d->underrun_events, d->underrun_total_ms);
            }
        }
        size_t chunk = kRingBytes - tail; // contiguous run up to the wrap
        if (chunk > used) {
            chunk = used;
        }
        if (chunk > size - got) {
            chunk = size - got;
        }
        memcpy(dst + got, d->ring + tail, chunk);
        d->ring_tail = (tail + chunk) % kRingBytes;
        got += chunk;
    }
    return got;
}

static esp_audio_simple_dec_type_t detect_type_from_header(const uint8_t *header,
                                                           const size_t got,
                                                           const char *path)
{
    if (got >= 12u && memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (got >= 4u && memcmp(header, "fLaC", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (got >= 8u && memcmp(header + 4, "ftyp", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    if (got >= 3u && memcmp(header, "ID3", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (got >= 2u && header[0] == 0xFFu && (header[1] & 0xF6u) == 0xF0u) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC; // ADTS sync
    }
    if (got >= 2u && header[0] == 0xFFu && (header[1] & 0xE0u) == 0xE0u) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3; // bare MPEG frame sync
    }
    if (got >= 1u && header[0] == 0x47u) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_TS; // MPEG-TS sync byte (HLS segments)
    }

    // Fall back to the extension / URL suffix (e.g. MP3s that open with
    // junk bytes, or typeless radio streams).
    const char *dot = strrchr(path, '.');
    if (dot != nullptr) {
        if (strcasecmp(dot, ".mp3") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }
        if (strcasecmp(dot, ".flac") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        }
        if (strcasecmp(dot, ".wav") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        }
        if (strcasecmp(dot, ".m4a") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
        }
        if (strcasecmp(dot, ".aac") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        }
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

struct FlacStreamInfo {
    uint32_t sample_rate_hz;
    uint8_t bits_per_sample;
    uint8_t channels;
    uint8_t streaminfo[34];
    long audio_offset;
};

static bool read_file_exact(FILE *f, void *dst, const size_t n)
{
    return fread(dst, 1, n, f) == n;
}

static bool flac_read_stream_info(FILE *f, FlacStreamInfo *info)
{
    if (f == nullptr || info == nullptr) {
        return false;
    }
    const long saved = ftell(f);
    uint8_t magic[4];
    uint8_t bh[4];
    bool ok = false;

    if (saved >= 0 &&
        fseek(f, 0, SEEK_SET) == 0 &&
        read_file_exact(f, magic, sizeof(magic)) &&
        memcmp(magic, "fLaC", 4) == 0) {
        bool last = false;
        bool got_streaminfo = false;
        while (!last && read_file_exact(f, bh, sizeof(bh))) {
            last = (bh[0] & 0x80u) != 0u;
            const uint8_t type = bh[0] & 0x7Fu;
            const uint32_t block_len = (static_cast<uint32_t>(bh[1]) << 16) |
                                       (static_cast<uint32_t>(bh[2]) << 8) | bh[3];
            if (block_len > 16u * 1024u * 1024u) {
                last = false;
                break;
            }
            if (type == 0u && block_len >= sizeof(info->streaminfo)) {
                if (!read_file_exact(f, info->streaminfo, sizeof(info->streaminfo))) {
                    break;
                }
                if (block_len > sizeof(info->streaminfo) &&
                    fseek(f, static_cast<long>(block_len - sizeof(info->streaminfo)), SEEK_CUR) != 0) {
                    break;
                }
                got_streaminfo = true;
            } else if (fseek(f, static_cast<long>(block_len), SEEK_CUR) != 0) {
                break;
            }
        }
        const long audio_offset = ftell(f);
        if (last && got_streaminfo && audio_offset > 0) {
            const uint8_t *si = info->streaminfo;
            info->sample_rate_hz = (static_cast<uint32_t>(si[10]) << 12) |
                                   (static_cast<uint32_t>(si[11]) << 4) |
                                   (si[12] >> 4);
            info->channels = static_cast<uint8_t>(((si[12] >> 1) & 0x07u) + 1u);
            info->bits_per_sample = static_cast<uint8_t>(
                (((si[12] & 0x01u) << 4) | (si[13] >> 4)) + 1u);
            info->audio_offset = audio_offset;
            ok = info->sample_rate_hz > 0u && info->channels > 0u && info->bits_per_sample > 0u;
        }
    }

    if (saved >= 0) {
        (void)fseek(f, saved, SEEK_SET);
    }
    return ok;
}

static void flac_install_compact_prefix(MediaDecoder *d, const FlacStreamInfo *info)
{
    if (d == nullptr || info == nullptr || info->audio_offset <= 0) {
        return;
    }
    memcpy(d->prefix, "fLaC", 4);
    d->prefix[4] = 0x80u; // last metadata block, STREAMINFO
    d->prefix[5] = 0;
    d->prefix[6] = 0;
    d->prefix[7] = sizeof(info->streaminfo);
    memcpy(d->prefix + 8, info->streaminfo, sizeof(info->streaminfo));
    d->prefix_len = 8u + sizeof(info->streaminfo);
    d->prefix_off = 0;
    (void)fseek(d->file, info->audio_offset, SEEK_SET);
}

static bool grow_out_buffer(MediaDecoder *d, const size_t needed)
{
    return d != nullptr && needed <= d->out_capacity;
}

// Open an HTTP(S) stream, verifying TLS against the cert bundle first; on
// handshake failure retry once without verification. Net-radio CDNs commonly
// serve incomplete cert chains (missing intermediate) that mbedtls cannot
// repair -- these are public audio streams, so availability wins over
// authenticity for the retry. Returns an OPENED client or nullptr.
// `insecure_hint` (optional) remembers a stream that already needed the
// fallback, so follow-up connections (HLS segments every ~10 s) skip the
// doomed verified attempt instead of re-failing a handshake each time.
static esp_http_client_handle_t http_open_stream(const char *url, const int timeout_ms,
                                                 const int buffer_size,
                                                 bool *insecure_hint = nullptr)
{
    const int first = (insecure_hint != nullptr && *insecure_hint) ? 1 : 0;
    for (int insecure = first; insecure < 2; ++insecure) {
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.timeout_ms = timeout_ms;
        cfg.buffer_size = buffer_size;
        if (insecure == 0) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        if (http == nullptr) {
            return nullptr;
        }
        if (esp_http_client_open(http, 0) == ESP_OK) {
            if (insecure != 0) {
                if (insecure_hint == nullptr || !*insecure_hint) {
                    ESP_LOGW(TAG, "TLS verification skipped for %s", url);
                }
                if (insecure_hint != nullptr) {
                    *insecure_hint = true;
                }
            }
            return http;
        }
        esp_http_client_cleanup(http);
        if (strncmp(url, "https://", 8) != 0) {
            return nullptr; // plain http can't be a TLS failure; don't retry
        }
    }
    return nullptr;
}

// ---- HLS (m3u8) source ------------------------------------------------------

// Resolve `ref` against `base` (RFC-lite: absolute, host-relative, relative).
static void hls_resolve_url(const char *base, const char *ref, char *out, const size_t cap)
{
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, cap, "%s", ref);
        return;
    }
    if (ref[0] == '/') {
        // scheme://host[:port] + ref
        const char *scheme_end = strstr(base, "://");
        const char *host_end = (scheme_end != nullptr) ? strchr(scheme_end + 3, '/') : nullptr;
        const int host_len = (host_end != nullptr) ? static_cast<int>(host_end - base)
                                                   : static_cast<int>(strlen(base));
        snprintf(out, cap, "%.*s%s", host_len, base, ref);
        return;
    }
    // Relative to the playlist's directory (strip any query string first).
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", base);
    char *query = strchr(dir, '?');
    if (query != nullptr) {
        *query = '\0';
    }
    char *slash = strrchr(dir, '/');
    if (slash != nullptr && slash - dir > 7) { // don't chop the "https://"
        slash[1] = '\0';
    }
    snprintf(out, cap, "%s%s", dir, ref);
}

// GET a small text resource (the playlist). Returns bytes read, 0 on error.
static size_t hls_fetch_text(const char *url, char *out, const size_t cap, bool *insecure_hint)
{
    esp_http_client_handle_t http = http_open_stream(url, 8000, 2048, insecure_hint);
    if (http == nullptr) {
        ESP_LOGE(TAG, "hls: playlist open failed: %s", url);
        out[0] = '\0';
        return 0;
    }
    size_t got = 0;
    (void)esp_http_client_fetch_headers(http);
    const int status = esp_http_client_get_status_code(http);
    if (status == 200) {
        while (got + 1u < cap) {
            const int n = esp_http_client_read(http, out + got,
                                               static_cast<int>(cap - 1u - got));
            if (n <= 0) {
                break;
            }
            got += static_cast<size_t>(n);
        }
    } else {
        ESP_LOGE(TAG, "hls: playlist HTTP %d: %s", status, url);
    }
    (void)esp_http_client_close(http);
    (void)esp_http_client_cleanup(http);
    out[got] = '\0';
    return got;
}

// Fetch + parse the playlist; follows one master-playlist indirection (first
// variant). Queues segments with sequence >= next_media_seq.
static bool hls_load_playlist(HlsStream *h)
{
    char *text = s_hls_playlist_buffer;
    bool ok = false;
    for (int depth = 0; depth < 3; ++depth) {
        if (hls_fetch_text(h->playlist_url, text, kHlsPlaylistBytes, &h->insecure) == 0u) {
            break;
        }
        const bool master = strstr(text, "#EXT-X-STREAM-INF") != nullptr;
        uint64_t first_seq = 0;
        size_t listed = 0;
        bool endlist = false;
        bool got_variant = false;
        h->seg_count = 0;
        h->seg_next = 0;

        char *save = nullptr;
        bool after_stream_inf = false;
        for (char *line = strtok_r(text, "\n", &save); line != nullptr;
             line = strtok_r(nullptr, "\n", &save)) {
            const size_t len = strlen(line);
            if (len > 0u && line[len - 1u] == '\r') {
                line[len - 1u] = '\0';
            }
            if (line[0] == '\0') {
                continue;
            }
            if (master) {
                if (strncmp(line, "#EXT-X-STREAM-INF", 17) == 0) {
                    after_stream_inf = true;
                } else if (after_stream_inf && line[0] != '#') {
                    // First variant: adopt it as the media playlist and refetch.
                    char resolved[512];
                    hls_resolve_url(h->playlist_url, line, resolved, sizeof(resolved));
                    snprintf(h->playlist_url, sizeof(h->playlist_url), "%s", resolved);
                    got_variant = true;
                    break;
                }
                continue;
            }
            if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
                first_seq = strtoull(line + 22, nullptr, 10);
            } else if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
                const unsigned long v = strtoul(line + 22, nullptr, 10);
                if (v > 0ul && v <= 60ul) {
                    h->target_duration_s = static_cast<uint32_t>(v);
                }
            } else if (strcmp(line, "#EXT-X-ENDLIST") == 0) {
                endlist = true;
            } else if (line[0] != '#') {
                const uint64_t seq = first_seq + listed;
                ++listed;
                if (seq >= h->next_media_seq && h->seg_count < kHlsMaxSegments) {
                    hls_resolve_url(h->playlist_url, line,
                                    h->seg_urls[h->seg_count], sizeof(h->seg_urls[0]));
                    ++h->seg_count;
                }
            }
        }

        if (master) {
            if (!got_variant) {
                ESP_LOGE(TAG, "hls: master playlist without variants");
                break;
            }
            continue; // refetch the variant playlist
        }
        h->next_media_seq = first_seq + listed;
        h->live = !endlist;
        h->last_fetch_us = esp_timer_get_time();
        ok = true;
        break;
    }
    return ok;
}

// Sequential byte stream across segments; refreshes the playlist on live
// streams. Returns 0 only at true end of stream (VOD done / stopped / error).
static size_t hls_read(HlsStream *h, uint8_t *dst, const size_t size)
{
    while (h->stop == nullptr || !*h->stop) {
        if (h->seg_http == nullptr) {
            if (h->seg_next >= h->seg_count) {
                if (!h->live) {
                    ESP_LOGI(TAG, "hls: vod played out");
                    return 0; // VOD played out
                }
                // Live: poll the playlist for new segments, pacing by the
                // advertised segment duration.
                const int64_t since_us = esp_timer_get_time() - h->last_fetch_us;
                const int64_t min_wait_us =
                    static_cast<int64_t>(h->target_duration_s) * 500000LL; // half target
                if (since_us < min_wait_us) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                if (!hls_load_playlist(h)) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                if (h->seg_count == 0u) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                continue;
            }
            const char *url = h->seg_urls[h->seg_next++];
            h->seg_http = http_open_stream(url, 8000, 4096, &h->insecure);
            if (h->seg_http == nullptr) {
                ESP_LOGW(TAG, "hls: segment open failed, skipping");
                continue;
            }
            (void)esp_http_client_fetch_headers(h->seg_http);
            const int status = esp_http_client_get_status_code(h->seg_http);
            if (status != 200) {
                ESP_LOGW(TAG, "hls: segment HTTP %d, skipping", status);
                (void)esp_http_client_close(h->seg_http);
                esp_http_client_cleanup(h->seg_http);
                h->seg_http = nullptr;
                continue;
            }
            h->seg_bytes = 0;
        }
        const int n = esp_http_client_read(h->seg_http, reinterpret_cast<char *>(dst),
                                           static_cast<int>(size));
        if (n > 0) {
            h->seg_bytes += static_cast<size_t>(n);
            return static_cast<size_t>(n);
        }
        // Segment finished (or errored): move on to the next one.
        ESP_LOGI(TAG, "hls: segment %u/%u done (%u bytes)",
                 static_cast<unsigned>(h->seg_next),
                 static_cast<unsigned>(h->seg_count),
                 static_cast<unsigned>(h->seg_bytes));
        (void)esp_http_client_close(h->seg_http);
        esp_http_client_cleanup(h->seg_http);
        h->seg_http = nullptr;
    }
    ESP_LOGI(TAG, "hls: stopped by owner");
    return 0;
}

static void hls_close(HlsStream *h)
{
    if (h == nullptr) {
        return;
    }
    if (h->seg_http != nullptr) {
        (void)esp_http_client_close(h->seg_http);
        esp_http_client_cleanup(h->seg_http);
    }
    if (h->seg_urls == s_hls_segment_urls) s_hls_urls_in_use = false;
    heap_caps_free(h);
}

// Open the m3u8 source and prime the first segment so Open() can sniff the
// container type. Returns nullptr on any failure.
static HlsStream *hls_open(const char *url, volatile bool *stop)
{
    if (s_hls_urls_in_use) return nullptr;
    HlsStream *h = static_cast<HlsStream *>(heap_caps_calloc(1, sizeof(HlsStream), MALLOC_CAP_SPIRAM));
    if (h == nullptr) {
        return nullptr;
    }
    s_hls_urls_in_use = true;
    h->seg_urls = s_hls_segment_urls;
    snprintf(h->playlist_url, sizeof(h->playlist_url), "%s", url);
    h->target_duration_s = 6;
    h->stop = stop;
    if (!hls_load_playlist(h) || h->seg_count == 0u) {
        ESP_LOGE(TAG, "hls: no playable segments: %s", url);
        hls_close(h);
        return nullptr;
    }
    ESP_LOGI(TAG, "hls: %s stream, %u segments queued, target %lus",
             h->live ? "live" : "vod",
             static_cast<unsigned>(h->seg_count),
             static_cast<unsigned long>(h->target_duration_s));
    return h;
}

// ---- read-ahead ring --------------------------------------------------------

// Raw read from whichever source backs the decoder (no ring), serving the
// sniffed bytes first. Runs in the filler task for ring-backed sources.
static size_t source_read_direct(MediaDecoder *d, uint8_t *dst, const size_t size)
{
    if (decoder_stop_requested(d)) {
        return 0u;
    }
    size_t total = 0;
    while (total < size && d->prefix_off < d->prefix_len) {
        dst[total++] = d->prefix[d->prefix_off++];
    }
    if (total == size) {
        return total;
    }
    while (total < size && d->sniff_off < d->sniff_len) {
        dst[total++] = d->sniff[d->sniff_off++];
    }
    if (total == size) {
        return total;
    }
    if (d->smb != nullptr) {
        const int got = SMB_STREAM_Read(d->smb, dst + total, size - total);
        if (got < 0) {
            d->source_error = true;
            return total;
        }
        return total + static_cast<size_t>(got);
    }
    if (d->file != nullptr) {
        const int64_t t0 = esp_timer_get_time();
        const size_t got = fread(dst + total, 1, size - total, d->file);
        const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        if (dt_ms > 2000) {
            ESP_LOGW(TAG, "slow source read: %u bytes in %lld ms",
                     static_cast<unsigned>(got), static_cast<long long>(dt_ms));
        }
        if (got == 0u && ferror(d->file)) {
            ESP_LOGW(TAG, "source read failed: errno=%d", errno);
            d->source_error = true;
        }
        return total + got;
    }
    if (d->hls != nullptr) {
        return total + hls_read(d->hls, dst + total, size - total);
    }
    if (d->http != nullptr) {
        const int got = esp_http_client_read(d->http, reinterpret_cast<char *>(dst) + total,
                                             static_cast<int>(size - total));
        return total + ((got > 0) ? static_cast<size_t>(got) : 0u);
    }
    return total;
}

static void ring_filler_task(void *arg)
{
    (void)arg;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        MediaDecoder *d = s_ring_owner;
        if (d == nullptr) continue;

        while (!decoder_stop_requested(d)) {
            const size_t head = d->ring_head;
            const size_t tail = d->ring_tail;
            const size_t used = (head + kRingBytes - tail) % kRingBytes;
            const size_t free_space = kRingBytes - 1u - used;
            if (free_space < kRingFillChunk) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            size_t want = kRingBytes - head;
            if (want > free_space) want = free_space;
            if (want > kRingFillChunk) want = kRingFillChunk;
            const size_t got = source_read_direct(d, d->ring + head, want);
            if (got == 0u) {
                if (!decoder_stop_requested(d)) {
                    ESP_LOGI(TAG, "readahead: source end of stream");
                }
                d->ring_eof = true;
                break;
            }
            d->ring_head = (head + got) % kRingBytes;
            vTaskDelay(1);
        }
        d->ring_running = false;
    }
}

// Start the read-ahead ring; on failure the decoder falls back to direct
// reads (correct, just stutter-prone again).
static void ring_start(MediaDecoder *d)
{
    if (s_ring_owner != nullptr) {
        ESP_LOGW(TAG, "read-ahead ring already in use; using direct reads");
        return;
    }
    s_ring_owner = d;
    d->ring = s_media_ring;
    d->ring_head = 0;
    d->ring_tail = 0;
    d->ring_eof = false;
    d->ring_stop = false;
    d->ring_running = true;
    d->ring_wait_full = (d->file != nullptr || d->smb != nullptr ||
                         d->http != nullptr);
    d->ring_prebuffer_bytes = (d->http != nullptr)
                                  ? kHttpPrebufferBytes
                                  : (d->source_is_smb ? kSmbPrebufferBytes : 0u);
    // PSRAM stack: this task only does source reads (SMB over the network / SD via
    // SDMMC / HTTP) -- none of which touch the internal SPI flash, and flash
    // auto-suspend keeps the cache alive anyway -- so its stack is safe in PSRAM.
    // Keeping it out of internal RAM lets it create even while Bluetooth holds its
    // 40 KB internal reserve; the old 8 KB internal stack failed to allocate with
    // BT on, dropping SMB playback back to unbuffered direct reads (stutter). Only
    // a ~700 B TCB stays internal. Core 0 owns lwIP/Wi-Fi and was saturated by
    // their work plus SMB protocol processing; Core 1 still had >80% idle while
    // decoding FLAC. Run the filler on Core 1 so socket callbacks stay on Core 0
    // but libsmb2 polling/parsing and PSRAM writes use the spare core.
    if (s_ring_worker == nullptr) {
        if (xTaskCreatePinnedToCoreWithCaps(ring_filler_task, "mdec_fill", 8192,
                                            nullptr, 5, &s_ring_worker, 1,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGW(TAG, "read-ahead task create failed; using direct reads");
            d->ring_running = false;
            d->ring = nullptr;
            s_ring_owner = nullptr;
            return;
        }
    }
    d->ring_task = s_ring_worker;
    xTaskNotifyGive(s_ring_worker);
}

static void ring_stop_and_free(MediaDecoder *d)
{
    if (d->ring == nullptr) {
        return;
    }
    d->ring_stop = true;
    // The filler can sit inside an SMB/HTTP read for several seconds; give
    // it time to notice the flag. If it never exits, leak rather than crash.
    for (int i = 0; i < 1200 && d->ring_running; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (d->ring_running) {
        ESP_LOGE(TAG, "read-ahead task stuck; leaking decoder resources");
        return;
    }
    // Keep this PSRAM-backed worker and its TCB alive between tracks. Deleting
    // it from the other core while it entered vTaskSuspend() could corrupt the
    // SMP ready list during rapid stop/track changes.
    d->ring_task = nullptr;
    d->ring = nullptr;
    if (s_ring_owner == d) s_ring_owner = nullptr;
}

} // namespace

extern "C" MediaDecoder *MEDIA_DECODER_Open(
    const char *path, const volatile bool *stop_requested)
{
    if (path == nullptr || (stop_requested != nullptr && *stop_requested)) {
        return nullptr;
    }

    if (!s_default_registered) {
        // Two registries: the simple-dec default only adds the container
        // parsers (WAV/M4A/TS/OGG); they dispatch into the elementary-stream
        // decoder registry, which must be populated separately -- without it
        // every MP3/FLAC open fails with "Decoder ... not registered".
        // Register only the codecs the playlist accepts (wav/mp3/flac/m4a/aac,
        // see music_playlist.cpp): esp_audio_dec_register_default() would link
        // every codec lib (Opus/Vorbis/LC3/SBC/...) and overflow the app
        // partition.
        const bool ok = esp_mp3_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_aac_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_flac_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_pcm_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_adpcm_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_audio_simple_dec_register_default() == ESP_AUDIO_ERR_OK;
        if (!ok) {
            ESP_LOGE(TAG, "register default decoders failed");
            return nullptr;
        }
        s_default_registered = true;
    }

    const bool is_http = strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0;
    const bool is_hls = is_http && strstr(path, ".m3u8") != nullptr;
    constexpr size_t kSmbMountLen = sizeof(SMB_VFS_MOUNT_POINT) - 1u;
    const bool is_smb = strncmp(path, SMB_VFS_MOUNT_POINT, kSmbMountLen) == 0 &&
                        (path[kSmbMountLen] == '/' || path[kSmbMountLen] == '\0');

    if (s_media_buffers_in_use) {
        ESP_LOGE(TAG, "only one media decoder can be active");
        return nullptr;
    }

    MediaDecoder *d = static_cast<MediaDecoder *>(heap_caps_calloc(1, sizeof(MediaDecoder), MALLOC_CAP_SPIRAM));
    if (d == nullptr) {
        return nullptr;
    }
    s_media_buffers_in_use = true;
    d->external_stop = stop_requested;

    if (is_hls) {
        // m3u8 playlist: resolve to a segment stream, then sniff the first
        // segment's leading bytes for container detection (TS / ADTS / MP3).
        d->hls = hls_open(path, &d->ring_stop);
        if (d->hls == nullptr) {
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        size_t sniffed = 0;
        while (sniffed < sizeof(d->sniff)) {
            const size_t got = hls_read(d->hls, d->sniff + sniffed, sizeof(d->sniff) - sniffed);
            if (got == 0u) {
                break;
            }
            sniffed += got;
        }
        d->sniff_len = sniffed;
    } else if (is_http) {
        // Net-radio stream: open the connection, then sniff the first bytes
        // for format detection (the stream is not seekable, so the sniffed
        // bytes are replayed ahead of the decoder's reads).
        d->http = http_open_stream(path, 10000, 4096);
        if (d->http == nullptr) {
            ESP_LOGE(TAG, "stream open failed: %s", path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        (void)esp_http_client_fetch_headers(d->http);
        const int status = esp_http_client_get_status_code(d->http);
        if (status != 200) {
            ESP_LOGE(TAG, "stream HTTP %d: %s", status, path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        int sniffed = 0;
        while (sniffed < static_cast<int>(sizeof(d->sniff))) {
            const int got = esp_http_client_read(d->http,
                                                 reinterpret_cast<char *>(d->sniff) + sniffed,
                                                 static_cast<int>(sizeof(d->sniff)) - sniffed);
            if (got <= 0) {
                break;
            }
            sniffed += got;
        }
        d->sniff_len = static_cast<size_t>(sniffed);
    } else if (is_smb) {
        d->source_is_smb = true;
        d->smb = SMB_STREAM_Open(path, stop_requested);
        if (d->smb == nullptr) {
            ESP_LOGE(TAG, "SMB stream open failed: %s", path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        const int sniffed = SMB_STREAM_Read(d->smb, d->sniff, sizeof(d->sniff));
        if (sniffed <= 0 || !SMB_STREAM_Seek(d->smb, 0, SEEK_SET)) {
            ESP_LOGE(TAG, "SMB stream sniff failed: %s", path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        d->sniff_len = static_cast<size_t>(sniffed);
        d->sniff_off = d->sniff_len; // stream rewound: nothing to replay
    } else {
        d->file = fopen(path, "rb");
        if (d->file == nullptr) {
            ESP_LOGE(TAG, "open failed: %s", path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        d->sniff_len = fread(d->sniff, 1, sizeof(d->sniff), d->file);
        (void)fseek(d->file, 0, SEEK_SET);
        d->sniff_off = d->sniff_len; // file rewound: nothing to replay
    }

    esp_audio_simple_dec_type_t type = detect_type_from_header(d->sniff, d->sniff_len, path);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE && is_hls) {
        // HLS segments that didn't sniff as TS/ADTS/MP3: raw AAC is the
        // usual remaining case.
        type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE && is_http) {
        // Typeless radio URL: MP3 is the de-facto webradio default.
        type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGE(TAG, "unsupported format: %s", path);
        MEDIA_DECODER_Close(d);
        return nullptr;
    }
    d->type = type;

    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC && d->file != nullptr) {
        FlacStreamInfo flac = {};
        if (flac_read_stream_info(d->file, &flac)) {
            flac_install_compact_prefix(d, &flac);
            ESP_LOGI(TAG, "FLAC streaminfo: %luHz %ubit %uch, audio offset=%ld",
                     static_cast<unsigned long>(flac.sample_rate_hz),
                     static_cast<unsigned>(flac.bits_per_sample),
                     static_cast<unsigned>(flac.channels),
                     flac.audio_offset);
        }
    }

    d->raw_buffer = s_raw_buffer;
    d->out_buffer = s_out_buffer;
    d->out_capacity = kOutputBufferBytes;

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = type;
    if (d->raw_buffer == nullptr || d->out_buffer == nullptr ||
        esp_audio_simple_dec_open(&cfg, &d->handle) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed: %s (%s)", path, esp_audio_simple_dec_get_name(type));
        MEDIA_DECODER_Close(d);
        return nullptr;
    }

    // File, HLS and direct HTTP sources read through the PSRAM ring so storage
    // or network latency does not reach the I2S writer as audio stutter.
    if (d->file != nullptr || d->smb != nullptr || d->hls != nullptr ||
        d->http != nullptr) {
        ring_start(d);
    }

    ESP_LOGI(TAG, "decoding %s as %s%s%s", path, esp_audio_simple_dec_get_name(type),
             is_hls ? " (hls)" : (is_http ? " (stream)" : ""),
             d->ring != nullptr ? " +readahead" : "");
    return d;
}

extern "C" int MEDIA_DECODER_Decode(MediaDecoder *d, const uint8_t **pcm_out, size_t *bytes_out)
{
    if (d == nullptr || d->handle == nullptr || pcm_out == nullptr || bytes_out == nullptr) {
        return -1;
    }

    // Progress diagnostics: a healthy call returns within a few reads. If we
    // are still in here after seconds, report where the time is going (spin
    // in the parser vs. blocked in the source read) instead of hanging mute.
    const int64_t enter_us = esp_timer_get_time();
    int64_t last_report_us = enter_us;
    unsigned iterations = 0;
    unsigned stalled_calls = 0;

    while (true) {
        if (decoder_stop_requested(d)) {
            return 0;
        }
        ++iterations;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_report_us > 5000000LL) {
            last_report_us = now_us;
            ESP_LOGW(TAG, "decode stalled %llds: %u loops, raw fill=%u off=%u eof=%d",
                     static_cast<long long>((now_us - enter_us) / 1000000LL),
                     iterations,
                     static_cast<unsigned>(d->raw_fill),
                     static_cast<unsigned>(d->raw_offset),
                     d->eof ? 1 : 0);
        }

        // Refill the raw buffer when the parser consumed everything.
        if (d->raw_offset >= d->raw_fill) {
            if (d->eof) {
                return 0;
            }
            const size_t raw_want = (d->type == ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC)
                                        ? kFlacRawChunkBytes
                                        : (d->source_is_smb ? kSmbRawChunkBytes
                                                            : kRawBufferBytes);
            d->raw_fill = read_source(d, d->raw_buffer, raw_want);
            d->raw_offset = 0;
            if (d->raw_fill == 0u) {
                if (d->source_error) {
                    return -1;
                }
                d->eof = true;
                return 0;
            }
            // Live HTTP streams legitimately return short reads; only local
            // files treat one as end-of-stream (FLAC needs the eos flag to
            // flush its tail).
            if ((d->file != nullptr || d->smb != nullptr) &&
                d->raw_fill < raw_want) {
                d->eof = true;
            }
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = d->raw_buffer + d->raw_offset;
        raw.len = static_cast<uint32_t>(d->raw_fill - d->raw_offset);
        raw.eos = d->eof;

        esp_audio_simple_dec_out_t frame = {};
        frame.buffer = d->out_buffer;
        frame.len = static_cast<uint32_t>(d->out_capacity);

        const esp_audio_err_t err = esp_audio_simple_dec_process(d->handle, &raw, &frame);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            const size_t before = d->out_capacity;
            if (!grow_out_buffer(d, frame.needed_size)) {
                ESP_LOGE(TAG, "out buffer grow to %lu failed",
                         static_cast<unsigned long>(frame.needed_size));
                return -1;
            }
            if (d->out_capacity == before) {
                // Decoder demands a buffer we already have: bail rather than
                // spin forever on the same error.
                ESP_LOGE(TAG, "decoder wants %lu bytes but %lu available",
                         static_cast<unsigned long>(frame.needed_size),
                         static_cast<unsigned long>(d->out_capacity));
                return -1;
            }
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "decode error %d", static_cast<int>(err));
            return -1;
        }

        d->raw_offset += raw.consumed;

        if (frame.decoded_size > 0u) {
            if (!d->info_valid) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(d->handle, &info) == ESP_AUDIO_ERR_OK) {
                    d->info.sample_rate_hz = info.sample_rate;
                    d->info.bits_per_sample = info.bits_per_sample;
                    d->info.channels = info.channel;
                    d->info_valid = true;
                }
            }
            *pcm_out = d->out_buffer;
            *bytes_out = frame.decoded_size;
            return 1;
        }

        // No output and nothing consumed: call process again with the SAME
        // untouched window. The simple decoders cache input internally and
        // may hold references into the unconsumed span, so it must never be
        // moved or overwritten (see the reference loop in the esp_audio_codec
        // test app) -- the old compact-and-refill here corrupted the TS
        // demuxer's state. Guard against a decoder that truly stops.
        if (raw.consumed == 0u) {
            if (++stalled_calls > 1000u) {
                ESP_LOGE(TAG, "parser made no progress (%u bytes pending)",
                         static_cast<unsigned>(d->raw_fill - d->raw_offset));
                return -1;
            }
        } else {
            stalled_calls = 0;
        }
    }
}

extern "C" bool MEDIA_DECODER_GetInfo(MediaDecoder *d, MediaDecoderInfo *out_info)
{
    if (d == nullptr || !d->info_valid || out_info == nullptr) {
        return false;
    }
    *out_info = d->info;
    return true;
}

extern "C" void MEDIA_DECODER_Close(MediaDecoder *d)
{
    if (d == nullptr) {
        return;
    }
    if (d->underrun_events > 0u) {
        ESP_LOGI(TAG, "underrun summary: %u events, %u ms dry total",
                 d->underrun_events, d->underrun_total_ms);
    }
    // Stop the read-ahead filler before touching the sources it reads from.
    if (d->ring != nullptr) {
        ring_stop_and_free(d);
        if (d->ring != nullptr) {
            // Filler wedged inside a source read: leak everything it might
            // still touch rather than free under its feet.
            return;
        }
    }
    if (d->handle != nullptr) {
        esp_audio_simple_dec_close(d->handle);
    }
    if (d->file != nullptr) {
        fclose(d->file);
    }
    if (d->smb != nullptr) {
        SMB_STREAM_Close(d->smb);
    }
    if (d->http != nullptr) {
        (void)esp_http_client_close(d->http);
        (void)esp_http_client_cleanup(d->http);
    }
    if (d->hls != nullptr) {
        hls_close(d->hls);
    }
    heap_caps_free(d);
    s_media_buffers_in_use = false;
}
