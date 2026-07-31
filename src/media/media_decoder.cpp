#include "media/media_decoder.h"

#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_fourcc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_helper.h"
#include "esp_gmf_io_file.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_port.h"
#include "esp_hls_io.h"

#include "lib/nrl_psram.h"
#include "media/gmf_io_radio.h"
#include "media/gmf_io_smb.h"
#include "services/smb_vfs.h"

static const char *TAG = "MDEC";

namespace {

// Decoded-PCM ring (PSRAM). The GMF pipeline's out port pushes PCM here from
// the decoder task; MEDIA_DECODER_Decode drains it from the player task, so
// source/network latency absorbed upstream (GMF IO threads) never reaches the
// I2S writer as stutter. 2 MB is ~11 s of 44.1 kHz stereo 16-bit. Reserved at
// link time so display/cover/codec allocations cannot fragment the large
// contiguous block before playback starts.
constexpr size_t kRingBytes = 2 * 1024 * 1024;
// Decode() copies one chunk out of the ring into this buffer; the pointer
// stays valid until the next Decode call.
constexpr size_t kOutputBufferBytes = 128 * 1024;
// Per-Decode target for file sources (file semantics: block until the chunk
// is full or the stream ends). Live streams return whatever is buffered.
constexpr size_t kDecodeChunkBytes = 32 * 1024;
// PCM reserve built before the first Decode returns on live sources. Direct
// HTTP radio is paced by the remote server; at 64 kbps-compressed this is
// roughly four seconds of decoded audio headroom.
constexpr size_t kHttpPrebufferBytes = 64 * 1024;
// Build enough SMB reserve before enabling I2S so a two-second reconnect can
// be hidden even for lossless audio. The pipeline continues toward the full
// 2 MB ring after playback begins.
constexpr size_t kSmbPrebufferBytes = 256 * 1024;
constexpr size_t kMinimumPrebufferBytes = 16 * 1024;
constexpr int64_t kHttpPrebufferMaxWaitUs = 3000000LL;
constexpr int64_t kSmbPrebufferMaxWaitUs = 8000000LL;
// Wait for the decoder's first REPORT_INFO (real PCM format) before the
// first Decode returns, and for the source IO to come up inside Open.
constexpr int64_t kInfoMaxWaitUs = 10000000LL;
constexpr int64_t kOpenMaxWaitUs = 20000000LL;

static bool s_default_registered = false;
NRL_PSRAM_BSS uint8_t s_media_ring[kRingBytes];
NRL_PSRAM_BSS uint8_t s_out_buffer[kOutputBufferBytes];
static bool s_media_buffers_in_use = false;
static esp_gmf_pool_handle_t s_pool = nullptr;

} // namespace

struct MediaDecoder {
    esp_gmf_pipeline_handle_t pipe;
    esp_gmf_task_handle_t task;
    // PCM ring: the out-port release callback (pipeline task) produces,
    // MEDIA_DECODER_Decode (player task) consumes.
    uint8_t *ring;
    volatile size_t ring_head;
    volatile size_t ring_tail;
    volatile bool abort;     // set by Close to unblock the release callback
    volatile bool opened;    // pipeline reported OPENING (source IO is up)
    volatile bool finished;  // pipeline FINISHED / last PCM payload seen
    volatile bool error;     // pipeline ERROR
    const volatile bool *external_stop;
    bool live;               // HTTP radio / HLS: short reads are normal
    bool source_is_smb;
    bool wait_full;          // file semantics: block for a full Decode chunk
    size_t prebuffer_bytes;  // PCM bytes required before the first Decode
    bool started;            // prebuffer/info gate already ran
    bool info_valid;
    MediaDecoderInfo info;
    // Underrun telemetry: the PCM ring ran dry while the player still needed
    // bytes (audible gap on I2S). Counted per event, logged at 1 Hz at most,
    // summarized on close.
    unsigned underrun_events;
    unsigned underrun_total_ms;
    int64_t underrun_start_us;
    int64_t underrun_last_log_us;
};

namespace {

static bool decoder_stop_requested(const MediaDecoder *d)
{
    return d->abort ||
           (d->external_stop != nullptr && *d->external_stop);
}

// ---- GMF pool (shared, created on first Open) -------------------------------

// Source routing is manual, not pool get_score: io_file scores every "/..."
// path (including /smb) STANDARD, the same as io_smb, so scoring is
// ambiguous. Explicit routing is deterministic.
static const char *select_in_io_tag(const char *path)
{
    constexpr size_t kSmbMountLen = sizeof(SMB_VFS_MOUNT_POINT) - 1u;
    if (strncmp(path, SMB_VFS_MOUNT_POINT, kSmbMountLen) == 0 &&
        (path[kSmbMountLen] == '/' || path[kSmbMountLen] == '\0')) {
        return "io_smb";
    }
    const bool is_http = strncmp(path, "http://", 7) == 0 ||
                         strncmp(path, "https://", 8) == 0;
    if (is_http && strstr(path, ".m3u8") != nullptr) {
        return "io_hls";
    }
    if (is_http) {
        return "io_radio";
    }
    return "io_file";
}

static bool register_default_decoders()
{
    if (s_default_registered) {
        return true;
    }
    // Two registries: the simple-dec default only adds the container parsers
    // (WAV/M4A/TS); they dispatch into the elementary-stream decoder
    // registry, which must be populated separately -- without it every
    // MP3/FLAC open fails with "Decoder ... not registered". Register only
    // the codecs the playlist accepts (wav/mp3/flac/m4a/aac, see
    // music_playlist.cpp): esp_audio_dec_register_default() would link every
    // codec lib (Opus/Vorbis/LC3/SBC/...) and overflow the app partition.
    const bool ok = esp_mp3_dec_register() == ESP_AUDIO_ERR_OK &&
                    esp_aac_dec_register() == ESP_AUDIO_ERR_OK &&
                    esp_flac_dec_register() == ESP_AUDIO_ERR_OK &&
                    esp_pcm_dec_register() == ESP_AUDIO_ERR_OK &&
                    esp_adpcm_dec_register() == ESP_AUDIO_ERR_OK &&
                    esp_audio_simple_dec_register_default() == ESP_AUDIO_ERR_OK;
    if (!ok) {
        ESP_LOGE(TAG, "register default decoders failed");
        return false;
    }
    s_default_registered = true;
    return true;
}

static bool gmf_pool_setup()
{
    if (s_pool != nullptr) {
        return true;
    }
    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        s_pool = nullptr;
        return false;
    }
    esp_gmf_io_handle_t io = nullptr;
    esp_gmf_element_handle_t el = nullptr;
    bool ok = false;

    // Local files (/sdcard, /usb): own thread + small PSRAM buffer so storage
    // reads decouple from the decode job (the old read-ahead filler role).
    file_io_cfg_t file_cfg = FILE_IO_CFG_DEFAULT();
    file_cfg.dir = ESP_GMF_IO_DIR_READER;
    file_cfg.io_cfg.thread.stack = 4096;
    file_cfg.io_cfg.thread.prio = 5;
    file_cfg.io_cfg.thread.core = 1;
    file_cfg.io_cfg.thread.stack_in_ext = true;
    file_cfg.io_cfg.buffer_cfg.io_size = 4096;
    file_cfg.io_cfg.buffer_cfg.buffer_size = 32 * 1024;
    if (esp_gmf_io_file_init(&file_cfg, &io) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_register_io(s_pool, io, nullptr) != ESP_GMF_ERR_OK) {
        goto fail;
    }
    // SMB share: deep buffer (PSRAM, see esp_gmf_oal_mem.c) to ride out the
    // request/response latency of the dedicated media connection.
    {
        gmf_io_smb_cfg_t smb_cfg = {};
        smb_cfg.dir = ESP_GMF_IO_DIR_READER;
        smb_cfg.io_cfg.thread.stack = 8192;
        smb_cfg.io_cfg.thread.prio = 5;
        smb_cfg.io_cfg.thread.core = 1;
        smb_cfg.io_cfg.thread.stack_in_ext = true;
        smb_cfg.io_cfg.buffer_cfg.io_size = 32 * 1024;
        smb_cfg.io_cfg.buffer_cfg.buffer_size = 256 * 1024;
        if (gmf_io_smb_init(&smb_cfg, &io) != ESP_GMF_ERR_OK ||
            esp_gmf_pool_register_io(s_pool, io, nullptr) != ESP_GMF_ERR_OK) {
            goto fail;
        }
    }
    // Direct HTTP(S) net radio, with the verify-then-insecure TLS fallback.
    {
        gmf_io_radio_cfg_t radio_cfg = {};
        radio_cfg.dir = ESP_GMF_IO_DIR_READER;
        radio_cfg.io_cfg.thread.stack = 8192;
        radio_cfg.io_cfg.thread.prio = 5;
        radio_cfg.io_cfg.thread.core = 1;
        radio_cfg.io_cfg.thread.stack_in_ext = true;
        radio_cfg.io_cfg.buffer_cfg.io_size = 4096;
        radio_cfg.io_cfg.buffer_cfg.buffer_size = 32 * 1024;
        if (gmf_io_radio_init(&radio_cfg, &io) != ESP_GMF_ERR_OK ||
            esp_gmf_pool_register_io(s_pool, io, nullptr) != ESP_GMF_ERR_OK) {
            goto fail;
        }
    }
    // HLS: official esp_hls_stream IO; fetches playlist+segments through pool
    // IOs (io_radio matches the http(s) scheme), extracts the audio
    // elementary stream from TS containers itself. Default 600 KB buffer and
    // 20 KB stack both land in PSRAM.
    {
        esp_hls_io_cfg_t hls_cfg = {};
        esp_gmf_io_cfg_t hls_io_cfg = DEFAULT_HLS_IO_CFG();
        hls_cfg.pool = s_pool;
        hls_cfg.io_cfg = hls_io_cfg;
        if (esp_gmf_io_hls_init(&hls_cfg, &io) != ESP_GMF_ERR_OK ||
            esp_gmf_pool_register_io(s_pool, io, nullptr) != ESP_GMF_ERR_OK) {
            goto fail;
        }
    }
    {
        esp_audio_simple_dec_cfg_t dec_cfg = DEFAULT_ESP_GMF_AUDIO_DEC_CONFIG();
        if (esp_gmf_audio_dec_init(&dec_cfg, &el) != ESP_GMF_ERR_OK ||
            esp_gmf_pool_register_element(s_pool, el, nullptr) != ESP_GMF_ERR_OK) {
            goto fail;
        }
    }
    ok = true;
fail:
    if (!ok) {
        ESP_LOGE(TAG, "GMF pool setup failed");
        esp_gmf_pool_deinit(s_pool);
        s_pool = nullptr;
    }
    return ok;
}

// ---- pipeline output port: PCM into the ring --------------------------------

static int mdec_acquire_write(void *handle, esp_gmf_payload_t *load,
                              uint32_t wanted_size, int block_ticks)
{
    return static_cast<int>(wanted_size);
}

static int mdec_release_write(void *handle, esp_gmf_payload_t *load, int block_ticks)
{
    MediaDecoder *d = static_cast<MediaDecoder *>(handle);
    if (load == nullptr) {
        return 0;
    }
    if (load->is_done) {
        d->finished = true; // last PCM chunk of the stream
    }
    const uint8_t *src = load->buf;
    size_t left = load->valid_size;
    while (left > 0u) {
        if (decoder_stop_requested(d)) {
            return -1; // abort the pipeline job (teardown in progress)
        }
        const size_t head = d->ring_head;
        const size_t tail = d->ring_tail;
        const size_t used = (head + kRingBytes - tail) % kRingBytes;
        const size_t free_space = kRingBytes - 1u - used;
        if (free_space == 0u) {
            // Bounded slices: even though the port is registered with
            // ESP_GMF_MAX_DELAY, a stop/abort is honoured within 50 ms.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t chunk = kRingBytes - head; // contiguous run up to the wrap
        if (chunk > free_space) {
            chunk = free_space;
        }
        if (chunk > left) {
            chunk = left;
        }
        memcpy(d->ring + head, src, chunk);
        d->ring_head = (head + chunk) % kRingBytes;
        src += chunk;
        left -= chunk;
    }
    return 0;
}

static esp_gmf_err_t mdec_event_cb(esp_gmf_event_pkt_t *pkt, void *ctx)
{
    MediaDecoder *d = static_cast<MediaDecoder *>(ctx);
    if (pkt->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        if (pkt->sub == ESP_GMF_EVENT_STATE_OPENING ||
            pkt->sub == ESP_GMF_EVENT_STATE_RUNNING) {
            d->opened = true; // source IO opened (or pipeline running)
        } else if (pkt->sub == ESP_GMF_EVENT_STATE_FINISHED) {
            d->finished = true;
        } else if (pkt->sub == ESP_GMF_EVENT_STATE_ERROR) {
            d->error = true;
        }
    } else if (pkt->type == ESP_GMF_EVT_TYPE_REPORT_INFO &&
               pkt->payload != nullptr &&
               pkt->payload_size >= static_cast<int>(sizeof(esp_gmf_info_sound_t))) {
        const esp_gmf_info_sound_t *snd =
            static_cast<const esp_gmf_info_sound_t *>(pkt->payload);
        if (snd->sample_rates > 0 && snd->channels > 0u && snd->bits > 0u) {
            d->info.sample_rate_hz = static_cast<uint32_t>(snd->sample_rates);
            d->info.channels = static_cast<uint8_t>(snd->channels);
            d->info.bits_per_sample = static_cast<uint8_t>(snd->bits);
            d->info_valid = true;
        }
    }
    return ESP_GMF_ERR_OK;
}

} // namespace

extern "C" MediaDecoder *MEDIA_DECODER_Open(
    const char *path, const volatile bool *stop_requested)
{
    if (path == nullptr || (stop_requested != nullptr && *stop_requested)) {
        return nullptr;
    }
    if (!register_default_decoders() || !gmf_pool_setup()) {
        return nullptr;
    }
    if (s_media_buffers_in_use) {
        ESP_LOGE(TAG, "only one media decoder can be active");
        return nullptr;
    }

    MediaDecoder *d = static_cast<MediaDecoder *>(
        heap_caps_calloc(1, sizeof(MediaDecoder), MALLOC_CAP_SPIRAM));
    if (d == nullptr) {
        return nullptr;
    }
    s_media_buffers_in_use = true;
    d->external_stop = stop_requested;
    d->ring = s_media_ring;

    const char *in_tag = select_in_io_tag(path);
    d->live = strcmp(in_tag, "io_radio") == 0 || strcmp(in_tag, "io_hls") == 0;
    d->source_is_smb = strcmp(in_tag, "io_smb") == 0;
    d->wait_full = !d->live;
    d->prebuffer_bytes = d->source_is_smb ? kSmbPrebufferBytes
                                          : (d->live ? kHttpPrebufferBytes : 0u);

    const char *el_names[] = {"aud_dec"};
    if (esp_gmf_pool_new_pipeline(s_pool, in_tag, el_names, 1, nullptr, &d->pipe) !=
            ESP_GMF_ERR_OK ||
        d->pipe == nullptr) {
        ESP_LOGE(TAG, "pipeline create failed: %s", path);
        MEDIA_DECODER_Close(d);
        return nullptr;
    }
    // The pipeline has no output IO; PCM leaves through this application
    // callback port on the last element.
    esp_gmf_port_handle_t out_port = static_cast<esp_gmf_port_handle_t>(
        NEW_ESP_GMF_PORT_OUT_BYTE(reinterpret_cast<void *>(mdec_acquire_write),
                                  reinterpret_cast<void *>(mdec_release_write),
                                  nullptr, d, 2048, ESP_GMF_MAX_DELAY));
    if (out_port == nullptr ||
        esp_gmf_pipeline_reg_el_port(d->pipe, "aud_dec", ESP_GMF_IO_DIR_WRITER,
                                     out_port) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "out port register failed");
        MEDIA_DECODER_Close(d);
        return nullptr;
    }
    esp_gmf_task_cfg_t tcfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    tcfg.thread.stack = 8192;
    tcfg.thread.prio = 5;
    tcfg.thread.core = 1;
    tcfg.thread.stack_in_ext = true;
    tcfg.name = "mdec";
    if (esp_gmf_task_init(&tcfg, &d->task) != ESP_GMF_ERR_OK || d->task == nullptr) {
        ESP_LOGE(TAG, "pipeline task create failed");
        MEDIA_DECODER_Close(d);
        return nullptr;
    }
    esp_gmf_task_set_timeout(d->task, 5000);
    esp_gmf_pipeline_bind_task(d->pipe, d->task);

    // Arm the cooperative stop on the pipeline's duplicated IN io instance.
    esp_gmf_io_handle_t in_io = nullptr;
    if (esp_gmf_pipeline_get_in(d->pipe, &in_io) == ESP_GMF_ERR_OK && in_io != nullptr) {
        if (d->source_is_smb) {
            gmf_io_smb_set_stop_request(in_io, stop_requested);
        } else if (strcmp(in_tag, "io_radio") == 0) {
            gmf_io_radio_set_stop_request(in_io, stop_requested);
        }
    }

    // Preconfigure the decoder from the URI; aud_dec re-sniffs the actual
    // content at runtime and reconfigures itself when it disagrees.
    esp_gmf_element_handle_t dec_el = nullptr;
    esp_gmf_info_sound_t snd = {};
    if (esp_gmf_pipeline_get_el_by_name(d->pipe, "aud_dec", &dec_el) != ESP_GMF_ERR_OK) {
        MEDIA_DECODER_Close(d);
        return nullptr;
    }
    esp_gmf_audio_helper_get_audio_type_by_uri(path, &snd.format_id);
    if (snd.format_id == 0) {
        snd.format_id = ESP_FOURCC_MP3; // typeless stream: webradio default
    }
    if (esp_gmf_audio_dec_reconfig_by_sound_info(dec_el, &snd) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_set_in_uri(d->pipe, path) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_loading_jobs(d->pipe) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pipeline setup failed: %s", path);
        MEDIA_DECODER_Close(d);
        return nullptr;
    }
    esp_gmf_pipeline_set_event(d->pipe, mdec_event_cb, d);
    if (esp_gmf_pipeline_run(d->pipe) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "pipeline run failed: %s", path);
        MEDIA_DECODER_Close(d);
        return nullptr;
    }

    // Synchronous open semantics: the pipeline opens the source IO in its own
    // task; wait for the outcome so a missing file / unreachable stream fails
    // here (NULL) instead of at the first Decode.
    const int64_t wait_start_us = esp_timer_get_time();
    while (!d->opened && !d->error && !decoder_stop_requested(d) &&
           esp_timer_get_time() - wait_start_us < kOpenMaxWaitUs) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!d->opened || d->error) {
        ESP_LOGE(TAG, "source open failed: %s", path);
        MEDIA_DECODER_Close(d);
        return nullptr;
    }
    ESP_LOGI(TAG, "decoding %s via %s%s", path, in_tag, d->live ? " (live)" : "");
    return d;
}

extern "C" int MEDIA_DECODER_Decode(MediaDecoder *d, const uint8_t **pcm_out,
                                    size_t *bytes_out)
{
    if (d == nullptr || pcm_out == nullptr || bytes_out == nullptr) {
        return -1;
    }

    if (!d->started) {
        d->started = true;
        // Prebuffer gate (live sources only): build a PCM reserve before I2S
        // playback begins so network jitter does not immediately drain it.
        if (d->prebuffer_bytes > 0u) {
            const int64_t wait_started_us = esp_timer_get_time();
            while (!d->finished && !d->error && !decoder_stop_requested(d)) {
                const size_t used = (d->ring_head + kRingBytes - d->ring_tail) % kRingBytes;
                const bool target_reached = used >= d->prebuffer_bytes;
                const int64_t max_wait_us = d->source_is_smb
                                                ? kSmbPrebufferMaxWaitUs
                                                : kHttpPrebufferMaxWaitUs;
                const bool wait_expired =
                    used >= kMinimumPrebufferBytes &&
                    esp_timer_get_time() - wait_started_us >= max_wait_us;
                if (target_reached || wait_expired) {
                    ESP_LOGI(TAG, "prebuffer: %s buffered %u PCM bytes%s",
                             d->source_is_smb ? "SMB" : "HTTP",
                             static_cast<unsigned>(used),
                             target_reached ? "" : " (startup timeout)");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        // The first decoded frame's REPORT_INFO carries the real PCM format;
        // wait for it so GetInfo is valid right after the first Decode.
        const int64_t info_wait_us = esp_timer_get_time();
        while (!d->info_valid && !d->finished && !d->error &&
               !decoder_stop_requested(d) &&
               esp_timer_get_time() - info_wait_us < kInfoMaxWaitUs) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    const size_t want = d->wait_full ? kDecodeChunkBytes : 1u;
    size_t got = 0;
    while (true) {
        if (decoder_stop_requested(d)) {
            if (got == 0u) {
                return 0;
            }
            break;
        }
        const size_t head = d->ring_head;
        const size_t tail = d->ring_tail;
        const size_t used = (head + kRingBytes - tail) % kRingBytes;
        if (used == 0u) {
            if (d->error) {
                if (got == 0u) {
                    return -1;
                }
                break; // drain what is left, fail on the next call
            }
            if (d->finished) {
                if (got == 0u) {
                    return 0;
                }
                break;
            }
            if (!d->wait_full && got > 0u) {
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
                ESP_LOGW(TAG, "underrun: PCM ring dry (event #%u, total %u ms)",
                         d->underrun_events, d->underrun_total_ms);
            }
        }
        size_t chunk = kRingBytes - tail; // contiguous run up to the wrap
        if (chunk > used) {
            chunk = used;
        }
        if (chunk > kOutputBufferBytes - got) {
            chunk = kOutputBufferBytes - got;
        }
        memcpy(s_out_buffer + got, d->ring + tail, chunk);
        d->ring_tail = (tail + chunk) % kRingBytes;
        got += chunk;
        if (got >= want || got >= kOutputBufferBytes) {
            break;
        }
    }
    if (got == 0u) {
        return 0;
    }
    *pcm_out = s_out_buffer;
    *bytes_out = got;
    return 1;
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
    // Wake a pipeline job blocked in the PCM release callback before stopping,
    // then stop: the stop flow closes the source IO (its prev_close breaks a
    // blocked network read) and waits for every job to exit.
    d->abort = true;
    if (d->pipe != nullptr) {
        (void)esp_gmf_pipeline_stop(d->pipe);
        (void)esp_gmf_pipeline_destroy(d->pipe);
        d->pipe = nullptr;
    }
    if (d->task != nullptr) {
        (void)esp_gmf_task_deinit(d->task);
        d->task = nullptr;
    }
    heap_caps_free(d);
    s_media_buffers_in_use = false;
}
