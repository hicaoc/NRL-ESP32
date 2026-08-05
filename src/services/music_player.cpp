#include "services/music_player.h"

#include "audio/audio_focus.h"
#include "audio/voice_resampler.h"
#include "esp_asrc.h"
#include "services/config_notify.h"
#include "driver/board_pins.h"
#include "driver/es8311.h"
#include "driver/es8389.h"
#include "lib/nrl_audio_bridge.h"
#include "lib/nrl_bt_hfp.h"
#include "lib/nrl_psram.h"
#include "media/media_decoder.h"
#include "services/smb_vfs.h"
#include "services/storage_service.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MUSIC";

namespace {

constexpr size_t kMaxPathLen = 256; // long enough for net-radio URLs
constexpr uint32_t kVoiceResumeDelayMs = 30000u;
constexpr size_t kStereoScratchSamples = 64u * 1024u; // 128 KB
constexpr size_t kNetScratchSamples = 32u * 1024u;    // 64 KB
constexpr size_t kBtScratchSamples = 128u * 1024u;    // 256 KB

static bool speaker_hifi_acquire(const uint32_t sample_rate_hz,
                                 const uint8_t bits_per_sample,
                                 const uint8_t channels)
{
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    return ES8311_HifiAcquire(sample_rate_hz, bits_per_sample, channels);
#elif defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    return ES8389_HifiAcquire(sample_rate_hz, bits_per_sample, channels);
#else
    (void)sample_rate_hz;
    (void)bits_per_sample;
    (void)channels;
    return false;
#endif
}

static bool speaker_hifi_write(const void *pcm, const size_t bytes)
{
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    return ES8311_HifiWrite(pcm, bytes);
#elif defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    return ES8389_HifiWrite(pcm, bytes);
#else
    (void)pcm;
    (void)bytes;
    return false;
#endif
}

static bool speaker_hifi_release(void)
{
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    return ES8311_HifiRelease();
#elif defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    return ES8389_HifiRelease();
#else
    return true;
#endif
}

static TaskHandle_t s_player_task = nullptr;
static volatile bool s_track_running = false;
// Persistent player task on a static internal stack. SD/FatFS access can run
// with the flash cache paused, so the stack must be internal (PSRAM is
// unreachable then); and it must be static/one-shot: the runtime heap
// shatters to ~1.5 KB largest blocks in steady state (see the BT reserve in
// nrl_bt_hfp.cpp), so the old per-track xTaskCreate of an 8 KB stack failed
// with "player task create failed" once the device had been up a while.
constexpr size_t kPlayerStackBytes = 8192;
static StackType_t s_player_stack[kPlayerStackBytes / sizeof(StackType_t)];
static StaticTask_t s_player_tcb;
static volatile bool s_stop_requested = false;
static volatile bool s_playing = false;
static volatile bool s_focus_voice_active = false;
static volatile bool s_focus_suspended = false;
static volatile uint32_t s_focus_resume_at_ms = 0u;
static char s_current_path[kMaxPathLen] = {};
static MediaTrackInfo s_track_info = {};

// Mono tracks are expanded to stereo before the codec: always opening the
// I2S/codec 2-channel avoids per-format bring-up risk on the slot config.
NRL_PSRAM_BSS static int16_t s_stereo_storage[kStereoScratchSamples];
static int16_t *s_stereo_buffer = s_stereo_storage;
static size_t s_stereo_capacity = sizeof(s_stereo_storage);
static volatile int s_target = MUSIC_TARGET_LOCAL;

// 8 kHz mono scratch for the network branch of the fan-out.
NRL_PSRAM_BSS static int16_t s_net_storage[kNetScratchSamples];
static int16_t *s_net_buffer = s_net_storage;
static size_t s_net_capacity = kNetScratchSamples;

// Local output device (speaker hi-fi path vs BT headset A2DP).
static volatile int s_output = MUSIC_OUTPUT_SPEAKER;

// 44.1 kHz stereo scratch for the A2DP branch.
NRL_PSRAM_BSS static int16_t s_bt_storage[kBtScratchSamples];
static int16_t *s_bt_buffer = s_bt_storage;
static size_t s_bt_capacity = kBtScratchSamples; // in interleaved samples

// Arbitrary-rate mono/stereo PCM16 -> 44.1 kHz stereo for A2DP, built on
// esp_asrc: channel conversion (weights) and band-limited rate conversion in
// one pass, hardware-accelerated on ESP32-S31 with an automatic
// optimized-software fallback on ESP32-S3.
struct BtResampler {
    uint8_t channels;
    esp_asrc_handle_t asrc;
    // Staging buffers (esp_asrc_align_alloc): the hardware path requires
    // cache-line-aligned buffers, and callers' pointers cannot guarantee that.
    uint8_t *in_buf;
    size_t in_cap_bytes;
    uint8_t *out_buf;
    size_t out_cap_bytes;
};

// Hardware-path frame timeout; the ASRC peripheral converts far faster than
// real time, so 500 ms can only fire on a genuine hardware wedge.
constexpr int32_t kBtAsrcTimeoutMs = 500;

static bool bt_grow_aligned(uint8_t **buf, size_t *cap, const size_t need,
                            const uint32_t addr_align, const uint32_t size_align)
{
    if (need <= *cap) {
        return true;
    }
    uint32_t allocated = 0;
    void *p = esp_asrc_align_alloc(static_cast<uint32_t>(need), addr_align,
                                   size_align, &allocated);
    if (p == nullptr) {
        return false;
    }
    free(*buf);
    *buf = static_cast<uint8_t *>(p);
    *cap = allocated;
    return true;
}

static void bt_resampler_deinit(BtResampler *rs)
{
    if (rs->asrc != nullptr) {
        esp_asrc_close(rs->asrc);
        rs->asrc = nullptr;
    }
    free(rs->in_buf);
    rs->in_buf = nullptr;
    rs->in_cap_bytes = 0;
    free(rs->out_buf);
    rs->out_buf = nullptr;
    rs->out_cap_bytes = 0;
}

// Idempotent re-init per output acquisition (focus suspend/resume re-runs
// this). Returns false on handle allocation failure; caller falls back to
// the speaker path.
static bool bt_resampler_init(BtResampler *rs, const uint32_t in_rate, const uint8_t channels)
{
    if (channels != 1u && channels != 2u) {
        return false;
    }
    bt_resampler_deinit(rs);
    rs->channels = channels;

    // NULL weight: stereo stays put, mono duplicates to both channels.
    esp_asrc_cfg_t cfg = {};
    cfg.src_info.sample_rate = in_rate;
    cfg.src_info.channel = channels;
    cfg.src_info.bits_per_sample = 16u;
    cfg.dest_info.sample_rate = 44100u;
    cfg.dest_info.channel = 2u;
    cfg.dest_info.bits_per_sample = 16u;
    cfg.perf_type = ESP_ASRC_PERF_TYPE_AUTO;
    cfg.complexity = 2u; // music-grade, unlike the voice uplink path
    cfg.timeout_ms = kBtAsrcTimeoutMs;
    if (esp_asrc_open(&cfg, &rs->asrc) != ESP_ASRC_ERR_OK) {
        bt_resampler_deinit(rs);
        return false;
    }
    return true;
}

// in: interleaved PCM16 (mono or stereo) frames; out: 44.1 kHz stereo
// interleaved, at most out_cap_frames frames.
static size_t bt_resampler_process(BtResampler *rs, const int16_t *in, const size_t in_frames,
                                   int16_t *out, const size_t out_cap_frames)
{
    if (in_frames == 0u || out_cap_frames == 0u || rs->asrc == nullptr) {
        return 0;
    }
    esp_asrc_buffer_alignment_t align = {};
    if (esp_asrc_get_buffer_alignment(&align) != ESP_ASRC_ERR_OK) {
        return 0;
    }
    const size_t in_bytes = in_frames * rs->channels * sizeof(int16_t);
    uint32_t out_frames_max = 0;
    if (esp_asrc_get_out_sample_num(rs->asrc, static_cast<uint32_t>(in_frames),
                                    &out_frames_max) != ESP_ASRC_ERR_OK) {
        return 0;
    }
    const size_t out_bytes = static_cast<size_t>(out_frames_max) * 2u * sizeof(int16_t);
    if (!bt_grow_aligned(&rs->in_buf, &rs->in_cap_bytes, in_bytes,
                         align.inbuf_addr_align, align.inbuf_size_align) ||
        !bt_grow_aligned(&rs->out_buf, &rs->out_cap_bytes, out_bytes,
                         align.outbuf_addr_align, align.outbuf_size_align)) {
        return 0;
    }
    memcpy(rs->in_buf, in, in_bytes);
    uint32_t produced = out_frames_max;
    if (esp_asrc_process(rs->asrc, rs->in_buf, static_cast<uint32_t>(in_frames),
                         rs->out_buf, &produced) != ESP_ASRC_ERR_OK) {
        return 0;
    }
    size_t frames = produced;
    if (frames > out_cap_frames) {
        frames = out_cap_frames; // clamp defensively
    }
    memcpy(out, rs->out_buf, frames * 2u * sizeof(int16_t));
    return frames;
}

// Playback target + net-radio station persist in NVS so the web portal and
// LCD UI survive a reboot (the AT path shares the same setters).
constexpr const char *kMediaNvsNamespace = "media";
static char s_radio_url[kMaxPathLen] = {};

static void media_config_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kMediaNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t target = MUSIC_TARGET_LOCAL;
    if (nvs_get_u8(nvs, "target", &target) == ESP_OK &&
        target <= MUSIC_TARGET_BOTH) {
        s_target = target;
    }
    uint8_t output = MUSIC_OUTPUT_SPEAKER;
    if (nvs_get_u8(nvs, "output", &output) == ESP_OK &&
        output <= MUSIC_OUTPUT_BT) {
        s_output = output;
    }
    size_t len = sizeof(s_radio_url);
    if (nvs_get_str(nvs, "radio_url", s_radio_url, &len) != ESP_OK) {
        s_radio_url[0] = '\0';
    }
    nvs_close(nvs);
}

static void media_config_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kMediaNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "media config persist failed");
        return;
    }
    (void)nvs_set_u8(nvs, "target", static_cast<uint8_t>(s_target));
    (void)nvs_set_u8(nvs, "output", static_cast<uint8_t>(s_output));
    (void)nvs_set_str(nvs, "radio_url", s_radio_url);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
    CONFIG_NOTIFY_Bump();
}

static size_t pcm24le_to_pcm32le_stereo(const uint8_t *src, const size_t bytes, int32_t *dst)
{
    if (src == nullptr || dst == nullptr) {
        return 0;
    }
    const size_t samples = bytes / 3u;
    for (size_t i = 0; i < samples; ++i) {
        const uint8_t *p = src + i * 3u;
        int32_t sample = static_cast<int32_t>(p[0]) |
                         (static_cast<int32_t>(p[1]) << 8) |
                         (static_cast<int32_t>(p[2]) << 16);
        if ((sample & 0x00800000) != 0) {
            sample |= static_cast<int32_t>(0xFF000000);
        }
        dst[i] = sample << 8;
    }
    return samples * sizeof(int32_t);
}

static volatile MusicTrackEndCb_t s_track_end_cb = nullptr;

static bool is_smb_path(const char *path)
{
    constexpr size_t kMountLen = sizeof(SMB_VFS_MOUNT_POINT) - 1u;
    return path != nullptr &&
           strncmp(path, SMB_VFS_MOUNT_POINT, kMountLen) == 0 &&
           (path[kMountLen] == '/' || path[kMountLen] == '\0');
}

// Stream format of the current track for the UI's format line; written by
// the player task when the first frame decodes, cleared when it exits.
static MediaDecoderInfo s_stream_info = {};
static volatile bool s_stream_info_valid = false;

static bool media_focus_blocks_playback()
{
    if (s_focus_voice_active) {
        return true;
    }
    const uint32_t resume_at = s_focus_resume_at_ms;
    if (resume_at == 0u) {
        return false;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (static_cast<int32_t>(now - resume_at) < 0) {
        return true;
    }
    s_focus_resume_at_ms = 0u;
    return false;
}

static void play_current_track()
{
    // Tags first (cheap header/tail reads), so the UI has title/cover as
    // soon as -- or before -- the first PCM reaches the speaker. SMB is the
    // exception: its many small seeks are expensive on the embedded client
    // and delayed first sound by several seconds. Start network playback
    // immediately; the UI already falls back to the filename for its title.
    const bool smb_track = is_smb_path(s_current_path);
    if (!smb_track) {
        (void)MEDIA_META_Read(s_current_path, &s_track_info, true);
    }

    // Playback target is latched per track (docs/architecture.md nanny
    // 三档切换): local hi-fi, NRL network uplink, or a fan-out of both --
    // one decode, two consumers.
    const int target = s_target;
    const bool to_local = (target != MUSIC_TARGET_NET);
    const bool to_net = (target != MUSIC_TARGET_LOCAL);

    bool hifi_acquired = false;
    bool bt_active = false;
    bool format_ready = false;
    bool reached_end = false;
    // Consecutive decode-failure reconnects (live streams only); reset on the
    // first successfully decoded chunk after a reconnect.
    int stream_reconnects = 0;
    VoiceResampler resampler = {};
    BtResampler bt_resampler = {};
    // Pacing for the net-only case: without the I2S DMA back-pressure the
    // decode loop would free-run; throttle to real time by the 8 kHz clock.
    int64_t net_start_us = 0;
    uint64_t net_sent_samples = 0;
    MediaDecoder *decoder = MEDIA_DECODER_Open(s_current_path, &s_stop_requested);
    const bool live_stream = strncmp(s_current_path, "http://", 7) == 0 ||
                             strncmp(s_current_path, "https://", 8) == 0;
    bool media_uplink_active = false;

    auto acquire_local_output = [&](const MediaDecoderInfo &info) -> bool {
        if (!to_local) {
            return true;
        }
        const bool speaker_24bit = info.bits_per_sample == 24u &&
                                   info.channels == 2u && !to_net;
        if (s_output == MUSIC_OUTPUT_BT && info.bits_per_sample == 16u &&
            NRL_BtA2dp_RequestStart()) {
            for (int i = 0; i < 40 && !NRL_BtA2dp_IsStreaming() &&
                            !s_stop_requested; ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            bt_active = NRL_BtA2dp_IsStreaming();
            if (bt_active) {
                if (bt_resampler_init(&bt_resampler, info.sample_rate_hz, info.channels)) {
                    return true;
                }
                ESP_LOGW(TAG, "A2DP resampler init failed, falling back to speaker");
                NRL_BtA2dp_RequestStop();
                bt_active = false;
            } else {
                ESP_LOGW(TAG, "A2DP not ready, falling back to speaker");
            }
        }
        const uint8_t speaker_bits = speaker_24bit ? 32u : 16u;
        if (!speaker_hifi_acquire(info.sample_rate_hz, speaker_bits, 2u)) {
            ESP_LOGE(TAG, "hi-fi speaker unavailable (wrong board, or voice busy)");
            return false;
        }
        hifi_acquired = true;
        return true;
    };

    auto release_outputs = [&]() {
        if (hifi_acquired) {
            speaker_hifi_release();
            hifi_acquired = false;
        }
        if (bt_active) {
            NRL_BtA2dp_RequestStop();
            bt_active = false;
        }
        if (media_uplink_active) {
            NRLAudioBridge_SetMediaUplinkActive(false);
            media_uplink_active = false;
        }
    };

    if (to_net && decoder != nullptr) {
        NRLAudioBridge_SetMediaUplinkActive(true);
        media_uplink_active = true;
    }

    while (!s_stop_requested) {
        if (media_focus_blocks_playback()) {
            if (!s_focus_suspended) {
                release_outputs();
                // A live stream cannot remain idle for the 30-second voice
                // holdoff. Reconnect it at the current live point on resume;
                // file decoders stay open and retain their exact position.
                if (live_stream && decoder != nullptr) {
                    MEDIA_DECODER_Close(decoder);
                    decoder = nullptr;
                    format_ready = false;
                    s_stream_info_valid = false;
                }
                s_focus_suspended = true;
                ESP_LOGI(TAG, "voice focus: playback paused");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_focus_suspended) {
            if (live_stream) {
                decoder = MEDIA_DECODER_Open(s_current_path, &s_stop_requested);
                if (decoder == nullptr) {
                    ESP_LOGE(TAG, "voice focus: stream reconnect failed: %s", s_current_path);
                    break;
                }
            }
            if (to_net && decoder != nullptr) {
                NRLAudioBridge_SetMediaUplinkActive(true);
                media_uplink_active = true;
                net_start_us = esp_timer_get_time();
                net_sent_samples = 0u;
            }
            if (format_ready && !acquire_local_output(s_stream_info)) {
                if (media_focus_blocks_playback()) {
                    release_outputs();
                    continue;
                }
                break;
            }
            s_focus_suspended = false;
            ESP_LOGI(TAG, "voice focus: playback resumed after 30 seconds idle");
        }

        if (decoder == nullptr) {
            break;
        }

        const uint8_t *pcm = nullptr;
        size_t bytes = 0;
        const int rc = MEDIA_DECODER_Decode(decoder, &pcm, &bytes);
        if (rc <= 0) {
            if (rc < 0) {
                // A live stream has no pristine source to re-read: network
                // jitter can land the MP3 parser on a false sync, and one
                // garbage frame fails the whole aud_dec job (pipeline abort).
                // Reconnect at the live point instead of ending the track,
                // with bounded retries so a dead stream still gives up.
                constexpr int kMaxStreamReconnects = 3;
                if (live_stream && !s_stop_requested &&
                    stream_reconnects < kMaxStreamReconnects) {
                    ++stream_reconnects;
                    ESP_LOGW(TAG, "decode error, reconnecting stream (%d/%d): %s",
                             stream_reconnects, kMaxStreamReconnects, s_current_path);
                    MEDIA_DECODER_Close(decoder);
                    decoder = nullptr;
                    format_ready = false;
                    s_stream_info_valid = false;
                    release_outputs();
                    for (int waited = 0; waited < 1000 * stream_reconnects &&
                                        !s_stop_requested; waited += 100) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    if (!s_stop_requested) {
                        decoder = MEDIA_DECODER_Open(s_current_path, &s_stop_requested);
                    }
                    if (decoder == nullptr) {
                        ESP_LOGE(TAG, "stream reconnect failed: %s", s_current_path);
                        break;
                    }
                    if (to_net) {
                        NRLAudioBridge_SetMediaUplinkActive(true);
                        media_uplink_active = true;
                        net_start_us = esp_timer_get_time();
                        net_sent_samples = 0u;
                    }
                    continue;
                }
                ESP_LOGE(TAG, "decode failed: %s", s_current_path);
            } else {
                reached_end = format_ready;
            }
            break;
        }
        stream_reconnects = 0;

        MediaDecoderInfo info = {};
        if (!format_ready) {
            if (!MEDIA_DECODER_GetInfo(decoder, &info)) {
                ESP_LOGE(TAG, "no format info from decoder");
                break;
            }
            const bool speaker_24bit =
                info.bits_per_sample == 24u && info.channels == 2u &&
                to_local && !to_net;
            if ((info.bits_per_sample != 16u && !speaker_24bit) ||
                (info.channels != 1u && info.channels != 2u)) {
                ESP_LOGE(TAG, "unsupported PCM: %ubit %uch (16-bit 1/2ch, or speaker 24-bit stereo)",
                         static_cast<unsigned>(info.bits_per_sample),
                         static_cast<unsigned>(info.channels));
                break;
            }
            if (media_focus_blocks_playback()) {
                continue;
            }
            if (!acquire_local_output(info)) {
                break;
            }
            if (to_net) {
                if (!VOICE_RESAMPLER_Init(&resampler, info.sample_rate_hz, info.channels)) {
                    ESP_LOGE(TAG, "resampler init failed (%luHz)",
                             static_cast<unsigned long>(info.sample_rate_hz));
                    break;
                }
                net_start_us = esp_timer_get_time();
            }
            format_ready = true;
            s_stream_info = info;
            s_stream_info_valid = true;
            ESP_LOGI(TAG, "playing %s: %luHz %ubit %uch target=%s",
                     s_current_path,
                     static_cast<unsigned long>(info.sample_rate_hz),
                     static_cast<unsigned>(info.bits_per_sample),
                     static_cast<unsigned>(info.channels),
                     (target == MUSIC_TARGET_BOTH) ? "both"
                                                   : (to_net ? "net" : "local"));
        }

        (void)MEDIA_DECODER_GetInfo(decoder, &info);

        if (to_net) {
            const size_t in_frames = bytes / (sizeof(int16_t) * info.channels);
            const int16_t *input = reinterpret_cast<const int16_t *>(pcm);
            constexpr size_t kInputChunkFrames = 8192u;
            for (size_t offset = 0u; offset < in_frames;) {
                const size_t take = (in_frames - offset < kInputChunkFrames)
                                        ? in_frames - offset : kInputChunkFrames;
                const size_t out_n = VOICE_RESAMPLER_Process(
                    &resampler, input + offset * info.channels, take,
                    s_net_buffer, s_net_capacity);
                if (out_n > 0u) {
                    NRLAudioBridge_SendMediaUplink(s_net_buffer, out_n);
                    net_sent_samples += out_n;
                }
                offset += take;
            }
            if (!to_local) {
                // Throttle to real time: stay ~100 ms ahead of the 8 kHz clock.
                const int64_t audio_us = static_cast<int64_t>(net_sent_samples) * 1000000LL / 8000LL;
                const int64_t elapsed_us = esp_timer_get_time() - net_start_us;
                const int64_t lead_us = audio_us - elapsed_us - 100000LL;
                if (lead_us > 20000LL) {
                    vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(lead_us / 1000LL)));
                }
            }
        }

        if (to_local && bt_active) {
            const size_t in_frames = bytes / (sizeof(int16_t) * info.channels);
            const int16_t *input = reinterpret_cast<const int16_t *>(pcm);
            constexpr size_t kInputChunkFrames = 4096u;
            bool bt_failed = false;
            for (size_t offset = 0u; offset < in_frames;) {
                const size_t take = (in_frames - offset < kInputChunkFrames)
                                        ? in_frames - offset : kInputChunkFrames;
                const size_t out_frames = bt_resampler_process(
                    &bt_resampler, input + offset * info.channels, take,
                    s_bt_buffer, s_bt_capacity / 2u);
                if (out_frames > 0u &&
                    NRL_BtA2dp_Write(s_bt_buffer, out_frames) == 0u && !NRL_BtA2dp_IsStreaming()) {
                    ESP_LOGW(TAG, "A2DP stream lost, stopping track");
                    bt_failed = true;
                    break;
                }
                offset += take;
            }
            if (bt_failed) break;
        } else if (to_local) {
            if (info.bits_per_sample == 24u && info.channels == 2u) {
                const size_t frames = bytes / 6u;
                const size_t chunk_frames = s_stereo_capacity / (2u * sizeof(int32_t));
                bool write_failed = false;
                for (size_t offset = 0u; offset < frames;) {
                    const size_t take = (frames - offset < chunk_frames) ? frames - offset : chunk_frames;
                    const size_t converted = pcm24le_to_pcm32le_stereo(
                        pcm + offset * 6u, take * 6u,
                        reinterpret_cast<int32_t *>(s_stereo_buffer));
                    if (converted == 0u || !speaker_hifi_write(s_stereo_buffer, converted)) {
                        write_failed = true;
                        break;
                    }
                    offset += take;
                }
                if (write_failed) break;
                continue;
            }
            if (info.channels == 2u) {
                if (!speaker_hifi_write(pcm, bytes)) {
                    break;
                }
            } else {
                const size_t samples = bytes / sizeof(int16_t);
                const int16_t *mono = reinterpret_cast<const int16_t *>(pcm);
                const size_t chunk_samples = s_stereo_capacity / (2u * sizeof(int16_t));
                bool write_failed = false;
                for (size_t offset = 0u; offset < samples;) {
                    const size_t take = (samples - offset < chunk_samples) ? samples - offset : chunk_samples;
                    for (size_t i = 0u; i < take; ++i) {
                        s_stereo_buffer[i * 2u] = mono[offset + i];
                        s_stereo_buffer[i * 2u + 1u] = mono[offset + i];
                    }
                    if (!speaker_hifi_write(s_stereo_buffer, take * 2u * sizeof(int16_t))) {
                        write_failed = true;
                        break;
                    }
                    offset += take;
                }
                if (write_failed) break;
            }
        }
    }

    ESP_LOGI(TAG, "%s: %s", s_current_path, s_stop_requested ? "stopped" : "finished");

    VOICE_RESAMPLER_Deinit(&resampler);
    bt_resampler_deinit(&bt_resampler);
    release_outputs();
    if (decoder != nullptr) {
        MEDIA_DECODER_Close(decoder);
    }

    s_stream_info_valid = false;
    s_focus_suspended = false;
    s_playing = false;
    s_track_running = false;

    // Auto-advance hook: the track is fully released and s_track_running is
    // clear, so the callback may call MUSIC_PlayFile -- it notifies this
    // task, and the outer loop starts the next track as soon as we return.
    const MusicTrackEndCb_t end_cb = s_track_end_cb;
    if (reached_end && !s_stop_requested && end_cb != nullptr) {
        end_cb();
    }
}

// One persistent player task: MUSIC_PlayFile hands over a track with a
// notification; play_current_track() runs it to the end and loops. No
// per-track task create/delete churn on the fragmented runtime heap.
static void player_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        play_current_track();
    }
}

static void on_voice_start(void)
{
    s_focus_voice_active = true;
    s_focus_resume_at_ms = 0u;
    if (s_playing) {
        ESP_LOGI(TAG, "voice focus acquired, pausing media");
    }
}

static void on_voice_end(void)
{
    s_focus_voice_active = false;
    s_focus_resume_at_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL) +
                           kVoiceResumeDelayMs;
    if (s_playing) {
        ESP_LOGI(TAG, "voice focus released, media resumes after 30 seconds idle");
    }
}

} // namespace

extern "C" void MUSIC_Init(void)
{
    AudioFocus_RegisterVoiceStart(on_voice_start);
    AudioFocus_RegisterVoiceEnd(on_voice_end);
    media_config_load();
    // Create the persistent player task now, while the boot heap is still
    // whole -- a static stack anyway, so this can only fail on a bug.
    // Priority below the voice passthrough task (10): file reads, decode and
    // I2S writes are throughput work, not latency-critical.
    s_player_task = xTaskCreateStaticPinnedToCore(player_task, "music_player",
                                                  kPlayerStackBytes, nullptr, 5,
                                                  s_player_stack, &s_player_tcb, 1);
    if (s_player_task == nullptr) {
        ESP_LOGE(TAG, "player task create failed at init");
    }
}

extern "C" bool MUSIC_PlayFile(const char *path)
{
    if (path == nullptr || path[0] == '\0' || strlen(path) >= kMaxPathLen) {
        return false;
    }

    // Radio contention: SMB (network TCP bulk) cannot stream while Bluetooth holds
    // the single shared radio (SMB2 I/O timeouts). Refuse network tracks while BT
    // is on -- covers auto-advance / prev-next / AT, not just the UI. Local
    // SD/USB playback is unaffected. (The UI also hides SMB tracks while BT is on.)
    if (is_smb_path(path) && NRL_BtHfp_IsEnabled()) {
        ESP_LOGW(TAG, "refusing SMB track while Bluetooth is on (radio contention): %s", path);
        return false;
    }

    MUSIC_Stop();
    // Wait for the previous track to wind down (it releases the codec).
    for (int i = 0; i < 200 && s_track_running; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_track_running) {
        ESP_LOGE(TAG, "previous track did not stop");
        return false;
    }

    MEDIA_META_Free(&s_track_info);
    memset(&s_track_info, 0, sizeof(s_track_info));

    snprintf(s_current_path, sizeof(s_current_path), "%s", path);
    s_stop_requested = false;
    s_playing = true;

    if (s_player_task == nullptr) {
        s_playing = false;
        ESP_LOGE(TAG, "player task unavailable (init failed)");
        return false;
    }
    s_track_running = true;
    xTaskNotifyGive(s_player_task);
    return true;
}

extern "C" void MUSIC_Stop(void)
{
    if (s_track_running) {
        s_stop_requested = true;
    }
}

extern "C" bool MUSIC_IsPlaying(void)
{
    return s_playing && !s_focus_suspended;
}

extern "C" const char *MUSIC_CurrentPath(void)
{
    return s_current_path;
}

extern "C" const MediaTrackInfo *MUSIC_GetTrackInfo(void)
{
    return &s_track_info;
}

extern "C" void MUSIC_ReleaseTrackCover(void)
{
    MEDIA_META_ReleaseCover(&s_track_info);
}

extern "C" void MUSIC_SetTrackEndCallback(MusicTrackEndCb_t callback)
{
    s_track_end_cb = callback;
}

extern "C" void MUSIC_SetTarget(const int target)
{
    if (target >= MUSIC_TARGET_LOCAL && target <= MUSIC_TARGET_BOTH &&
        target != s_target) {
        s_target = target;
        media_config_save();
    }
}

extern "C" int MUSIC_GetTarget(void)
{
    return s_target;
}

extern "C" void MUSIC_SetOutput(const int output)
{
    if (output >= MUSIC_OUTPUT_SPEAKER && output <= MUSIC_OUTPUT_BT &&
        output != s_output) {
        s_output = output;
        media_config_save();
    }
}

extern "C" int MUSIC_GetOutput(void)
{
    return s_output;
}

extern "C" bool MUSIC_SetRadioUrl(const char *url)
{
    if (url == nullptr || strlen(url) >= sizeof(s_radio_url)) {
        return false;
    }
    if (url[0] != '\0' &&
        strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    if (strcmp(url, s_radio_url) != 0) {
        snprintf(s_radio_url, sizeof(s_radio_url), "%s", url);
        media_config_save();
    }
    return true;
}

extern "C" void MUSIC_GetRadioUrl(char *out, const size_t out_size)
{
    if (out != nullptr && out_size > 0u) {
        snprintf(out, out_size, "%s", s_radio_url);
    }
}

extern "C" bool MUSIC_GetStreamInfo(uint32_t *sample_rate_hz,
                                    uint8_t *bits_per_sample,
                                    uint8_t *channels)
{
    if (!s_stream_info_valid) {
        return false;
    }
    if (sample_rate_hz != nullptr) {
        *sample_rate_hz = s_stream_info.sample_rate_hz;
    }
    if (bits_per_sample != nullptr) {
        *bits_per_sample = s_stream_info.bits_per_sample;
    }
    if (channels != nullptr) {
        *channels = s_stream_info.channels;
    }
    return true;
}
