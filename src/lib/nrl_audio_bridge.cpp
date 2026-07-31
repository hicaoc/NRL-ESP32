#include "nrl_audio_bridge.h"

#include "improv_protocol.h"
#include "nrl_at_commands.h"
#include "nrl_audio_config.h"
#include "nrl_bt_hfp.h"
#include "nrl_ethernet.h"
#include "nrl_g711.h"
#include "nrl_net_compat.h"
#include "nrl_text_message.h"
#include "nrl_usb_console.h"
#include "nrl_wifi.h"
#include "wifi_config_portal.h"

#include "audio/audio_focus.h"
#include "audio/audio_router.h"
#include "media/opus_voice.h"
#include "driver/audio_passthrough.h"
#include "driver/board_pins.h"
#include "driver/es8311.h"
#include "driver/external_radio.h"
#include "driver/sci_serial.h"
#include "driver/status_io.h"
#include "services/espnow_link.h"
#include "services/display_notice.h"
#include "services/signaling_service.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "NRL";

namespace {

constexpr uint8_t kNrlHeaderSize = 48u;
constexpr uint8_t kNrlTypeVoice = 1u;
constexpr uint8_t kNrlTypeTextMessage = 5u;
constexpr uint8_t kNrlTypeOpusVoice = 8u;  // wideband voice: one 16k/20ms Opus frame per packet
constexpr uint8_t kNrlTypeVideo = 13u;     // video-call JPEG fragments (services/video_call)
constexpr uint8_t kNrlTypeHeartbeat = 2u;
constexpr uint8_t kNrlTypeServerHeartbeat = 3u;
constexpr uint8_t kNrlTypeServerVoice = 9u;
constexpr uint8_t kNrlTypeAtCommand = 11u;
constexpr uint8_t kNrlTypeSciTransparent = 12u;
constexpr size_t kAudioCodecFrameSamples = 80u;
// Minimum / maximum G.711 A-law payload bytes per outbound voice packet. The
// runtime value comes from ExternalRadioConfig::voice_payload_bytes and is
// clamped into this range; the accumulator and stack packet buffer below are
// sized at the max so the threshold can be changed at runtime without realloc.
constexpr size_t kG711PayloadMinBytes = 160u;
constexpr size_t kG711PayloadMaxBytes = 500u;
constexpr size_t kG711PayloadDefaultBytes = kG711PayloadMinBytes;
constexpr size_t kG711RxPayloadMaxBytes = 500u;
constexpr size_t kSciPayloadMaxBytes = 256u;
constexpr size_t kNrlMaxPayloadSize = 1024u;
constexpr size_t kNrlMaxPacketSize = kNrlHeaderSize + kNrlMaxPayloadSize;
constexpr uint32_t kTextMessageNoticeDurationMs = 15000u;
constexpr int kDownlinkPcmGain = 1;
constexpr uint32_t kSciPollIntervalMs = 20u;
constexpr uint32_t kSciFlushIntervalMs = 20u;

int s_udp_fd = -1;
SemaphoreHandle_t s_udp_mutex = nullptr;
TaskHandle_t s_bridge_task = nullptr;
bool s_bridge_initialized = false;
bool s_udp_started = false;
bool s_downlink_playback_active = false;
bool s_voice_stream_logged = false;
bool s_voice_decode_logged = false;
bool s_reboot_pending = false;
int16_t s_voice_decode_peak_max = 0;
uint32_t s_voice_decode_sample_total = 0u;
uint32_t s_voice_decode_chunks = 0u;
uint16_t s_packet_counter = 0u;
uint32_t s_last_rx_packet_ms = 0u;
// Codec of the most recently decoded downlink voice packet, for the LCD:
// 0 = G.711 (packet type 1/9), 1 = Opus (type 8). Latched per packet.
volatile uint8_t s_last_rx_codec = 0u;
uint32_t s_last_heartbeat_ms = 0u;
uint32_t s_last_remote_identity_ms = 0u;
uint32_t s_reboot_at_ms = 0u;
uint32_t s_server_ip = 0u;
uint32_t s_last_peer_ip = 0u;
uint16_t s_last_peer_port = 0u;
uint8_t s_cpu_id[4] = {};
char s_remote_callsign[7] = {};
uint8_t s_remote_ssid = 0u;
uint32_t s_remote_dmr_id = 0u;
// Identity of the last *voice* caller specifically. s_remote_callsign above
// tracks every packet (heartbeat, AT, SCI...), so it must not be used to
// decide "who is talking" or "is a voice stream active".
char s_voice_callsign[7] = {};
uint8_t s_voice_ssid = 0u;
uint32_t s_voice_dmr_id = 0u;
size_t s_last_voice_payload_size = 0u;
uint32_t s_next_wifi_retry_ms = 0u;
uint8_t s_wifi_connect_failures = 0u;
uint8_t s_rx_packet_buffer[kNrlMaxPacketSize];
uint8_t s_at_reply_packet[kNrlMaxPacketSize];
uint8_t s_sci_tx_packet[kNrlHeaderSize + kSciPayloadMaxBytes];
uint8_t s_sci_payload_buffer[kSciPayloadMaxBytes];
char s_text_message_log[kNrlMaxPayloadSize + 1u];
int16_t s_downlink_pcm_buffer[kG711RxPayloadMaxBytes];
size_t s_sci_payload_count = 0u;
uint32_t s_last_sci_rx_ms = 0u;
uint32_t s_last_sci_log_ms = 0u;
uint32_t s_last_sci_poll_ms = 0u;
// Tail-audio suppression: when a downlink (network->radio) voice stream ends,
// this is set to the wall-clock time until which captured radio audio is
// dropped instead of forwarded to the network. 0 means no window pending. It
// is written by the bridge task (stopDownlinkPlayback) and read by the audio
// task (sendVoiceFrame); a uint32_t word read/write is atomic on the ESP32, so
// no lock is needed and an occasional stale read is harmless.
uint32_t s_tail_suppress_until_ms = 0u;

constexpr uint32_t kWifiRetryBackoffMs = 30000u;
constexpr uint8_t kWifiFallbackFailureThreshold = 3u;
constexpr size_t kAtLogTextMax = 96u;

static void initCpuId(void)
{
    memset(s_cpu_id, 0, sizeof(s_cpu_id));
}

static void formatAtLogText(const uint8_t *payload,
                            const size_t payload_size,
                            char *out,
                            const size_t out_size)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    out[0] = '\0';
    if (payload == nullptr || payload_size <= 1u) {
        return;
    }

    const size_t text_size = payload_size - 1u;
    const size_t copy_size = (text_size < (out_size - 1u)) ? text_size : (out_size - 1u);
    for (size_t i = 0; i < copy_size; ++i) {
        const uint8_t ch = payload[i + 1u];
        out[i] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : '.';
    }
    out[copy_size] = '\0';
}

static void updateRemoteIdentity(const uint8_t *packet, const size_t packet_size)
{
    if (packet == nullptr || packet_size < 31u) {
        return;
    }

    memcpy(s_remote_callsign, packet + 24u, 6u);
    s_remote_callsign[6] = '\0';
    for (int i = 5; i >= 0; --i) {
        if (s_remote_callsign[i] == '\0' || s_remote_callsign[i] == '\r' || s_remote_callsign[i] == ' ') {
            s_remote_callsign[i] = '\0';
        } else {
            break;
        }
    }
    s_remote_ssid = packet[30];
    s_remote_dmr_id = (static_cast<uint32_t>(packet[6]) << 16u) |
                      (static_cast<uint32_t>(packet[7]) << 8u) |
                      static_cast<uint32_t>(packet[8]);
    s_last_remote_identity_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int16_t applyPcmGain(const int16_t sample, const int gain)
{
    int32_t value = static_cast<int32_t>(sample) * gain;
    if (value > INT16_MAX) {
        value = INT16_MAX;
    } else if (value < INT16_MIN) {
        value = INT16_MIN;
    }
    return static_cast<int16_t>(value);
}

static size_t buildNrlPacket(const uint8_t packet_type,
                             const uint8_t *payload,
                             const size_t payload_size,
                             uint8_t *out_packet,
                             const size_t out_capacity)
{
    const size_t total_size = kNrlHeaderSize + payload_size;
    if (out_packet == nullptr || total_size > out_capacity) {
        return 0;
    }

    memset(out_packet, 0, total_size);
    memcpy(out_packet, "NRL2", 4u);
    out_packet[4] = static_cast<uint8_t>((total_size >> 8) & 0xFFu);
    out_packet[5] = static_cast<uint8_t>(total_size & 0xFFu);
    memcpy(out_packet + 6u, s_cpu_id, sizeof(s_cpu_id));
    out_packet[20] = packet_type;
    out_packet[21] = 1u;
    ++s_packet_counter;
    out_packet[22] = static_cast<uint8_t>((s_packet_counter >> 8) & 0xFFu);
    out_packet[23] = static_cast<uint8_t>(s_packet_counter & 0xFFu);

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const char *callsign = (config != nullptr) ? config->callsign : NRL_AUDIO_CALLSIGN;
    const uint8_t callsign_ssid = (config != nullptr) ? config->callsign_ssid : static_cast<uint8_t>(NRL_AUDIO_CALLSIGN_SSID);
    const uint8_t device_mode = (config != nullptr) ? config->device_mode : static_cast<uint8_t>(NRL_AUDIO_DEVICE_MODE);
    // Fixed 6-byte callsign field, zero-padded by the memset above (no NUL
    // terminator on the wire).
    memcpy(out_packet + 24u, callsign, strnlen(callsign, 6u));
    out_packet[30] = callsign_ssid;
    out_packet[31] = device_mode;

    if (payload_size > 0u) {
        memcpy(out_packet + kNrlHeaderSize, payload, payload_size);
    }

    return total_size;
}

static bool parseNrlPacket(const uint8_t *packet,
                           const size_t packet_size,
                           uint8_t *out_type,
                           const uint8_t **out_payload,
                           size_t *out_payload_size)
{
    if (packet == nullptr || packet_size < kNrlHeaderSize) {
        return false;
    }
    if (memcmp(packet, "NRL2", 4u) != 0) {
        return false;
    }

    uint16_t total_size = static_cast<uint16_t>((packet[4] << 8) | packet[5]);
    if (total_size > packet_size || total_size < kNrlHeaderSize) {
        return false;
    }

    // Some NRL senders keep the header length fixed at 48 even when a payload
    // is present. In that case, trust the UDP datagram length so voice/AT
    // payloads are not discarded.
    if (total_size == kNrlHeaderSize && packet_size > kNrlHeaderSize) {
        total_size = static_cast<uint16_t>(packet_size);
    }

    if (out_type != nullptr) {
        *out_type = packet[20];
    }
    if (out_payload != nullptr) {
        *out_payload = packet + kNrlHeaderSize;
    }
    if (out_payload_size != nullptr) {
        *out_payload_size = total_size - kNrlHeaderSize;
    }
    return true;
}

static bool udpSendPacketTo(const uint8_t *packet,
                            const size_t packet_size,
                            const uint32_t dest_ip,
                            const uint16_t dest_port)
{
    if (packet == nullptr || packet_size == 0u || !s_udp_started || s_udp_fd < 0 ||
        !nrlNetworkConnected() || dest_port == 0u || dest_ip == 0u) {
        return false;
    }
    if (s_udp_mutex == nullptr) {
        return false;
    }
    // Wait up to 5 ms for the mutex instead of returning false immediately.
    // The bridge task's recvfrom() holds this mutex for microseconds at a
    // time, but a non-blocking try-take meant occasional voice packets were
    // silently dropped under contention.
    if (xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dest_port);
    dest.sin_addr.s_addr = dest_ip;
    const ssize_t sent = sendto(s_udp_fd, packet, packet_size, 0,
                                reinterpret_cast<struct sockaddr *>(&dest),
                                sizeof(dest));
    const bool ok = (sent == static_cast<ssize_t>(packet_size));
    xSemaphoreGive(s_udp_mutex);
    return ok;
}

static bool udpSendPacket(const uint8_t *packet, const size_t packet_size)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const uint16_t server_port = (config != nullptr) ? config->server_port : static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    return udpSendPacketTo(packet, packet_size, s_server_ip, server_port);
}

static void flushSciPayload(void)
{
    if (s_sci_payload_count == 0u) {
        return;
    }

    const size_t packet_size = buildNrlPacket(kNrlTypeSciTransparent,
                                              s_sci_payload_buffer,
                                              s_sci_payload_count,
                                              s_sci_tx_packet,
                                              sizeof(s_sci_tx_packet));
    const size_t payload_count = s_sci_payload_count;
    s_sci_payload_count = 0u;
    if (packet_size == 0u) {
        return;
    }

    const bool sent = udpSendPacket(s_sci_tx_packet, packet_size);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (!sent && (now - s_last_sci_log_ms) >= 1000u) {
        s_last_sci_log_ms = now;
        ESP_LOGI(TAG,"[SCI] uplink send failed bytes=%u\n",
                      static_cast<unsigned>(payload_count));
    }
}

static void pollSciUplink(void)
{
    int available = SCI_SERIAL_Available();
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    while (available > 0) {
        const size_t space = sizeof(s_sci_payload_buffer) - s_sci_payload_count;
        if (space == 0u) {
            flushSciPayload();
            continue;
        }

        const size_t read = SCI_SERIAL_Read(s_sci_payload_buffer + s_sci_payload_count, space);
        if (read == 0u) {
            break;
        }
        s_sci_payload_count += read;
        s_last_sci_rx_ms = now;
        if (s_sci_payload_count == sizeof(s_sci_payload_buffer)) {
            flushSciPayload();
        }
        available = SCI_SERIAL_Available();
    }

    if (s_sci_payload_count > 0u &&
        (now - s_last_sci_rx_ms) >= kSciFlushIntervalMs) {
        flushSciPayload();
    }
}

static void handleIncomingSciPayload(const uint8_t *payload, const size_t payload_size)
{
    if (payload == nullptr || payload_size == 0u || payload_size > kNrlMaxPayloadSize) {
        return;
    }

    const size_t written = SCI_SERIAL_Write(payload, payload_size);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (written != payload_size && (now - s_last_sci_log_ms) >= 1000u) {
        s_last_sci_log_ms = now;
        ESP_LOGI(TAG,"[SCI] downlink short write bytes=%u written=%u\n",
                      static_cast<unsigned>(payload_size),
                      static_cast<unsigned>(written));
    }
}

// The fixed NRL_AUDIO_RX_PACKET_TIMEOUT_MS (default 120 ms) was tuned for
// ~20 ms G.711 frames. Voice senders configured for larger payloads (up to
// 500 bytes = 62.5 ms) exceed it after just one or two packets of jitter,
// which would call stopDownlinkPlayback() and flush the DAC queue mid-talk.
// Scale to 4× the last observed frame duration so a few packets of jitter
// don't tear the playback.
static uint32_t currentRxPacketTimeoutMs(void)
{
    const uint32_t base_ms = static_cast<uint32_t>(NRL_AUDIO_RX_PACKET_TIMEOUT_MS);
    if (s_last_voice_payload_size == 0u) {
        return base_ms;
    }
    // G.711 8 kHz A-law: 8 bytes per millisecond of audio.
    const uint32_t frame_ms = static_cast<uint32_t>(s_last_voice_payload_size) / 8u;
    const uint32_t scaled = frame_ms * 4u;
    return (scaled > base_ms) ? scaled : base_ms;
}

static size_t currentVoicePayloadBytes(void)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const size_t configured = (config != nullptr)
                                  ? static_cast<size_t>(config->voice_payload_bytes)
                                  : kG711PayloadDefaultBytes;
    if (configured < kG711PayloadMinBytes) {
        return kG711PayloadDefaultBytes;
    }
    if (configured > kG711PayloadMaxBytes) {
        return kG711PayloadMaxBytes;
    }
    return configured;
}

// Video-fragment RX hook (NRL packet type 13); set by services/video_call.
static volatile NrlVideoRxHandler_t s_video_rx_handler = nullptr;

// True while a media stream (nanny music / beacon) owns the network uplink;
// mic frames are dropped for the duration so the G.711 accumulator carries
// exactly one producer at a time.
static volatile bool s_media_uplink_active = false;

// Captured-audio gate shared by the G.711 and Opus uplink paths: transmit
// only while the radio squelch is open and outside the tail-suppression
// window (see stopDownlinkPlayback).
static bool uplinkGateOpen(void)
{
    if (ESPNOW_LINK_GetPttMode() == 1u) {
        return false;
    }
    if (!STATUS_IO_IsSqlActive()) {
        return false;
    }
    // Tail-audio suppression: for a configured window after the device
    // finishes playing a network voice stream out to the radio, drop
    // captured radio audio so a repeater's echo response is not bounced
    // back onto the network (which would mutually re-trigger two or more
    // networked devices). Media uplink is not radio echo, so ungated.
    if (s_tail_suppress_until_ms != 0u) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (static_cast<int32_t>(now - s_tail_suppress_until_ms) < 0) {
            return false;
        }
        s_tail_suppress_until_ms = 0u;
    }
    return true;
}

static uint8_t alaw_accumulator[kG711PayloadMaxBytes];
static size_t alaw_accumulator_count = 0u;

static void sendVoiceFrameInternal(const int16_t *pcm8k, const size_t sample_count, const bool gated)
{
    if (pcm8k == nullptr || sample_count == 0u) {
        return;
    }

    if (gated && !uplinkGateOpen()) {
        alaw_accumulator_count = 0u; // drop stale partial frame on squelch close
        return;
    }

    const size_t target_bytes = currentVoicePayloadBytes();
    // If the configured size shrunk while we already had more accumulated,
    // drop the stale tail so we start clean at the new threshold.
    if (alaw_accumulator_count > target_bytes) {
        alaw_accumulator_count = 0u;
    }

    const size_t used_samples = sample_count;
    for (size_t i = 0; i < used_samples; ++i) {
        if (alaw_accumulator_count < target_bytes) {
            alaw_accumulator[alaw_accumulator_count++] = NRL_G711_EncodeALaw(pcm8k[i]);
        }

        if (alaw_accumulator_count >= target_bytes) {
            uint8_t packet[kNrlHeaderSize + kG711PayloadMaxBytes];
            const size_t packet_size = buildNrlPacket(kNrlTypeVoice,
                                                      alaw_accumulator,
                                                      target_bytes,
                                                      packet,
                                                      sizeof(packet));
            if (packet_size > 0u) {
                udpSendPacket(packet, packet_size);
            }
            alaw_accumulator_count = 0u;
        }
    }
}

static void sendVoiceFrame(const int16_t *pcm8k, const size_t sample_count)
{
    sendVoiceFrameInternal(pcm8k, sample_count, true);
}

// ---- Wideband voice (NRL packet type 8, shared Opus codec) -----------------
// TX codec selection: 0 = G.711 8k (type 1, default), 1 = Opus 16k (type 8).
// Persisted in NVS namespace "voice". RX always accepts both types.
constexpr const char *kVoiceNvsNamespace = "voice";
constexpr uint32_t kOpusFrameMs = 20u;
constexpr size_t kOpusFrameSamples = (OPUS_VOICE_SAMPLE_RATE / 1000u) * kOpusFrameMs; // 320
volatile uint8_t s_voice_codec = 0u;
OpusVoiceEnc *s_opus_enc = nullptr;
OpusVoiceDec *s_opus_dec = nullptr;
int16_t s_opus_tx_stage[kOpusFrameSamples];
size_t s_opus_tx_fill = 0u;

static void sendOpusVoice16k(const int16_t *pcm16k, const size_t sample_count, const bool gated)
{
    if (gated && !uplinkGateOpen()) {
        s_opus_tx_fill = 0u; // drop the partial frame on squelch close
        return;
    }
    if (s_opus_enc == nullptr) {
        s_opus_enc = OPUS_VOICE_EncOpen(kOpusFrameMs);
        if (s_opus_enc == nullptr) {
            return;
        }
    }
    for (size_t i = 0; i < sample_count; ++i) {
        s_opus_tx_stage[s_opus_tx_fill++] = pcm16k[i];
        if (s_opus_tx_fill < kOpusFrameSamples) {
            continue;
        }
        s_opus_tx_fill = 0u;
        uint8_t frame[OPUS_VOICE_MAX_FRAME_BYTES];
        const int encoded = OPUS_VOICE_EncProcess(s_opus_enc, s_opus_tx_stage,
                                                  kOpusFrameSamples, frame, sizeof(frame));
        if (encoded <= 0) {
            continue;
        }
        uint8_t packet[kNrlHeaderSize + OPUS_VOICE_MAX_FRAME_BYTES];
        const size_t packet_size = buildNrlPacket(kNrlTypeOpusVoice, frame,
                                                  static_cast<size_t>(encoded),
                                                  packet, sizeof(packet));
        if (packet_size > 0u) {
            udpSendPacket(packet, packet_size);
        }
    }
}

// Router sink for the NRL uplink, registered at 16 kHz (the voice domain's
// native rate): raw mic frames arrive untouched; 8 kHz producers (AFE, BT
// SCO) are upsampled by the router. In G.711 mode the pair-average
// downsample happens here -- the same conversion the router used to do --
// so the narrowband path sounds as before; in Opus mode the full 16 kHz
// bandwidth reaches the encoder.
static void uplinkSinkWrite(const uint8_t source_id,
                            const int16_t *samples,
                            const size_t sample_count,
                            void *)
{
    const bool signaling_tail = source_id == AUDIO_SRC_MDC_NRL ||
                                source_id == AUDIO_SRC_DTMF_NRL ||
                                source_id == AUDIO_SRC_CW_NRL;
    // A media stream (nanny/beacon) owns the uplink exclusively while
    // active: captured audio would garble the G.711 accumulator.
    if (s_media_uplink_active) {
        return;
    }
    // When a Bluetooth headset is connected, its mic is the uplink source;
    // ignore the onboard mic so audio isn't double-captured.
    if (source_id == AUDIO_SRC_MIC && NRL_BtHfp_IsConnected()) {
        return;
    }

    // TX-side level heartbeat (1 Hz while the uplink gate is open): the NRL
    // twin of the ESPNOW tx log, so mic levels reaching the two sinks can be
    // compared A/B on the same board with one firmware.
    if (uplinkGateOpen()) {
        static uint32_t s_ul_log_ms = 0u;
        static uint32_t s_ul_samples = 0u;
        static int32_t s_ul_peak = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            const int32_t mag = (samples[i] < 0) ? -static_cast<int32_t>(samples[i])
                                                 : static_cast<int32_t>(samples[i]);
            if (mag > s_ul_peak) {
                s_ul_peak = mag;
            }
        }
        s_ul_samples += sample_count;
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now - s_ul_log_ms >= 1000u) {
            ESP_LOGI(TAG, "[NRL] tx src=%u samples=%lu peak=%ld%s",
                     static_cast<unsigned>(source_id),
                     static_cast<unsigned long>(s_ul_samples),
                     static_cast<long>(s_ul_peak),
                     (s_ul_peak < 50) ? "  <-- mic/capture is SILENT" : "");
            s_ul_log_ms = now;
            s_ul_samples = 0u;
            s_ul_peak = 0;
        }
    }

    if (s_voice_codec == 1u) {
        sendOpusVoice16k(samples, sample_count, !signaling_tail);
        return;
    }

    // G.711: 16k -> 8k pair average, sliced to bound the stack buffer.
    int16_t pcm8k[160];
    size_t offset = 0;
    while (offset < sample_count) {
        const size_t take = ((sample_count - offset) < 320u) ? (sample_count - offset) : 320u;
        const size_t pairs = take / 2u;
        for (size_t i = 0; i < pairs; ++i) {
            const int32_t a = samples[offset + i * 2u];
            const int32_t b = samples[offset + i * 2u + 1u];
            pcm8k[i] = static_cast<int16_t>((a + b) / 2);
        }
        if (pairs > 0u) {
            if (signaling_tail) {
                sendVoiceFrameInternal(pcm8k, pairs, false);
            } else {
                sendVoiceFrame(pcm8k, pairs);
            }
        }
        offset += take;
    }
}

// Router sink for the Bluetooth headset speaker (8 kHz SCO). Pushing while
// merely connected (not just while SCO is already up) lets the buffer fill
// and signals nrl_bt_hfp to open the SCO on demand.
static void btHfpSinkWrite(const uint8_t /*source_id*/,
                           const int16_t *samples,
                           const size_t sample_count,
                           void *)
{
    NRL_BtHfp_PushPlayback(samples, sample_count);
}

static WifiConnResult connectSavedWifi(const ExternalRadioConfig *config)
{
    if (config == nullptr) {
        return wifiEnsureConnected(NRL_AUDIO_WIFI_SSID, NRL_AUDIO_WIFI_PASSWORD, 10000u);
    }

    const size_t count = EXTERNAL_RADIO_GetWifiProfileCount();
    if (count == 0U) return WIFI_CONN_FAILED;

    // Scan once so unavailable profiles do not each consume a full connection
    // timeout. Saved order is the priority; hidden/not-seen profiles are still
    // attempted afterward so hidden SSIDs remain supported.
    NrlWifiScanResult scanned[32] = {};
    const bool scan_ok = nrlWifiScanStartBlocking(5000u);
    const size_t scanned_count = scan_ok ? nrlWifiScanGetCache(scanned, 32U) : 0U;
    bool attempted[EXTERNAL_RADIO_MAX_WIFI_PROFILES] = {};
    WifiConnResult last = WIFI_CONN_FAILED;

    for (size_t i = 0; i < count; ++i) {
        bool visible = !scan_ok;
        for (size_t j = 0; j < scanned_count; ++j) {
            if (strcmp(config->wifi_profiles[i].ssid, scanned[j].ssid) == 0) {
                visible = true;
                break;
            }
        }
        if (!visible) continue;
        attempted[i] = true;
        ESP_LOGI(TAG, "trying saved Wi-Fi %u/%u: \"%s\"",
                 static_cast<unsigned>(i + 1U), static_cast<unsigned>(count),
                 config->wifi_profiles[i].ssid);
        last = wifiEnsureConnected(config->wifi_profiles[i].ssid,
                                   config->wifi_profiles[i].password, 10000u);
        if (last == WIFI_CONN_OK || last == WIFI_ALREADY_ON_SSID) return last;
    }
    for (size_t i = 0; i < count; ++i) {
        if (attempted[i]) continue;
        ESP_LOGI(TAG, "trying hidden/unseen saved Wi-Fi %u/%u: \"%s\"",
                 static_cast<unsigned>(i + 1U), static_cast<unsigned>(count),
                 config->wifi_profiles[i].ssid);
        last = wifiEnsureConnected(config->wifi_profiles[i].ssid,
                                   config->wifi_profiles[i].password, 10000u);
        if (last == WIFI_CONN_OK || last == WIFI_ALREADY_ON_SSID) return last;
    }
    return last;
}

static bool ensureNetworkAndUdp(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_next_wifi_retry_ms != 0u &&
        static_cast<int32_t>(now - s_next_wifi_retry_ms) < 0) {
        return false;
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    // Master Wi-Fi switch only controls the radio. Wired Ethernet remains a
    // valid NRL transport, allowing A2DP/HFP to run without 2.4 GHz contention.
    static bool s_wifi_master_off_applied = false;
    if (config != nullptr && !config->wifi_enabled) {
        if (!s_wifi_master_off_applied) {
            s_wifi_master_off_applied = true;
            // Full stop (not just disconnect): frees the shared radio AND WiFi's
            // RAM, so Bluetooth A2DP can register + stream music to the headset.
            nrlWifiStopRadio();
            ESP_LOGI(TAG, "[NRL] Wi-Fi master switch OFF -- radio+RAM freed for Bluetooth/A2DP");
        }
        if (!nrlNetworkConnected()) {
            return false;
        }
    }
    if (s_wifi_master_off_applied) {
        s_wifi_master_off_applied = false;
        s_next_wifi_retry_ms = 0u;  // re-enabled: reconnect promptly
        ESP_LOGI(TAG, "[NRL] Wi-Fi master switch ON -- reconnecting");
    }
    const char *server_host = (config != nullptr) ? config->server_host : NRL_AUDIO_SERVER_HOST;
    const uint16_t server_port = (config != nullptr) ? config->server_port : static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    const uint16_t local_port = (config != nullptr) ? config->local_port : server_port;

    // Ethernet is preferred. If its physical link is already up, allow DHCP to
    // finish instead of immediately bringing up Wi-Fi and stealing the route.
    if (!nrlNetworkConnected() && nrlEthernetLinkUp()) {
        return false;
    }
    if (!nrlNetworkConnected()) {
        const WifiConnResult result = connectSavedWifi(config);
        if (result != WIFI_CONN_OK && result != WIFI_ALREADY_ON_SSID) {
            if (s_wifi_connect_failures < 0xFFu) {
                ++s_wifi_connect_failures;
            }
            s_next_wifi_retry_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) + kWifiRetryBackoffMs;
            if (s_wifi_connect_failures >= kWifiFallbackFailureThreshold) {
                WifiConfigPortal_EnterFallbackMode();
                ESP_LOGI(TAG,"[NRL] wifi connect failed %u times, enter config AP and retry after %lu ms\n",
                              static_cast<unsigned>(s_wifi_connect_failures),
                              static_cast<unsigned long>(kWifiRetryBackoffMs));
            } else {
                ESP_LOGI(TAG,"[NRL] wifi connect failed (%u/%u), retry after %lu ms\n",
                              static_cast<unsigned>(s_wifi_connect_failures),
                              static_cast<unsigned>(kWifiFallbackFailureThreshold),
                              static_cast<unsigned long>(kWifiRetryBackoffMs));
            }
            return false;
        }
        s_next_wifi_retry_ms = 0u;
        s_wifi_connect_failures = 0u;
    }
    // Recreate the socket if the preferred interface changed (for example,
    // Ethernet became available after the Wi-Fi fallback was already active).
    static uint32_t s_udp_network_ip = 0u;
    const uint32_t active_network_ip = nrlNetworkIp();
    if (s_udp_started && active_network_ip != 0u && active_network_ip != s_udp_network_ip) {
        close(s_udp_fd);
        s_udp_fd = -1;
        s_udp_started = false;
        ESP_LOGI(TAG, "network interface changed, reopening UDP socket");
    }
    if (!s_udp_started) {
        struct in_addr literal = {};
        if (inet_aton(server_host, &literal) != 0) {
            s_server_ip = literal.s_addr;
        } else {
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            struct addrinfo *res = nullptr;
            if (getaddrinfo(server_host, nullptr, &hints, &res) != 0 || res == nullptr) {
                ESP_LOGE(TAG, "resolve host failed: %s", server_host);
                if (res != nullptr) {
                    freeaddrinfo(res);
                }
                return false;
            }
            const struct sockaddr_in *sa = reinterpret_cast<const struct sockaddr_in *>(res->ai_addr);
            s_server_ip = sa->sin_addr.s_addr;
            freeaddrinfo(res);
        }

        if (s_udp_fd >= 0) {
            close(s_udp_fd);
            s_udp_fd = -1;
        }
        s_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp_fd < 0) {
            ESP_LOGE(TAG, "udp socket() failed");
            return false;
        }
        struct sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(local_port);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s_udp_fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) < 0) {
            ESP_LOGE(TAG, "udp bind(%u) failed", static_cast<unsigned>(local_port));
            close(s_udp_fd);
            s_udp_fd = -1;
            return false;
        }
        const int flags = fcntl(s_udp_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(s_udp_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGE(TAG, "udp set O_NONBLOCK failed");
            close(s_udp_fd);
            s_udp_fd = -1;
            return false;
        }
        // Mark every packet on this socket with DSCP CS6 (=48, TOS byte 0xC0).
        // 802.11e WMM maps DSCP CS6/CS7 to AC_VO (voice access category);
        // DSCP EF (46, 0xB8) which we previously set actually maps to AC_VI
        // (video). AC_VO has the lowest CW and shortest AIFS, so the MAC
        // contends for the channel sooner and is less likely to bunch our
        // outgoing voice packets with each other or with other traffic.
        const int tos = 0xC0;
        if (setsockopt(s_udp_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
            ESP_LOGW(TAG, "udp set IP_TOS=0xC0 (DSCP CS6 / AC_VO) failed (continuing anyway)");
        }
        s_udp_started = true;
        s_udp_network_ip = active_network_ip;
        char ip_buf[16] = {};
        nrlIpToString(s_server_ip, ip_buf, sizeof(ip_buf));
        ESP_LOGI(TAG, "udp local=%u remote=%s(%s):%u",
                 static_cast<unsigned>(local_port),
                 server_host,
                 ip_buf,
                 static_cast<unsigned>(server_port));
    }

    return true;
}

static void startDownlinkPlayback(void)
{
    if (s_downlink_playback_active) {
        return;
    }

    // Voice interrupts media playback (music yields the hi-fi speaker and
    // the voice passthrough restarts before these samples reach the DAC).
    AudioFocus_NotifyVoiceStart();

    AUDIO_ClearOutputQueue();
    STATUS_IO_SetPttActive(true);

#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    if (!ES8311_SetReceiveMode()) {
        ESP_LOGI(TAG,"[NRL] failed to keep ES8311 in receive/speaker mode");
        STATUS_IO_SetPttActive(false);
        AudioFocus_NotifyVoiceEnd();
        return;
    }
#endif

    s_downlink_playback_active = true;
}

static void logIncomingVoicePacket(const size_t payload_size)
{
    if (!s_voice_stream_logged) {
        s_voice_stream_logged = true;
        s_last_voice_payload_size = payload_size;
        const char *callsign = (s_remote_callsign[0] != '\0') ? s_remote_callsign : "UNKNOWN";
        ESP_LOGI(TAG,"[NRL] voice start: from=%s-%u\n",
                      callsign,
                      static_cast<unsigned>(s_remote_ssid));
        return;
    }

    if (payload_size != s_last_voice_payload_size) {
        s_last_voice_payload_size = payload_size;
    }
}

static void logReceivedPacket(const uint8_t packet_type,
                              const size_t payload_size,
                              const size_t packet_size,
                              const uint32_t peer_ip,
                              const uint16_t peer_port)
{
    if (packet_type == kNrlTypeHeartbeat ||
        packet_type == kNrlTypeServerHeartbeat ||
        packet_type == kNrlTypeVoice ||
        packet_type == kNrlTypeServerVoice ||
        packet_type == kNrlTypeOpusVoice ||
        packet_type == kNrlTypeVideo ||
        packet_type == kNrlTypeSciTransparent) {
        return;
    }

    const char *callsign = (s_remote_callsign[0] != '\0') ? s_remote_callsign : "UNKNOWN";
    char ip_buf[16] = {};
    nrlIpToString(peer_ip, ip_buf, sizeof(ip_buf));
    ESP_LOGI(TAG, "rx packet: from=%s-%u ip=%s:%u type=%u packet=%u payload=%u",
             callsign,
             static_cast<unsigned>(s_remote_ssid),
             ip_buf,
             static_cast<unsigned>(peer_port),
             static_cast<unsigned>(packet_type),
             static_cast<unsigned>(packet_size),
             static_cast<unsigned>(payload_size));
}

static void handleIncomingTextMessagePayload(const uint8_t *payload, const size_t payload_size)
{
    const NrlTextMessageView message = NRL_TEXT_MESSAGE_Parse(payload, payload_size);
    const char *media = message.kind == NRL_TEXT_MESSAGE_UNKNOWN && message.tag[0] != '\0'
                            ? message.tag
                            : NRL_TEXT_MESSAGE_KindLabel(message.kind);
    NRL_TEXT_MESSAGE_CopySanitized(message.content, message.content_size,
                                   s_text_message_log, sizeof(s_text_message_log));
    if (s_text_message_log[0] == '\0') {
        snprintf(s_text_message_log, sizeof(s_text_message_log), "(empty)");
    }

    const char *callsign = s_remote_callsign[0] != '\0' ? s_remote_callsign : "UNKNOWN";
    ESP_LOGI(TAG, "[NRL][TYPE5] from=%s-%u media=%s bytes=%u data=%s",
             callsign, static_cast<unsigned>(s_remote_ssid), media,
             static_cast<unsigned>(payload_size), s_text_message_log);

    char notice[sizeof(DisplayNoticeSnapshot::text)] = {};
    const int prefix_result = snprintf(notice, sizeof(notice), "NRL %.6s-%u %.8s: ",
                                       callsign, static_cast<unsigned>(s_remote_ssid), media);
    size_t prefix_size = prefix_result > 0 ? static_cast<size_t>(prefix_result) : 0u;
    if (prefix_size >= sizeof(notice)) prefix_size = sizeof(notice) - 1u;
    NRL_TEXT_MESSAGE_CopySanitized(message.content, message.content_size,
                                   notice + prefix_size, sizeof(notice) - prefix_size);
    if (notice[prefix_size] == '\0') {
        snprintf(notice + prefix_size, sizeof(notice) - prefix_size, "(empty)");
    }
    DISPLAY_NOTICE_Post(notice, DISPLAY_NOTICE_INFO, kTextMessageNoticeDurationMs);
}

static void stopDownlinkPlayback(void)
{
    if (!s_downlink_playback_active) {
        return;
    }

    // NRL voice marks this flag through logIncomingVoicePacket(). ESP-NOW uses
    // the same playback lifecycle but must not trigger the NRL speaker tail.
    const bool was_nrl_voice = s_voice_stream_logged;
    AUDIO_ClearOutputQueue();

#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    if (!ES8311_SetReceiveMode()) {
        ESP_LOGI(TAG,"[NRL] failed to keep ES8311 in receive/speaker mode");
    }
#endif

    s_downlink_playback_active = false;
    AudioFocus_NotifyVoiceEnd();
    s_voice_stream_logged = false;
    s_voice_decode_logged = false;
    s_voice_decode_peak_max = 0;
    s_voice_decode_sample_total = 0u;
    s_voice_decode_chunks = 0u;
    s_last_voice_payload_size = 0u;
    // Clear the caller identity with the session: playback can be re-activated
    // by ESP-NOW intercom RX (see bridgeTask), and GetRemoteCaller() must not
    // resurrect the previous NRL caller's name on the LCD in that case.
    s_voice_callsign[0] = '\0';
    s_voice_ssid = 0u;
    s_voice_dmr_id = 0u;
    STATUS_IO_SetPttActive(false);

    // Arm the tail-audio suppression window now that downlink playback (the
    // voice we just sent out to the radio) has ended. See sendVoiceFrame().
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const uint32_t tail_ms = (config != nullptr) ? config->tail_suppress_ms : 0u;
    if (tail_ms > 0u) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        s_tail_suppress_until_ms = now + tail_ms;
    } else {
        s_tail_suppress_until_ms = 0u;
    }

    // Append configured MDC1200/DTMF audio only after the network voice queue
    // has ended, so the burst is the radio-side tail rather than mixed voice.
    if (was_nrl_voice) {
        SIGNALING_OnNetworkVoiceEnded();
    }
}

static void handleIncomingVoicePayload(const uint8_t *payload, const size_t payload_size)
{
    // The NRL packet type alone identifies a voice packet; the G.711 payload
    // length is variable, so it must not be range-checked here. The decode
    // loop below chunks whatever length arrived.
    if (payload == nullptr) {
        return;
    }

    // Priority: ESP-NOW intercom > NRL voice > music (the router does not
    // mix -- see audio_router.h). While an ESP-NOW stream is live it owns the
    // speaker; drop NRL voice frames so the two don't interleave into garble.
    if (ESPNOW_LINK_IsReceiving()) {
        return;
    }

    // A real voice packet: latch its sender as the current voice caller.
    // updateRemoteIdentity() already parsed this packet's header into
    // s_remote_callsign / s_remote_ssid just before this call.
    memcpy(s_voice_callsign, s_remote_callsign, sizeof(s_voice_callsign));
    s_voice_ssid = s_remote_ssid;
    s_voice_dmr_id = s_remote_dmr_id;

    startDownlinkPlayback();
    if (!s_downlink_playback_active) {
        return;
    }

    s_last_rx_packet_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_last_rx_codec = 0u; // G.711
    logIncomingVoicePacket(payload_size);

    // Route the downlink to the Bluetooth headset whenever one is connected;
    // otherwise play it out the onboard codec. Exactly one of the two sinks
    // is active at a time (the router does not mix yet -- see audio_router.h).
    const bool bt_route = NRL_BtHfp_IsConnected();
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_SPEAKER, !bt_route);
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_BT_HFP, bt_route);

    size_t offset = 0u;
    while (offset < payload_size) {
        const size_t chunk = ((payload_size - offset) < kG711RxPayloadMaxBytes)
                                 ? (payload_size - offset)
                                 : kG711RxPayloadMaxBytes;
        int16_t peak = 0;
        for (size_t i = 0; i < chunk; ++i) {
            s_downlink_pcm_buffer[i] = applyPcmGain(NRL_G711_DecodeALaw(payload[offset + i]), kDownlinkPcmGain);
            int16_t value = s_downlink_pcm_buffer[i];
            if (value < 0) {
                value = static_cast<int16_t>(-value);
            }
            if (value > peak) {
                peak = value;
            }
        }
        if (!s_voice_decode_logged) {
            s_voice_decode_logged = true;
        }
        if (peak > s_voice_decode_peak_max) {
            s_voice_decode_peak_max = peak;
        }
        s_voice_decode_sample_total += static_cast<uint32_t>(chunk);
        ++s_voice_decode_chunks;
        if (chunk > 0u) {
            AudioRouter_PushFrame(AUDIO_SRC_NRL_DOWNLINK, 8000u, s_downlink_pcm_buffer, chunk);
        }
        offset += chunk;
    }
}

// Wideband sibling of handleIncomingVoicePayload: one 16 kHz/20 ms Opus
// frame per type-8 packet. Shares the caller latch, playback state and
// per-packet BT/speaker routing; the router upsamples nothing here (the
// speaker sink runs at 16 kHz natively, BT SCO gets a 16k->8k downconvert).
static void handleIncomingOpusPayload(const uint8_t *payload, const size_t payload_size)
{
    if (payload == nullptr || payload_size == 0u) {
        return;
    }

    // Priority: ESP-NOW intercom > NRL voice > music (see the G.711 handler).
    if (ESPNOW_LINK_IsReceiving()) {
        return;
    }

    memcpy(s_voice_callsign, s_remote_callsign, sizeof(s_voice_callsign));
    s_voice_ssid = s_remote_ssid;
    s_voice_dmr_id = s_remote_dmr_id;

    startDownlinkPlayback();
    if (!s_downlink_playback_active) {
        return;
    }

    s_last_rx_packet_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_last_rx_codec = 1u; // Opus
    // Feed the stream logger/timeout the G.711-equivalent of 20 ms so
    // currentRxPacketTimeoutMs' bytes-per-ms math stays meaningful.
    logIncomingVoicePacket(160u);

    const bool bt_route = NRL_BtHfp_IsConnected();
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_SPEAKER, !bt_route);
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_BT_HFP, bt_route);

    if (s_opus_dec == nullptr) {
        s_opus_dec = OPUS_VOICE_DecOpen(kOpusFrameMs);
        if (s_opus_dec == nullptr) {
            return;
        }
    }
    static int16_t pcm16k[kOpusFrameSamples * 3u]; // headroom for 40/60 ms senders
    const int decoded = OPUS_VOICE_DecProcess(s_opus_dec, payload, payload_size,
                                              pcm16k, sizeof(pcm16k) / sizeof(pcm16k[0]));
    if (decoded > 0) {
        AudioRouter_PushFrame(AUDIO_SRC_NRL_DOWNLINK, 16000u, pcm16k,
                              static_cast<size_t>(decoded));
    }
}

static void sendHeartbeat(void)
{
    uint8_t packet[kNrlHeaderSize];
    const size_t packet_size = buildNrlPacket(kNrlTypeHeartbeat, nullptr, 0u, packet, sizeof(packet));
    if (packet_size > 0u) {
        udpSendPacket(packet, packet_size);
    }
}

static void restartTransport(const bool restart_wifi, const bool restart_udp)
{
    if (restart_udp) {
        if (s_udp_fd >= 0) {
            close(s_udp_fd);
            s_udp_fd = -1;
        }
        s_udp_started = false;
        s_server_ip = 0u;
    }
    if (restart_wifi) {
        esp_wifi_disconnect();
        // Clear the retry backoff and failure counter so the bridge task
        // tries the new credentials on its next loop iteration instead of
        // waiting out the 30s backoff from the previous bad SSID. Without
        // this, submitting a new SSID via the config portal silently waits
        // up to 30s before the device leaves AP fallback for STA.
        s_next_wifi_retry_ms = 0u;
        s_wifi_connect_failures = 0u;
    }
}

static void scheduleReboot(void)
{
    s_reboot_pending = true;
    s_reboot_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) + 1000u;
    ESP_LOGI(TAG,"[NRL] reboot scheduled by AT command");
}

//=============== Serial AT command console (USB debug serial) ================

// Sized for the longest command: AT+RADIOADD=<48-byte name>,<200-byte URL>.
constexpr size_t kSerialAtLineMax = 288u;
char s_serial_at_line[kSerialAtLineMax];
size_t s_serial_at_len = 0u;
uint8_t s_serial_at_payload[kSerialAtLineMax + 1u];
// Serial AT commands execute inside nrl_main_loop. Keep the 1 KB reply object
// off that task's 6 KB stack; the remote AT path has its own result object on
// the separate 16 KB bridge task.
NrlAtCommandResult s_serial_at_result = {};

// Execute one AT command line typed on the USB debug serial and print the
// reply back to the same serial port.
static void processSerialAtLine(char *line)
{
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    size_t len = strlen(line);
    while (len > 0u && (line[len - 1u] == ' ' || line[len - 1u] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0u) {
        return;
    }

    ESP_LOGI(TAG,"[AT] serial command: %s\n", line);

    // Wrap the text line into the binary payload the AT handler expects:
    // byte 0 is the 0x01 request marker, followed by the "AT+KEY=VALUE" text.
    if (len > sizeof(s_serial_at_payload) - 1u) {
        len = sizeof(s_serial_at_payload) - 1u;
    }
    s_serial_at_payload[0] = 0x01u;
    memcpy(s_serial_at_payload + 1u, line, len);

    NRL_AT_HandlePayload(s_serial_at_payload, len + 1u, NRL_AT_SOURCE_SERIAL, &s_serial_at_result);

    if (s_serial_at_result.should_reply && s_serial_at_result.payload_size > 1u) {
        // Skip the 0x02 reply marker; the remainder is CRLF-delimited text.
        NRL_USB_Console_Write(s_serial_at_result.payload + 1u,
                              s_serial_at_result.payload_size - 1u);
    }

    if (s_serial_at_result.restart_wifi || s_serial_at_result.restart_udp) {
        ESP_LOGI(TAG,"[AT] applying config via serial: restart_wifi=%d restart_udp=%d\n",
                      s_serial_at_result.restart_wifi ? 1 : 0,
                      s_serial_at_result.restart_udp ? 1 : 0);
        restartTransport(s_serial_at_result.restart_wifi, s_serial_at_result.restart_udp);
    }
    if (s_serial_at_result.reboot) {
        scheduleReboot();
    }
}

static void pollSerialAtConsole(void)
{
    uint8_t buf[32];
    while (true) {
        const size_t got = NRL_USB_Console_Read(buf, sizeof(buf));
        if (got == 0u) {
            break;
        }
        ESP_LOGI(TAG, "[SERIAL] RX %u bytes, first=0x%02X", (unsigned)got, buf[0]);
        // Hex dump for Improv debugging (first 32 bytes)
        {
            char hex[3*32+1];
            const size_t n = got < 32 ? got : 32;
            for (size_t h = 0; h < n; ++h)
                snprintf(hex + h*3, 4, "%02X ", buf[h]);
            hex[n*3] = '\0';
            ESP_LOGI(TAG, "[SERIAL] HEX: %s", hex);
        }
        for (size_t i = 0; i < got; ++i) {
            // Improv protocol (binary frames from ESP Web Tools) takes priority;
            // only bytes not consumed by Improv reach the AT line parser.
            if (IMPROV_ProcessByte(buf[i])) {
                continue;
            }
            const char ch = static_cast<char>(buf[i]);
            if (ch == '\n' || ch == '\r') {
                if (s_serial_at_len > 0u) {
                    s_serial_at_line[s_serial_at_len] = '\0';
                    processSerialAtLine(s_serial_at_line);
                    s_serial_at_len = 0u;
                }
                continue;
            }
            if (s_serial_at_len + 1u >= sizeof(s_serial_at_line)) {
                s_serial_at_len = 0u;
                ESP_LOGW(TAG, "AT serial command too long, discarded");
                continue;
            }
            s_serial_at_line[s_serial_at_len++] = ch;
        }
    }
}

static void bridgeTask(void *)
{
    bool previous_sql = STATUS_IO_IsSqlActive();
    if (previous_sql && ESPNOW_LINK_GetPttMode() == 0u) {
        AudioFocus_NotifyVoiceStart();
    }
    while (true) {
        if (!ensureNetworkAndUdp()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool have_packet = false;
        int bytes_read = 0;
        if (s_udp_fd >= 0 && s_udp_mutex != nullptr &&
            xSemaphoreTake(s_udp_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            struct sockaddr_in from = {};
            socklen_t fromlen = sizeof(from);
            const ssize_t got = recvfrom(s_udp_fd,
                                         s_rx_packet_buffer,
                                         sizeof(s_rx_packet_buffer),
                                         MSG_DONTWAIT,
                                         reinterpret_cast<struct sockaddr *>(&from),
                                         &fromlen);
            if (got > 0) {
                s_last_peer_ip = from.sin_addr.s_addr;
                s_last_peer_port = ntohs(from.sin_port);
                bytes_read = static_cast<int>(got);
                have_packet = true;
            }
            xSemaphoreGive(s_udp_mutex);
        }

        if (have_packet) {
            uint8_t packet_type = 0u;
            const uint8_t *payload = nullptr;
            size_t payload_size = 0u;
            if (parseNrlPacket(s_rx_packet_buffer,
                               static_cast<size_t>(bytes_read),
                               &packet_type,
                               &payload,
                               &payload_size)) {
                STATUS_IO_NotifyHeartbeatReceived();
                updateRemoteIdentity(s_rx_packet_buffer, static_cast<size_t>(bytes_read));
                logReceivedPacket(packet_type,
                                  payload_size,
                                  static_cast<size_t>(bytes_read),
                                  s_last_peer_ip,
                                  s_last_peer_port);
                if (packet_type == kNrlTypeVoice || packet_type == kNrlTypeServerVoice) {
                    handleIncomingVoicePayload(payload, payload_size);
                } else if (packet_type == kNrlTypeTextMessage) {
                    handleIncomingTextMessagePayload(payload, payload_size);
                } else if (packet_type == kNrlTypeOpusVoice) {
                    handleIncomingOpusPayload(payload, payload_size);
                } else if (packet_type == kNrlTypeVideo) {
                    const NrlVideoRxHandler_t video_handler = s_video_rx_handler;
                    if (video_handler != nullptr) {
                        video_handler(payload, payload_size);
                    }
                } else if (packet_type == kNrlTypeSciTransparent) {
                    handleIncomingSciPayload(payload, payload_size);
                } else if (packet_type == kNrlTypeAtCommand) {
                    char at_log_text[kAtLogTextMax] = {};
                    formatAtLogText(payload, payload_size, at_log_text, sizeof(at_log_text));
                    ESP_LOGI(TAG,"[NRL] at packet: from=%s-%u payload=%u data=%s\n",
                                  (s_remote_callsign[0] != '\0') ? s_remote_callsign : "UNKNOWN",
                                  static_cast<unsigned>(s_remote_ssid),
                                  static_cast<unsigned>(payload_size),
                                  at_log_text);
                    NrlAtCommandResult at_result = {};
                    NRL_AT_HandlePayload(payload, payload_size, NRL_AT_SOURCE_REMOTE, &at_result);
                    if (at_result.should_reply && at_result.payload_size > 0u) {
                        const size_t reply_packet_size = buildNrlPacket(kNrlTypeAtCommand,
                                                                        at_result.payload,
                                                                        at_result.payload_size,
                                                                        s_at_reply_packet,
                                                                        sizeof(s_at_reply_packet));
                        if (reply_packet_size > 0u) {
                            const bool sent = udpSendPacketTo(s_at_reply_packet,
                                                              reply_packet_size,
                                                              s_last_peer_ip,
                                                              s_last_peer_port);
                            if (!sent) {
                                char ip_buf[16] = {};
                                nrlIpToString(s_last_peer_ip, ip_buf, sizeof(ip_buf));
                                ESP_LOGE(TAG, "at reply send failed to %s:%u",
                                         ip_buf,
                                         static_cast<unsigned>(s_last_peer_port));
                            }
                        }
                    }
                    if (at_result.restart_wifi || at_result.restart_udp) {
                        restartTransport(at_result.restart_wifi, at_result.restart_udp);
                    }
                    if (at_result.reboot) {
                        scheduleReboot();
                    }
                }
            } else {
                char ip_buf[16] = {};
                nrlIpToString(s_last_peer_ip, ip_buf, sizeof(ip_buf));
                ESP_LOGW(TAG, "rx parse failed: ip=%s:%u bytes=%u hdr=%02X %02X %02X %02X len=%02X%02X type=%02X",
                         ip_buf,
                         static_cast<unsigned>(s_last_peer_port),
                         static_cast<unsigned>(bytes_read),
                         static_cast<unsigned>(s_rx_packet_buffer[0]),
                         static_cast<unsigned>(s_rx_packet_buffer[1]),
                         static_cast<unsigned>(s_rx_packet_buffer[2]),
                         static_cast<unsigned>(s_rx_packet_buffer[3]),
                         static_cast<unsigned>(s_rx_packet_buffer[4]),
                         static_cast<unsigned>(s_rx_packet_buffer[5]),
                         static_cast<unsigned>((bytes_read > 20) ? s_rx_packet_buffer[20] : 0u));
            }
        }

        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        // ESP-NOW intercom RX rides the same speaker playback lifecycle as the
        // NRL downlink: startDownlinkPlayback() takes audio focus and switches
        // the ES8311 into receive/speaker mode. Without this, decoded ESP-NOW
        // frames queue into a DAC that was never switched to playback -- the
        // router route is enabled but nothing is audible (gezipai/bh4tdv).
        if (ESPNOW_LINK_IsReceiving()) {
            startDownlinkPlayback();
            s_last_rx_packet_ms = now;
            // Preemption owns the display too: NRL frames are dropped while the
            // intercom is live, so show the ESP-NOW peer, not a stale caller.
            if (s_voice_callsign[0] != '\0') {
                s_voice_callsign[0] = '\0';
                s_voice_ssid = 0u;
            }
            s_voice_dmr_id = 0u;
        }
        if (s_downlink_playback_active &&
            (now - s_last_rx_packet_ms) > currentRxPacketTimeoutMs()) {
            stopDownlinkPlayback();
        }
        if ((now - s_last_heartbeat_ms) > static_cast<uint32_t>(NRL_AUDIO_HEARTBEAT_INTERVAL_MS)) {
            sendHeartbeat();
            s_last_heartbeat_ms = now;
        }
        if ((now - s_last_sci_poll_ms) >= kSciPollIntervalMs) {
            s_last_sci_poll_ms = now;
            pollSciUplink();
        }
        if (s_reboot_pending && static_cast<int32_t>(now - s_reboot_at_ms) >= 0) {
            ESP_LOGI(TAG, "reboot now");
            NRL_USB_Console_Flush();
            esp_restart();
        }

        STATUS_IO_Poll();
        const bool current_sql = STATUS_IO_IsSqlActive();
        if (!previous_sql && current_sql && ESPNOW_LINK_GetPttMode() == 0u) {
            AudioFocus_NotifyVoiceStart();
        }
        if (previous_sql && !current_sql && ESPNOW_LINK_GetPttMode() == 0u) {
            // The captured voice gate just closed: queue the signaling burst
            // ungated so it follows the final NRL voice packet.
            SIGNALING_OnLocalPttReleased();
            AudioFocus_NotifyVoiceEnd();
        }
        previous_sql = current_sql;

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

} // namespace

bool NRLAudioBridge_SendTyped(const uint8_t packet_type, const uint8_t *payload, const size_t payload_size)
{
    if (payload == nullptr || payload_size == 0u || payload_size > kNrlMaxPayloadSize) {
        return false;
    }
    static uint8_t packet[kNrlMaxPacketSize]; // bridge/video task only; sized for the max payload
    const size_t packet_size = buildNrlPacket(packet_type, payload, payload_size,
                                              packet, sizeof(packet));
    if (packet_size == 0u) {
        return false;
    }
    return udpSendPacket(packet, packet_size);
}

void NRLAudioBridge_SetVideoRxHandler(NrlVideoRxHandler_t handler)
{
    s_video_rx_handler = handler;
}

// Pre-open the bridge's own Opus encoder+decoder (idempotent). Returns false
// when either allocation fails.
static bool bridgePrewarmOpus(void)
{
    if (s_opus_enc == nullptr) {
        s_opus_enc = OPUS_VOICE_EncOpen(kOpusFrameMs);
    }
    if (s_opus_dec == nullptr) {
        s_opus_dec = OPUS_VOICE_DecOpen(kOpusFrameMs);
    }
    if (s_opus_enc == nullptr || s_opus_dec == nullptr) {
        ESP_LOGW(TAG, "[NRL] opus prewarm failed (enc=%d dec=%d)",
                 s_opus_enc != nullptr, s_opus_dec != nullptr);
        return false;
    }
    return true;
}

bool NRLAudioBridge_SetVoiceCodec(uint8_t codec)
{
    if (codec > 1u) {
        return false;
    }
    if (codec == 1u && !bridgePrewarmOpus()) {
        // Allocate everything the NRL Opus path needs up front so a RAM
        // shortfall fails the switch here instead of silently muting audio
        // mid-conversation. Roll back: stay on G.711 and free the TX encoder
        // (unused while the codec is G.711, so this cannot race the audio
        // task); the decoder is kept -- RX follows the remote side's codec.
        // The ESP-NOW intercom codec is a separate switch (ESPNOW_LINK_SetTxCodec).
        s_voice_codec = 0u;
        if (s_opus_enc != nullptr) {
            OPUS_VOICE_EncClose(s_opus_enc);
            s_opus_enc = nullptr;
        }
        ESP_LOGW(TAG, "[NRL] opus switch rolled back to G.711 (allocation failed)");
        return false;
    }
    s_voice_codec = codec;
    s_opus_tx_fill = 0u;
    nvs_handle_t nvs;
    if (nvs_open(kVoiceNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "codec", codec);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    return true;
}

uint8_t NRLAudioBridge_GetVoiceCodec(void)
{
    return s_voice_codec;
}

void NRLAudioBridge_SetMediaUplinkActive(bool active)
{
    s_media_uplink_active = active;
}

void NRLAudioBridge_SendMediaUplink(const short *pcm8k, size_t sample_count)
{
    // Media (nanny music / beacon) streams to the NRL server regardless of
    // the radio squelch: it's a deliberate transmission, not captured audio.
    sendVoiceFrameInternal(pcm8k, sample_count, false);
}

void NRLAudioBridge_FeedExternalMic(const short *pcm8k, size_t sample_count)
{
    // The Bluetooth headset mic delivers PCM16 mono at 8 kHz. Push it through
    // the router as its own source so the uplink sink can tell it apart from
    // the onboard mic (PTT gating and G.711 packetisation reused unchanged).
    AudioRouter_PushFrame(AUDIO_SRC_BT_HFP_MIC, 8000u, pcm8k, sample_count);
}

bool NRLAudioBridge_Init(void)
{
    if (s_bridge_initialized) {
        return true;
    }

    EXTERNAL_RADIO_Init();
    SCI_SERIAL_Init();
    initCpuId();
    NRL_G711_Init();

    // Restore the persisted TX voice codec (0 = G.711 type 1, 1 = Opus type 8).
    // Do NOT prewarm the Opus encoder here: this runs on the main task whose
    // stack (~24 KB) cannot accommodate the silk encoder init (~10 KB on top
    // of all the other init frames already on the stack).  The encoder is
    // lazily created in sendOpusVoice16k() on the audio_passthrough task
    // (32 KB stack) the first time an Opus frame is actually produced.
    {
        nvs_handle_t nvs;
        uint8_t codec = 0u;
        if (nvs_open(kVoiceNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
            (void)nvs_get_u8(nvs, "codec", &codec);
            nvs_close(nvs);
        }
        s_voice_codec = (codec <= 1u) ? codec : 0u;
    }

    if (s_udp_mutex == nullptr) {
        s_udp_mutex = xSemaphoreCreateMutex();
        if (s_udp_mutex == nullptr) {
            ESP_LOGI(TAG,"[NRL] failed to create udp mutex");
            return false;
        }
    }

    // Wire the NRL voice paths into the audio router: both mic flavours feed
    // the uplink; the downlink starts on the onboard speaker (flipped to the
    // BT sink per-packet in handleIncomingVoicePayload when a headset is up).
    AudioRouter_RegisterSink(AUDIO_SINK_NRL_UPLINK, 16000u, uplinkSinkWrite, nullptr);
    AudioRouter_RegisterSink(AUDIO_SINK_BT_HFP, 8000u, btHfpSinkWrite, nullptr);
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_NRL_UPLINK, true);
    AudioRouter_SetRoute(AUDIO_SRC_BT_HFP_MIC, AUDIO_SINK_NRL_UPLINK, true);
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_SPEAKER, true);

    // Pin to core 0 so RX recvfrom() / heartbeat / AT replies share the WiFi
    // and lwIP cores; voice TX from the audio task on core 1 still hits the
    // same socket but lwIP's internal locking handles that.
    //
    // 16 KB stack: the Opus RX decode (handleIncomingOpusPayload ->
    // esp_opus_dec_decode / libopus) runs inline on this task and its peak
    // stack, on top of the UDP recv + packet-parse baseline, overflows an
    // 8 KB stack (stack-protection fault on the first received Opus frame).
    //
    // Stack MUST be internal RAM: this task persists configuration (remote AT
    // commands, volume saves -> NVS/EEPROM), and flash writes disable the
    // cache -- a task whose stack lives in PSRAM (which sits behind that very
    // cache) trips the esp_task_stack_is_sane_cache_disabled() assert the
    // moment it touches flash. A PSRAM-stack experiment also fragmented the
    // internal heap on the S31 boards, breaking the Classic BT controller's
    // large contiguous pool. The S3 display boards can afford these 16 KB
    // internal since BLE provisioning became boot-skipped (~66 KB reclaimed).
    if (xTaskCreatePinnedToCore(bridgeTask, "nrl_audio_bridge", 16384, nullptr,
                                2, &s_bridge_task, 0) != pdPASS) {
        ESP_LOGI(TAG,"[NRL] failed to create bridge task");
        return false;
    }

    s_bridge_initialized = true;
    ESP_LOGI(TAG,"[NRL] bridge task started");
    return true;
}

bool NRLAudioBridge_GetRemoteIdentity(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return false;
    }

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_last_remote_identity_ms == 0u ||
        (now - s_last_remote_identity_ms) > static_cast<uint32_t>(NRL_AUDIO_RX_PACKET_TIMEOUT_MS + 2000u) ||
        s_remote_callsign[0] == '\0') {
        buffer[0] = '\0';
        return false;
    }

    snprintf(buffer, buffer_size, "%s-%u", s_remote_callsign, static_cast<unsigned>(s_remote_ssid));
    return true;
}

bool NRLAudioBridge_GetRemoteCaller(char *callsign, size_t callsign_size, unsigned *ssid)
{
    if (callsign != nullptr && callsign_size > 0u) {
        snprintf(callsign, callsign_size, "%s", s_voice_callsign);
    }
    if (ssid != nullptr) {
        *ssid = static_cast<unsigned>(s_voice_ssid);
    }

    // "Receiving" means a voice stream is actually playing right now -- not
    // merely that some packet arrived recently. Heartbeat / AT / SCI packets
    // never start downlink playback, so they no longer count as reception.
    return s_downlink_playback_active;
}

uint32_t NRLAudioBridge_GetRemoteDmrId(void)
{
    return s_downlink_playback_active ? s_voice_dmr_id : 0u;
}

uint8_t NRLAudioBridge_GetRxCodec(void)
{
    return s_last_rx_codec;
}

void NRLAudioBridge_ApplyConfig(bool restart_wifi, bool restart_udp)
{
    if (!restart_wifi && !restart_udp) {
        return;
    }

    restartTransport(restart_wifi, restart_udp);
}

void NRLAudioBridge_PollSerialConsole(void)
{
    pollSerialAtConsole();
}
