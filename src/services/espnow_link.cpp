#include "services/espnow_link.h"

#include "audio/audio_router.h"
#include "driver/board_pins.h"
#include "driver/external_radio.h"
#include "driver/status_io.h"
#include "lib/nrl_audio_bridge.h"
#include "lib/nrl_g711.h"
#include "lib/nrl_psram.h"
#include "media/opus_voice.h"
#include "services/config_notify.h"

#include <esp_log.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ESPNOW";

namespace {

constexpr const char *kNvsNamespace = "espnow";

// Packet layout: magic(4) type(1) callsign(6) ssid(1) payload.
// Type 1 = G.711 A-law, type 8 = shared NRL Opus voice framing.
constexpr uint8_t kMagic[4] = {'N', 'R', 'L', 'E'};
constexpr uint8_t kTypeVoice = 1;
constexpr uint8_t kTypeOpusVoice = 8;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kVoiceBytes = 160; // 20 ms at 8 kHz
constexpr size_t kPacketBytes = kHeaderBytes + kVoiceBytes;
static_assert(kPacketBytes <= ESP_NOW_MAX_DATA_LEN, "packet exceeds ESP-NOW MTU");
constexpr size_t kEspNowPayloadMax = ESP_NOW_MAX_DATA_LEN - kHeaderBytes;
constexpr uint32_t kOpusFrameMs = 20u;
constexpr size_t kOpusFrameSamples = (OPUS_VOICE_SAMPLE_RATE / 1000u) * kOpusFrameMs;

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static volatile bool s_enabled = false;
static volatile bool s_rx_enabled = true;     // RX switch; defaults ON
static volatile uint8_t s_ptt_mode = 0;       // 0 = NRL, 1 = ESP-NOW, 2 = FMO
static volatile uint8_t s_f2_ptt_target = 0;  // BH4TDV-RF F2 key: 0 = FMO, 1 = ESP-NOW
static volatile uint8_t s_tx_codec = 0;       // 0 = G.711, 1 = Opus; independent of NRL
static bool s_espnow_inited = false;
static uint8_t s_tx_packet[kPacketBytes];
static size_t s_tx_fill = kHeaderBytes;
static int16_t s_opus_tx_stage[kOpusFrameSamples];
static size_t s_opus_tx_fill = 0u;
static OpusVoiceEnc *s_opus_enc = nullptr;
static OpusVoiceDec *s_opus_dec = nullptr;
static char s_last_peer[16] = {};
static volatile bool s_ptt_held = false;      // dedicated ESP-NOW hold-to-talk
static volatile int64_t s_last_rx_us = 0;     // last voice frame arrival
static volatile uint8_t s_last_rx_codec = 0u; // 0 = G.711, 1 = Opus

static void save_enabled(const bool enabled)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "en", enabled ? 1 : 0);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool load_enabled(void)
{
    nvs_handle_t nvs;
    uint8_t en = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "en", &en);
        nvs_close(nvs);
    }
    return en != 0;
}

static void save_ptt_mode(const uint8_t mode)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "ptt", mode <= 2u ? mode : 0u);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static uint8_t load_ptt_mode(void)
{
    nvs_handle_t nvs;
    uint8_t mode = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "ptt", &mode);
        nvs_close(nvs);
    }
    return mode <= 2u ? mode : 0u;
}

static void save_f2_ptt_target(const uint8_t target)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "f2ptt", target != 0u ? 1u : 0u);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static uint8_t load_f2_ptt_target(void)
{
    nvs_handle_t nvs;
    uint8_t target = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "f2ptt", &target);
        nvs_close(nvs);
    }
    return target != 0u ? 1u : 0u;
}

static void save_rx_enabled(const bool enabled)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "rx", enabled ? 1 : 0);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool load_rx_enabled(void)
{
    nvs_handle_t nvs;
    uint8_t en = 1; // default ON: intercom voice is heard unless opted out
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "rx", &en);
        nvs_close(nvs);
    }
    return en != 0;
}

static void save_tx_codec(const uint8_t codec)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "codec", codec <= 1u ? codec : 0u);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static uint8_t load_tx_codec(void)
{
    nvs_handle_t nvs;
    uint8_t codec = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "codec", &codec);
        nvs_close(nvs);
    }
    return codec <= 1u ? codec : 0u;
}

static void reset_tx_accumulators(void)
{
    s_tx_fill = kHeaderBytes;
    s_opus_tx_fill = 0u;
}

static bool espnow_keyed(void)
{
    return s_ptt_held || (s_ptt_mode == 1u && STATUS_IO_IsSqlActive());
}

static void send_g711_16k(const int16_t *samples, const size_t sample_count)
{
    for (size_t i = 0; i + 1u < sample_count; i += 2u) {
        const int32_t a = samples[i];
        const int32_t b = samples[i + 1u];
        s_tx_packet[s_tx_fill++] = NRL_G711_EncodeALaw(static_cast<int16_t>((a + b) / 2));
        if (s_tx_fill >= kPacketBytes) {
            s_tx_fill = kHeaderBytes;
            s_tx_packet[4] = kTypeVoice;
            (void)esp_now_send(kBroadcastMac, s_tx_packet, kPacketBytes);
        }
    }
}

static void send_opus_16k(const int16_t *samples, const size_t sample_count)
{
    if (s_opus_enc == nullptr) {
        s_opus_enc = OPUS_VOICE_EncOpen(kOpusFrameMs);
        if (s_opus_enc == nullptr) {
            return;
        }
    }
    for (size_t i = 0; i < sample_count; ++i) {
        s_opus_tx_stage[s_opus_tx_fill++] = samples[i];
        if (s_opus_tx_fill < kOpusFrameSamples) {
            continue;
        }
        s_opus_tx_fill = 0u;
        uint8_t frame[OPUS_VOICE_MAX_FRAME_BYTES];
        const int encoded = OPUS_VOICE_EncProcess(s_opus_enc, s_opus_tx_stage,
                                                  kOpusFrameSamples, frame, sizeof(frame));
        if (encoded <= 0 || static_cast<size_t>(encoded) > kEspNowPayloadMax) {
            continue;
        }
        uint8_t packet[ESP_NOW_MAX_DATA_LEN];
        memcpy(packet, s_tx_packet, kHeaderBytes);
        packet[4] = kTypeOpusVoice;
        memcpy(packet + kHeaderBytes, frame, static_cast<size_t>(encoded));
        (void)esp_now_send(kBroadcastMac, packet, kHeaderBytes + static_cast<size_t>(encoded));
    }
}

// Router sink: mic frames (16 kHz, router-converted) -> selected codec ->
// broadcast. Keying follows the configured PTT mode or the dedicated S31
// ESP-NOW touch PTT. Runs in the audio task.
static void espnow_sink_write(uint8_t /*source_id*/,
                              const int16_t *samples,
                              size_t sample_count,
                              void *)
{
    if (!s_enabled || !espnow_keyed()) {
        reset_tx_accumulators(); // drop partial packets on key release
        return;
    }

    // TX-side level heartbeat (1 Hz while keyed): reports the level of the mic
    // (or radio-line) audio as it reaches this sink -- the sender-side twin of
    // the RX heartbeat, to tell "capture is silent" from "send path loses it".
    static int64_t s_tx_log_us = 0;
    static uint32_t s_tx_samples = 0;
    static int32_t s_tx_peak = 0;
    static uint64_t s_tx_sum_sq = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        const int32_t mag = (samples[i] < 0) ? -static_cast<int32_t>(samples[i])
                                             : static_cast<int32_t>(samples[i]);
        if (mag > s_tx_peak) {
            s_tx_peak = mag;
        }
        s_tx_sum_sq += static_cast<uint64_t>(
            static_cast<int64_t>(samples[i]) * static_cast<int64_t>(samples[i]));
    }
    s_tx_samples += sample_count;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_tx_log_us >= 1000000) {
        const uint32_t rms = (s_tx_samples > 0)
            ? static_cast<uint32_t>(sqrt(static_cast<double>(s_tx_sum_sq) /
                                         static_cast<double>(s_tx_samples)))
            : 0u;
        ESP_LOGI(TAG, "tx %s samples=%lu peak=%ld rms=%lu%s",
                 (s_tx_codec == 1u) ? "Opus" : "G.711",
                 static_cast<unsigned long>(s_tx_samples),
                 static_cast<long>(s_tx_peak),
                 static_cast<unsigned long>(rms),
                 (s_tx_peak < 50) ? "  <-- mic/capture is SILENT" : "");
        s_tx_log_us = now_us;
        s_tx_samples = 0;
        s_tx_peak = 0;
        s_tx_sum_sq = 0;
    }

    if (s_tx_codec == 1u) {
        s_tx_fill = kHeaderBytes;
        send_opus_16k(samples, sample_count);
    } else {
        s_opus_tx_fill = 0u;
        send_g711_16k(samples, sample_count);
    }
}

// RX heartbeat stats, shared by the WiFi recv callback (arrival counting +
// G.711 inline decode) and the Opus decode task. Diagnostics only, so the
// unsynchronised cross-task updates are acceptable.
static int64_t s_rx_log_us = 0;
static uint32_t s_rx_frames = 0;
static uint32_t s_rx_samples = 0;
static int32_t s_rx_peak = 0;
static uint64_t s_rx_sum_sq = 0;

static void track_rx_level(const int16_t *pcm, const size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const int32_t mag = (pcm[i] < 0) ? -static_cast<int32_t>(pcm[i])
                                         : static_cast<int32_t>(pcm[i]);
        if (mag > s_rx_peak) {
            s_rx_peak = mag;
        }
        s_rx_sum_sq += static_cast<uint64_t>(
            static_cast<int64_t>(pcm[i]) * static_cast<int64_t>(pcm[i]));
    }
}

// Rate-limited RX heartbeat so "callsign shows but no audio" can be told
// apart on the serial log: frames arriving, codec, decoded samples pushed
// toward the speaker, and the audio level (a peak near 0 means the sender
// transmitted silence).
static void emit_rx_heartbeat(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_rx_log_us < 1000000) {
        return;
    }
    const uint32_t rms = (s_rx_samples > 0)
        ? static_cast<uint32_t>(sqrt(static_cast<double>(s_rx_sum_sq) /
                                     static_cast<double>(s_rx_samples)))
        : 0u;
    ESP_LOGI(TAG, "rx %s peer=%s frames=%lu samples=%lu peak=%ld rms=%lu -> speaker%s",
             s_last_rx_codec ? "Opus" : "G.711", s_last_peer,
             static_cast<unsigned long>(s_rx_frames),
             static_cast<unsigned long>(s_rx_samples),
             static_cast<long>(s_rx_peak),
             static_cast<unsigned long>(rms),
             (s_rx_peak < 50) ? "  <-- SILENT audio from sender" : "");
    s_rx_log_us = now_us;
    s_rx_frames = 0;
    s_rx_samples = 0;
    s_rx_peak = 0;
    s_rx_sum_sq = 0;
}

// Opus RX decode runs on its own task, NOT in the WiFi recv callback: libopus
// needs a deep stack (an inline decode overflows the ~6.5 KB WiFi task stack
// and reboots) and would stall WiFi for the duration. The callback enqueues
// the encoded payload; this task decodes and hands PCM to the router. The
// 32 KB stack lives in PSRAM, matching the audio tasks that run the encoder.
struct OpusRxPacket {
    uint16_t len;
    uint8_t payload[ESP_NOW_MAX_DATA_LEN];
};
constexpr size_t kOpusRxQueueLength = 16u; // 320 ms of 20 ms frames
NRL_PSRAM_BSS static uint8_t
    s_opus_rx_queue_storage[kOpusRxQueueLength * sizeof(OpusRxPacket)];
static StaticQueue_t s_opus_rx_queue_control;
static QueueHandle_t s_opus_rx_queue = nullptr;
static TaskHandle_t s_opus_rx_task = nullptr;

static void espnow_opus_rx_task(void *)
{
    OpusRxPacket pkt;
    for (;;) {
        if (xQueueReceive(s_opus_rx_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_opus_dec == nullptr) {
            s_opus_dec = OPUS_VOICE_DecOpen(kOpusFrameMs);
            if (s_opus_dec == nullptr) {
                ESP_LOGW(TAG, "opus decoder open failed; RX audio muted");
                continue;
            }
        }
        int16_t pcm[kOpusFrameSamples];
        const int decoded = OPUS_VOICE_DecProcess(s_opus_dec, pkt.payload, pkt.len,
                                                  pcm, kOpusFrameSamples);
        if (decoded > 0) {
            const size_t n = static_cast<size_t>(decoded);
            track_rx_level(pcm, n);
            s_rx_samples += n;
            AudioRouter_PushFrame(AUDIO_SRC_ESPNOW, OPUS_VOICE_SAMPLE_RATE, pcm, n);
        }
        emit_rx_heartbeat();
    }
}

// Receive callback (WiFi task context): stays cheap -- G.711 is a table
// lookup and decodes inline; Opus packets are queued to the decode task.
static void espnow_recv_cb(const esp_now_recv_info_t * /*info*/,
                           const uint8_t *data, int len)
{
    // RX has its own switch (default ON) and is independent of the TX enable:
    // incoming intercom voice is heard unless the user opted out.
    if (!s_rx_enabled || data == nullptr ||
        len <= static_cast<int>(kHeaderBytes) ||
        memcmp(data, kMagic, sizeof(kMagic)) != 0 ||
        (data[4] != kTypeVoice && data[4] != kTypeOpusVoice)) {
        return;
    }

    char callsign[7];
    memcpy(callsign, data + 5, 6);
    callsign[6] = '\0';
    for (int i = 5; i >= 0 && (callsign[i] == ' ' || callsign[i] == '\0'); --i) {
        callsign[i] = '\0';
    }
    snprintf(s_last_peer, sizeof(s_last_peer), "%s-%u", callsign,
             static_cast<unsigned>(data[11]));
    s_last_rx_us = esp_timer_get_time();
    s_last_rx_codec = (data[4] == kTypeOpusVoice) ? 1u : 0u;
    ++s_rx_frames;

    const size_t payload = static_cast<size_t>(len) - kHeaderBytes;
    if (data[4] == kTypeOpusVoice) {
        if (s_opus_rx_queue != nullptr) {
            OpusRxPacket pkt;
            pkt.len = static_cast<uint16_t>(payload);
            memcpy(pkt.payload, data + kHeaderBytes, payload);
            (void)xQueueSend(s_opus_rx_queue, &pkt, 0); // drop when backed up
        }
        return; // decode task reports the heartbeat for Opus streams
    }

    int16_t pcm[kVoiceBytes];
    const size_t n = (payload < kVoiceBytes) ? payload : kVoiceBytes;
    for (size_t i = 0; i < n; ++i) {
        pcm[i] = NRL_G711_DecodeALaw(data[kHeaderBytes + i]);
    }
    track_rx_level(pcm, n);
    s_rx_samples += n;
    AudioRouter_PushFrame(AUDIO_SRC_ESPNOW, 8000u, pcm, n);
    emit_rx_heartbeat();
}

static void fill_tx_header(void)
{
    memcpy(s_tx_packet, kMagic, sizeof(kMagic));
    s_tx_packet[4] = kTypeVoice;
    memset(s_tx_packet + 5, 0, 7);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config != nullptr) {
        // Fixed 6-byte callsign field, zero-padded by the memset above (no
        // NUL terminator on the wire).
        memcpy(s_tx_packet + 5, config->callsign, strnlen(config->callsign, 6));
        s_tx_packet[11] = config->callsign_ssid;
    }
}

static bool espnow_bring_up(void)
{
    if (s_espnow_inited) {
        return true;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        ESP_LOGW(TAG, "WiFi not started yet");
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return false;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
    peer.channel = 0; // follow the current WiFi channel
    peer.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        ESP_LOGE(TAG, "add broadcast peer failed");
        esp_now_deinit();
        return false;
    }
    if (esp_now_register_recv_cb(espnow_recv_cb) != ESP_OK) {
        ESP_LOGE(TAG, "register recv cb failed");
        esp_now_deinit();
        return false;
    }
    NRL_G711_Init();
    fill_tx_header();

    // Opus RX decode task + hand-off queue (see espnow_opus_rx_task). Created
    // once; without it Opus packets are dropped (G.711 keeps working inline).
    if (s_opus_rx_queue == nullptr) {
        s_opus_rx_queue = xQueueCreateStatic(kOpusRxQueueLength,
                                             sizeof(OpusRxPacket),
                                             s_opus_rx_queue_storage,
                                             &s_opus_rx_queue_control);
    }
    if (s_opus_rx_task == nullptr && s_opus_rx_queue != nullptr &&
        xTaskCreatePinnedToCoreWithCaps(espnow_opus_rx_task, "espnow_opus", 32768,
                                        nullptr, 9, &s_opus_rx_task, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_opus_rx_task = nullptr;
        ESP_LOGW(TAG, "opus rx task create failed; Opus RX disabled (G.711 OK)");
    }

    AudioRouter_RegisterSink(AUDIO_SINK_ESPNOW, OPUS_VOICE_SAMPLE_RATE, espnow_sink_write, nullptr);
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_ESPNOW, true);
    AudioRouter_SetRoute(AUDIO_SRC_BT_HFP_MIC, AUDIO_SINK_ESPNOW, true);
    AudioRouter_SetRoute(AUDIO_SRC_ESPNOW, AUDIO_SINK_SPEAKER, true);
    s_espnow_inited = true;
    ESP_LOGI(TAG, "voice link up (broadcast, G.711/type1 or Opus/type8)");
    return true;
}

} // namespace

namespace {
// One-shot deferred bring-up: the bridge task starts WiFi asynchronously after
// boot, so the always-on RX link (and the persisted TX enable state) waits for
// it here.
static void espnow_deferred_enable_task(void *)
{
    for (int i = 0; i < 60; ++i) { // give WiFi up to 2 minutes
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (espnow_bring_up()) {
            s_enabled = load_enabled();
            ESP_LOGI(TAG, "link up (RX always on, TX %s)", s_enabled ? "armed" : "off");
            break;
        }
    }
    vTaskDelete(nullptr);
}
} // namespace

extern "C" void ESPNOW_LINK_Init(void)
{
    s_ptt_mode = load_ptt_mode();
    s_f2_ptt_target = load_f2_ptt_target();
    s_rx_enabled = load_rx_enabled();
    // Restore the persisted TX codec; if boot-time RAM cannot hold the Opus
    // codecs, run this session as G.711 (NVS keeps the user's choice).
    s_tx_codec = load_tx_codec();
    if (s_tx_codec == 1u && !ESPNOW_LINK_PrewarmOpus()) {
        ESP_LOGW(TAG, "persisted Opus tx codec needs RAM that isn't available; "
                      "falling back to G.711 for this session");
        s_tx_codec = 0u;
    }
    // RX is always on: bring the link up regardless of the persisted enable
    // state -- the switch only arms TX (PTT re-targeting). WiFi starts
    // asynchronously, so fall back to the deferred retry task when it isn't
    // up yet.
    if (espnow_bring_up()) {
        s_enabled = load_enabled();
    } else if (xTaskCreatePinnedToCore(espnow_deferred_enable_task, "espnow_en",
                                       3072, nullptr, 2, nullptr, 0) != pdPASS) {
        ESP_LOGW(TAG, "deferred bring-up task failed; use AT+ESPNOW=ON");
    }
}

extern "C" bool ESPNOW_LINK_SetEnabled(const bool enabled)
{
    if (enabled) {
        if (!espnow_bring_up()) {
            return false;
        }
        fill_tx_header(); // refresh callsign in case config changed
        s_enabled = true;
    } else {
        s_enabled = false;
        s_ptt_held = false;
        reset_tx_accumulators();
    }
    // Link enable and physical PTT target are independent settings. Disarming
    // the link only falls back when the button currently targets ESP-NOW;
    // enabling it never steals an existing NRL or FMO selection.
    if (!enabled && s_ptt_mode == 1u) ESPNOW_LINK_SetPttMode(0u);
    save_enabled(enabled);
    CONFIG_NOTIFY_Bump();
    return true;
}

extern "C" bool ESPNOW_LINK_IsEnabled(void)
{
    return s_enabled;
}

extern "C" void ESPNOW_LINK_SetRxEnabled(const bool enabled)
{
    s_rx_enabled = enabled;
    save_rx_enabled(enabled);
    CONFIG_NOTIFY_Bump();
}

extern "C" bool ESPNOW_LINK_IsRxEnabled(void)
{
    return s_rx_enabled;
}

extern "C" bool ESPNOW_LINK_SetTxCodec(const uint8_t codec)
{
    if (codec > 1u) {
        return false;
    }
    if (codec == 1u && !ESPNOW_LINK_PrewarmOpus()) {
        // Roll back: stay on G.711 and free the unused TX encoder; the decoder
        // is kept (RX follows the remote side's codec). Nothing is persisted.
        s_tx_codec = 0u;
        ESPNOW_LINK_ReleaseOpusEncoder();
        ESP_LOGW(TAG, "opus tx switch rolled back to G.711 (allocation failed)");
        return false;
    }
    s_tx_codec = codec;
    s_opus_tx_fill = 0u;
    save_tx_codec(codec);
    CONFIG_NOTIFY_Bump();
    return true;
}

extern "C" uint8_t ESPNOW_LINK_GetTxCodec(void)
{
    return s_tx_codec;
}

extern "C" void ESPNOW_LINK_SetPttMode(const uint8_t mode)
{
    s_ptt_mode = mode <= 2u ? mode : 0u;
    if (s_ptt_mode != 1u) {
        s_ptt_held = false;
    }
    save_ptt_mode(s_ptt_mode);
    CONFIG_NOTIFY_Bump();
}

extern "C" uint8_t ESPNOW_LINK_GetPttMode(void)
{
    return s_ptt_mode;
}

extern "C" void ESPNOW_LINK_SetF2PttTarget(const uint8_t target)
{
    s_f2_ptt_target = target != 0u ? 1u : 0u;
    if (s_f2_ptt_target != 1u) {
        s_ptt_held = false;
    }
    save_f2_ptt_target(s_f2_ptt_target);
    CONFIG_NOTIFY_Bump();
}

extern "C" uint8_t ESPNOW_LINK_GetF2PttTarget(void)
{
    return s_f2_ptt_target;
}

extern "C" void ESPNOW_LINK_SetPtt(const bool held)
{
    s_ptt_held = held && s_enabled;
}

extern "C" bool ESPNOW_LINK_PttActive(void)
{
    return s_enabled && espnow_keyed();
}

extern "C" bool ESPNOW_LINK_IsReceiving(void)
{
    // "Receiving" = a voice frame arrived within the last half second. Not
    // gated on the enable switch: RX is always on (the switch only arms TX),
    // and the speaker playback lifecycle in the bridge keys off this.
    return s_last_rx_us != 0 &&
           (esp_timer_get_time() - s_last_rx_us) < 500000;
}

extern "C" void ESPNOW_LINK_GetLastPeer(char *out, const size_t out_size)
{
    if (out != nullptr && out_size > 0u) {
        snprintf(out, out_size, "%s", s_last_peer);
    }
}

extern "C" uint8_t ESPNOW_LINK_GetRxCodec(void)
{
    return s_last_rx_codec;
}

extern "C" bool ESPNOW_LINK_PrewarmOpus(void)
{
    if (s_opus_enc == nullptr) {
        s_opus_enc = OPUS_VOICE_EncOpen(kOpusFrameMs);
    }
    if (s_opus_dec == nullptr) {
        s_opus_dec = OPUS_VOICE_DecOpen(kOpusFrameMs);
    }
    if (s_opus_enc == nullptr || s_opus_dec == nullptr) {
        ESP_LOGW(TAG, "opus prewarm failed (enc=%d dec=%d)",
                 s_opus_enc != nullptr, s_opus_dec != nullptr);
        return false;
    }
    return true;
}

extern "C" void ESPNOW_LINK_ReleaseOpusEncoder(void)
{
    if (s_opus_enc != nullptr) {
        OPUS_VOICE_EncClose(s_opus_enc);
        s_opus_enc = nullptr;
    }
    s_opus_tx_fill = 0u;
}
