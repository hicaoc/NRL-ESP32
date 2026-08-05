#include "services/ai_assistant.h"

#include "driver/board_pins.h"

#include <stdio.h>

#if NRL_BOARD == NRL_BOARD_S31_KORVO || NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD

#include "audio/audio_router.h"
#include "media/opus_voice.h"
#include "lib/nrl_psram.h"
#include "services/config_notify.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "AI";

namespace {

constexpr const char *kNvsNamespace = "ai";
constexpr uint32_t kOpusFrameMs = 60u;   // xiaozhi default frame duration
constexpr size_t kOpusFrameSamples = (OPUS_VOICE_SAMPLE_RATE / 1000u) * kOpusFrameMs; // 960
// Mic PCM ring between the audio task (producer) and the AI task
// (encoder/sender): 16 kHz mono, ~4 s.
constexpr size_t kMicRingSamples = 64 * 1024;
constexpr size_t kRxAssembleMax = 4096;

static char s_url[160] = {};
static char s_token[96] = {};
static volatile bool s_enabled = false;
static volatile bool s_connected = false;   // hello exchanged
static volatile bool s_listening = false;
static char s_session_id[48] = {};

static esp_websocket_client_handle_t s_ws = nullptr;
static OpusVoiceEnc *s_enc = nullptr;
static OpusVoiceDec *s_dec = nullptr;

NRL_PSRAM_BSS static int16_t s_mic_ring_storage[kMicRingSamples];
static int16_t *s_mic_ring = s_mic_ring_storage;
static volatile size_t s_mic_head = 0;
static volatile size_t s_mic_tail = 0;

static TaskHandle_t s_ai_task = nullptr;
static volatile bool s_ai_task_run = false;

// Reassembly buffer for fragmented WS payloads (TTS Opus frames are small,
// but the client may still split across events).
NRL_PSRAM_BSS static uint8_t s_rx_buf[kRxAssembleMax];

static void save_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_str(nvs, "url", s_url);
        (void)nvs_set_str(nvs, "token", s_token);
        (void)nvs_set_u8(nvs, "en", s_enabled ? 1 : 0);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    CONFIG_NOTIFY_Bump();
}

static bool load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_url);
    (void)nvs_get_str(nvs, "url", s_url, &len);
    len = sizeof(s_token);
    (void)nvs_get_str(nvs, "token", s_token, &len);
    uint8_t en = 0;
    (void)nvs_get_u8(nvs, "en", &en);
    nvs_close(nvs);
    return en != 0 && s_url[0] != '\0';
}

static void send_json(const char *json)
{
    if (s_ws != nullptr && esp_websocket_client_is_connected(s_ws)) {
        (void)esp_websocket_client_send_text(s_ws, json, static_cast<int>(strlen(json)),
                                             pdMS_TO_TICKS(2000));
    }
}

static void send_hello(void)
{
    static const char kHello[] =
        "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}";
    send_json(kHello);
}

static void handle_server_json(const char *text, const size_t len)
{
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (root == nullptr) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "hello") == 0) {
            const cJSON *sid = cJSON_GetObjectItem(root, "session_id");
            if (cJSON_IsString(sid)) {
                snprintf(s_session_id, sizeof(s_session_id), "%s", sid->valuestring);
            }
            s_connected = true;
            ESP_LOGI(TAG, "session up (id=%s)", s_session_id);
        } else if (strcmp(type->valuestring, "stt") == 0) {
            const cJSON *txt = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(txt)) {
                ESP_LOGI(TAG, "you said: %s", txt->valuestring);
            }
        } else if (strcmp(type->valuestring, "tts") == 0) {
            const cJSON *state = cJSON_GetObjectItem(root, "state");
            const cJSON *txt = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(txt)) {
                ESP_LOGI(TAG, "ai: %s", txt->valuestring);
            }
            if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "tts done");
            }
        }
    }
    cJSON_Delete(root);
}

static void handle_tts_opus(const uint8_t *frame, const size_t bytes)
{
    if (s_dec == nullptr) {
        s_dec = OPUS_VOICE_DecOpen(kOpusFrameMs);
        if (s_dec == nullptr) {
            return;
        }
    }
    NRL_PSRAM_BSS static int16_t pcm[kOpusFrameSamples * 2u];
    const int n = OPUS_VOICE_DecProcess(s_dec, frame, bytes, pcm,
                                        sizeof(pcm) / sizeof(pcm[0]));
    if (n > 0) {
        AudioRouter_PushFrame(AUDIO_SRC_AI, OPUS_VOICE_SAMPLE_RATE, pcm,
                              static_cast<size_t>(n));
    }
}

static void ws_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    const esp_websocket_event_data_t *data =
        static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected, sending hello");
        s_session_id[0] = '\0';
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_connected = false;
        s_listening = false;
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (data == nullptr || data->data_len <= 0) {
            break;
        }
        // Reassemble fragmented payloads (payload_offset/-len describe the
        // whole message; data_ptr/data_len this fragment).
        if (data->payload_len <= 0 ||
            static_cast<size_t>(data->payload_len) > sizeof(s_rx_buf)) {
            break; // oversized: ignore
        }
        memcpy(s_rx_buf + data->payload_offset, data->data_ptr,
               static_cast<size_t>(data->data_len));
        if (data->payload_offset + data->data_len < data->payload_len) {
            break; // wait for the rest
        }
        if (data->op_code == 0x1) { // text
            handle_server_json(reinterpret_cast<const char *>(s_rx_buf),
                               static_cast<size_t>(data->payload_len));
        } else if (data->op_code == 0x2) { // binary: TTS Opus frame
            handle_tts_opus(s_rx_buf, static_cast<size_t>(data->payload_len));
        }
        break;
    }
    default:
        break;
    }
}

// Router sink (audio task): copy mic PCM into the ring; the AI task encodes.
static void ai_sink_write(uint8_t /*source_id*/, const int16_t *samples,
                          size_t sample_count, void *)
{
    if (!s_listening || s_mic_ring == nullptr) {
        return;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t next = (s_mic_head + 1u) % kMicRingSamples;
        if (next == s_mic_tail) {
            return; // full: drop (listening turns are short)
        }
        s_mic_ring[s_mic_head] = samples[i];
        s_mic_head = next;
    }
}

// AI task: pull 60 ms mic frames from the ring, Opus-encode, send binary.
static void ai_task(void *)
{
    NRL_PSRAM_BSS static int16_t frame[kOpusFrameSamples];
    static uint8_t encoded[OPUS_VOICE_MAX_FRAME_BYTES];
    size_t fill = 0;

    while (s_ai_task_run) {
        if (!s_listening || s_ws == nullptr || !esp_websocket_client_is_connected(s_ws)) {
            fill = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        while (fill < kOpusFrameSamples && s_mic_tail != s_mic_head) {
            frame[fill++] = s_mic_ring[s_mic_tail];
            s_mic_tail = (s_mic_tail + 1u) % kMicRingSamples;
        }
        if (fill < kOpusFrameSamples) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        fill = 0;
        if (s_enc == nullptr) {
            s_enc = OPUS_VOICE_EncOpen(kOpusFrameMs);
            if (s_enc == nullptr) {
                s_listening = false;
                continue;
            }
        }
        const int n = OPUS_VOICE_EncProcess(s_enc, frame, kOpusFrameSamples,
                                            encoded, sizeof(encoded));
        if (n > 0) {
            (void)esp_websocket_client_send_bin(s_ws, reinterpret_cast<const char *>(encoded),
                                                n, pdMS_TO_TICKS(1000));
        }
    }
    s_ai_task = nullptr;
    vTaskDelete(nullptr);
}

static bool ws_start(void)
{
    if (s_ws != nullptr) {
        return true;
    }
    if (s_url[0] == '\0') {
        return false;
    }

    // xiaozhi identifies devices by MAC; Client-Id just needs stability.
    uint8_t mac[6] = {};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static char device_id[20];
    snprintf(device_id, sizeof(device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    static char client_id[40];
    snprintf(client_id, sizeof(client_id), "nrl-esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
    static char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    static char headers[320];
    snprintf(headers, sizeof(headers),
             "Authorization: %s\r\nProtocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
             auth, device_id, client_id);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = s_url;
    cfg.headers = headers;
    cfg.task_stack = 8192;
    cfg.buffer_size = 4096;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == nullptr) {
        ESP_LOGE(TAG, "ws client init failed");
        return false;
    }
    (void)esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "ws client start failed");
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
        return false;
    }

    AudioRouter_RegisterSink(AUDIO_SINK_AI, OPUS_VOICE_SAMPLE_RATE, ai_sink_write, nullptr);
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_AI, true);
    AudioRouter_SetRoute(AUDIO_SRC_AI, AUDIO_SINK_SPEAKER, true);

    if (s_ai_task == nullptr) {
        s_ai_task_run = true;
        if (xTaskCreatePinnedToCore(ai_task, "ai_uplink", 6144, nullptr, 4, &s_ai_task, 0) != pdPASS) {
            s_ai_task_run = false;
            s_ai_task = nullptr;
            ESP_LOGE(TAG, "ai task create failed");
        }
    }
    ESP_LOGI(TAG, "connecting to %s", s_url);
    return true;
}

static void ws_stop(void)
{
    s_listening = false;
    s_connected = false;
    s_ai_task_run = false;
    for (int i = 0; i < 100 && s_ai_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_ws != nullptr) {
        (void)esp_websocket_client_stop(s_ws);
        (void)esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
    }
}

} // namespace

extern "C" void AI_Init(void)
{
    if (load_config()) {
        // esp_websocket_client reconnects on its own once the network is up,
        // so a boot-time start is safe even before WiFi connects.
        s_enabled = ws_start();
    }
}

extern "C" bool AI_Configure(const char *url, const char *token)
{
    if (url == nullptr || url[0] == '\0' || strlen(url) >= sizeof(s_url) ||
        (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0)) {
        return false;
    }
    if (token != nullptr && strlen(token) >= sizeof(s_token)) {
        return false;
    }
    ws_stop();
    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_token, sizeof(s_token), "%s", (token != nullptr) ? token : "");
    s_enabled = ws_start();
    save_config();
    return s_enabled;
}

extern "C" bool AI_SetEnabled(const bool enabled)
{
    if (enabled) {
        if (!ws_start()) {
            return false;
        }
        s_enabled = true;
    } else {
        ws_stop();
        s_enabled = false;
    }
    save_config();
    return true;
}

extern "C" bool AI_IsEnabled(void)
{
    return s_enabled;
}

extern "C" bool AI_IsConnected(void)
{
    return s_connected;
}

extern "C" bool AI_StartListen(void)
{
    if (!s_connected) {
        return false;
    }
    s_mic_head = 0;
    s_mic_tail = 0;
    char json[128];
    snprintf(json, sizeof(json),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}",
             s_session_id);
    send_json(json);
    s_listening = true;
    return true;
}

extern "C" void AI_StopListen(void)
{
    if (!s_listening) {
        return;
    }
    s_listening = false;
    char json[128];
    snprintf(json, sizeof(json),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
             s_session_id);
    send_json(json);
}

extern "C" bool AI_IsListening(void)
{
    return s_listening;
}

extern "C" void AI_Describe(char *out, const size_t out_size)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    if (s_url[0] == '\0') {
        snprintf(out, out_size, "(not configured)");
        return;
    }
    snprintf(out, out_size, "%s (%s%s)", s_url,
             s_connected ? "connected" : (s_enabled ? "connecting" : "off"),
             s_listening ? ", listening" : "");
}

extern "C" void AI_GetConfig(char *url, const size_t url_size, char *token, const size_t token_size)
{
    if (url != nullptr && url_size > 0u) {
        snprintf(url, url_size, "%s", s_url);
    }
    if (token != nullptr && token_size > 0u) {
        snprintf(token, token_size, "%s", s_token);
    }
}

#else // !S31

extern "C" void AI_Init(void) {}
extern "C" bool AI_Configure(const char *, const char *) { return false; }
extern "C" bool AI_SetEnabled(bool) { return false; }
extern "C" bool AI_IsEnabled(void) { return false; }
extern "C" bool AI_IsConnected(void) { return false; }
extern "C" bool AI_StartListen(void) { return false; }
extern "C" void AI_StopListen(void) {}
extern "C" bool AI_IsListening(void) { return false; }
extern "C" void AI_Describe(char *out, size_t out_size)
{
    if (out != nullptr && out_size > 0u) {
        snprintf(out, out_size, "(unsupported)");
    }
}
extern "C" void AI_GetConfig(char *url, size_t url_size, char *token, size_t token_size)
{
    if (url != nullptr && url_size > 0u) {
        url[0] = '\0';
    }
    if (token != nullptr && token_size > 0u) {
        token[0] = '\0';
    }
}

#endif
