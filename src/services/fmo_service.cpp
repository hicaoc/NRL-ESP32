#include "services/fmo_service.h"

#include "audio/audio_router.h"
#include "app/driver/external_radio.h"
#include "app/driver/status_io.h"
#include "lib/nrl_bt_hfp.h"
#include "lib/nrl_net_compat.h"
#include "lib/nrl_psram.h"
#include "media/opus_voice.h"
#include "media/media_metadata.h"
#include "services/fmo_adpcm.h"
#include "services/fmo_cert_store.h"
#include "services/espnow_link.h"
#include "services/config_notify.h"
#include "services/fmo_frame.h"
#include "services/fmo_protocol.h"
#include "services/fmo_qso.h"
#include "services/fmo_station_broadcast.h"
#include "services/server_list_store.h"
#include "services/time_sync_service.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mqtt_client.h>
#include <mqtt5_client.h>
#include <nvs.h>
#include <sodium.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr const char *TAG = "FMO";
constexpr const char *kNvsNamespace = "fmo";
constexpr const char *kNvsConfigKey = "config";
constexpr uint32_t kConfigMagic = 0x344f4d46u;
constexpr uint16_t kConfigVersion = 2u;
constexpr size_t kRawMaxSize = 8192u;
constexpr size_t kServerMax = 256u;
constexpr uint32_t kServerCacheMagic = 0x43534d46u; // "FMSC"
constexpr uint16_t kServerCacheVersion = 1u;
constexpr int64_t kServerSaveDelayUs = 15000000LL;
constexpr int64_t kServerSaveMinIntervalUs = 300000000LL;
constexpr size_t kTxPacketsPerFrame = 6u;
constexpr size_t kOpusFrameSamples = 320u; // FMO: 8 kHz, 40 ms
constexpr uint32_t kVoiceHoldUs = 900000u;
constexpr uint32_t kReconnectMs = 10000u;
// SAS ACL requires the claimed role to match the role registered in the
// certificate; on authentication refusal each remaining role is tried once,
// starting from the initial role (mirrors the nrl-pulse ROLE_SEQ logic).
constexpr const char *kRoleSequence[] = {"user", "super", "admin"};
constexpr size_t kRoleCount = sizeof(kRoleSequence) / sizeof(kRoleSequence[0]);
constexpr const char *kDiscoveryHost = "rotate.aprs2.net";
constexpr const char *kDiscoveryPort = "10152";

struct PersistedConfigV1 {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t transmit;
    FmoServer server;
    uint32_t crc32;
};

struct PersistedConfig {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t transmit;
    uint8_t mqtt_no_local;
    uint8_t reserved[3];
    FmoServer server;
    uint32_t crc32;
};

struct ServerCacheHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t count;
    uint32_t reserved;
};
static_assert(sizeof(ServerCacheHeader) == 16u, "FMO server cache header size");
static_assert(sizeof(ServerCacheHeader) + kServerMax * sizeof(FmoServer) <=
                  64u * 1024u,
              "FMO server cache must fit the server-list store payload limit");

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static FmoConfig s_config = {};
static FmoLinkStatus s_status = {};
static uint32_t s_config_generation = 1u;
static uint32_t s_active_generation = 0u;
static int64_t s_voice_until_us = 0;
static int64_t s_retry_at_us = 0;
static bool s_recreate_client = false;
static bool s_initialized = false;
static bool s_config_loaded = false;
static uint16_t s_client_suffix = 0u;
static size_t s_role_index = 0u;   // current role within kRoleSequence
static uint8_t s_role_tried = 0u;  // bitmask of roles already tried; 0 = derive initial
static bool s_role_retry = false;  // reconnect immediately with the next role

static SemaphoreHandle_t s_client_mutex = nullptr;
static SemaphoreHandle_t s_server_mutex = nullptr;
static esp_mqtt_client_handle_t s_client = nullptr;
static TaskHandle_t s_control_task = nullptr;
static TaskHandle_t s_discovery_task = nullptr;

NRL_PSRAM_BSS static uint8_t s_raw[kRawMaxSize];
static size_t s_raw_size = 0u;
static size_t s_raw_expected = 0u;
// FMO/QSO/UID/<本机uid> 记录 JSON 的分块重组缓冲（载荷很小）。
NRL_PSRAM_BSS static char s_qso_record[2048];
static size_t s_qso_record_size = 0u;
static size_t s_qso_record_expected = 0u;
NRL_PSRAM_BSS static FmoServer s_servers[kServerMax];
static size_t s_server_count = 0u;
static uint32_t s_server_generation = 0u;
static int64_t s_server_save_due_us = 0;
static int64_t s_server_last_save_us = 0;

static OpusVoiceDec *s_opus_decoder = nullptr;
static OpusVoiceEnc *s_opus_encoder = nullptr;
static bool s_tx_active = false;
static volatile bool s_ptt_held = false;
static int16_t s_tx_pcm[kOpusFrameSamples];
static size_t s_tx_pcm_count = 0u;
NRL_PSRAM_BSS static uint8_t
    s_tx_packets[kTxPacketsPerFrame][OPUS_VOICE_MAX_FRAME_BYTES];
static size_t s_tx_packet_sizes[kTxPacketsPerFrame];
static size_t s_tx_packet_count = 0u;
static uint32_t s_tx_packet_total = 0u;
static uint32_t s_tx_frame_count = 0u;
static uint16_t s_tx_session = 0u;
static uint32_t s_tx_started_ms = 0u;
static char s_tx_callsign[16] = {};
NRL_PSRAM_BSS static uint8_t s_tx_frame[kRawMaxSize];


static uint32_t crcConfig(const PersistedConfig *config)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(config),
                            offsetof(PersistedConfig, crc32));
}

static uint32_t crcConfigV1(const PersistedConfigV1 *config)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(config),
                            offsetof(PersistedConfigV1, crc32));
}

static void normalizeServer(FmoServer *server)
{
    server->name[sizeof(server->name) - 1u] = '\0';
    server->host[sizeof(server->host) - 1u] = '\0';
    server->callsign[sizeof(server->callsign) - 1u] = '\0';
    if (server->host[0] == '\0' || server->port == 0u || server->uid == 0u ||
        server->callsign[0] == '\0') {
        server->has_fingerprint = false;
    }
}

static bool serverNameIsCallsign(const FmoServer &server)
{
    return server.name[0] == '\0' ||
           strcasecmp(server.name, server.callsign) == 0;
}

static bool repairIncompleteCachedName(FmoServer *server)
{
    if (server == nullptr || !serverNameIsCallsign(*server)) return false;
    // Migration seed from the reference project's captured real
    // FMO-V4,STATION beacon. UID is the stable identity; a later live APRS
    // name remains authoritative and will overwrite this normally.
    if (server->uid == 447u && strcasecmp(server->callsign, "BG9JYT") == 0) {
        snprintf(server->name, sizeof(server->name), "%s", "如意甘肃");
        return true;
    }
    return false;
}

static bool loadConfig(void)
{
    PersistedConfig stored = {};
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(stored);
    const esp_err_t error = nvs_get_blob(nvs, kNvsConfigKey, &stored, &size);
    nvs_close(nvs);
    if (error != ESP_OK) {
        return false;
    }
    if (size == sizeof(PersistedConfigV1)) {
        const auto *legacy = reinterpret_cast<const PersistedConfigV1 *>(&stored);
        if (legacy->magic != kConfigMagic || legacy->version != 1u ||
            legacy->crc32 != crcConfigV1(legacy)) {
            return false;
        }
        s_config.enabled = legacy->enabled != 0u;
        s_config.transmit = legacy->transmit != 0u;
        s_config.mqtt_no_local = true;
        s_config.server = legacy->server;
        normalizeServer(&s_config.server);
        return true;
    }
    if (size != sizeof(stored) || stored.magic != kConfigMagic ||
        stored.version != kConfigVersion || stored.crc32 != crcConfig(&stored)) {
        return false;
    }
    s_config.enabled = stored.enabled != 0u;
    s_config.transmit = stored.transmit != 0u;
    s_config.mqtt_no_local = stored.mqtt_no_local != 0u;
    s_config.server = stored.server;
    normalizeServer(&s_config.server);
    return true;
}

static void ensureConfigLoaded(void)
{
    if (s_config_loaded) return;
    s_config.mqtt_no_local = true;
    (void)loadConfig();
    s_config_loaded = true;
}

static bool saveConfig(const FmoConfig &config)
{
    PersistedConfig stored = {};
    stored.magic = kConfigMagic;
    stored.version = kConfigVersion;
    stored.enabled = config.enabled ? 1u : 0u;
    stored.transmit = config.transmit ? 1u : 0u;
    stored.mqtt_no_local = config.mqtt_no_local ? 1u : 0u;
    stored.server = config.server;
    stored.crc32 = crcConfig(&stored);
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_blob(nvs, kNvsConfigKey, &stored,
                                 sizeof(stored)) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

static void refreshConfiguredServerMetadata(const FmoServer &server)
{
    bool persist_name = false;
    FmoConfig snapshot = {};
    portENTER_CRITICAL(&s_lock);
    if (s_config.server.uid != 0u && s_config.server.uid == server.uid) {
        // A legacy/incomplete cache often stores callsign in the name field.
        // It must never downgrade a friendly name already learned from a full
        // APRS STATION or MQTT SERVER_INFO record.
        const bool would_downgrade = serverNameIsCallsign(server) &&
                                     !serverNameIsCallsign(s_config.server);
        if (!would_downgrade && server.name[0] != '\0' &&
            strcmp(s_config.server.name, server.name) != 0) {
            snprintf(s_config.server.name, sizeof(s_config.server.name), "%s",
                     server.name);
            persist_name = true;
        }
        s_config.server.online = server.online;
        s_config.server.total = server.total;
        s_config.server.last_seen = server.last_seen;
        snapshot = s_config;
    }
    portEXIT_CRITICAL(&s_lock);
    if (persist_name && saveConfig(snapshot)) {
        CONFIG_NOTIFY_Bump();
        ESP_LOGI(TAG, "configured FMO server name updated: %s", server.name);
    }
}

static void applyRoutes(const FmoConfig &config)
{
    const bool fmo_tx = config.enabled && config.transmit;
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_FMO_UPLINK, fmo_tx);
    AudioRouter_SetRoute(AUDIO_SRC_BT_HFP_MIC, AUDIO_SINK_FMO_UPLINK, fmo_tx);
    // Both network sinks stay connected to the capture stream. Their PTT gates
    // are mutually exclusive, which also lets the S31's dedicated NRL/FMO
    // screen buttons work without changing the physical-button target.
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_NRL_UPLINK, true);
    AudioRouter_SetRoute(AUDIO_SRC_BT_HFP_MIC, AUDIO_SINK_NRL_UPLINK, true);
    AudioRouter_SetRoute(AUDIO_SRC_FMO_DOWNLINK, AUDIO_SINK_SPEAKER, true);
}

static bool serverUsable(const FmoServer &server)
{
    return server.host[0] != '\0' && server.port != 0u && server.uid != 0u &&
           server.callsign[0] != '\0' && server.has_fingerprint;
}

static void loadServerCache(void)
{
    uint8_t *payload = nullptr;
    size_t payload_size = 0u;
    if (!SERVER_LIST_STORE_Read(SERVER_LIST_FMO, &payload, &payload_size) ||
        payload_size < sizeof(ServerCacheHeader)) {
        free(payload);
        return;
    }
    const auto *header = reinterpret_cast<const ServerCacheHeader *>(payload);
    const size_t expected = sizeof(ServerCacheHeader) +
                            static_cast<size_t>(header->count) * sizeof(FmoServer);
    if (header->magic != kServerCacheMagic ||
        header->version != kServerCacheVersion ||
        header->record_size != sizeof(FmoServer) || header->count > kServerMax ||
        expected != payload_size) {
        ESP_LOGW(TAG, "ignoring incompatible FMO server cache");
        free(payload);
        return;
    }
    const auto *records = reinterpret_cast<const FmoServer *>(
        payload + sizeof(ServerCacheHeader));
    size_t loaded = 0u;
    size_t incomplete_names = 0u;
    size_t repaired_names = 0u;
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    for (size_t i = 0u; i < header->count; ++i) {
        FmoServer server = records[i];
        normalizeServer(&server);
        if (!serverUsable(server)) continue;
        if (server.name[0] == '\0') {
            snprintf(server.name, sizeof(server.name), "%s", server.callsign);
        }
        if (repairIncompleteCachedName(&server)) ++repaired_names;
        if (serverNameIsCallsign(server)) ++incomplete_names;
        s_servers[loaded++] = server;
    }
    s_server_count = loaded;
    if (repaired_names > 0u) {
        ++s_server_generation;
        s_server_save_due_us = esp_timer_get_time() + kServerSaveDelayUs;
    }
    xSemaphoreGive(s_server_mutex);
    free(payload);
    if (loaded > 0u) {
        ESP_LOGI(TAG, "restored %u FMO servers from flash (%u names await APRS)",
                 static_cast<unsigned>(loaded),
                 static_cast<unsigned>(incomplete_names));
        if (repaired_names > 0u) {
            ESP_LOGI(TAG, "repaired %u incomplete FMO cache names from APRS seed",
                     static_cast<unsigned>(repaired_names));
        }
    }
}

static void scheduleServerCacheSaveLocked(const int64_t now)
{
    ++s_server_generation;
    if (s_server_save_due_us != 0) return;
    int64_t due = now + kServerSaveDelayUs;
    if (s_server_last_save_us != 0 &&
        due < s_server_last_save_us + kServerSaveMinIntervalUs) {
        due = s_server_last_save_us + kServerSaveMinIntervalUs;
    }
    s_server_save_due_us = due;
}

static void persistServerCacheIfDue(void)
{
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    if (s_server_save_due_us == 0 || now < s_server_save_due_us) {
        xSemaphoreGive(s_server_mutex);
        return;
    }
    const size_t count = s_server_count;
    const uint32_t generation = s_server_generation;
    const size_t payload_size = sizeof(ServerCacheHeader) + count * sizeof(FmoServer);
    uint8_t *payload = static_cast<uint8_t *>(
        heap_caps_malloc(payload_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (payload != nullptr) {
        auto *header = reinterpret_cast<ServerCacheHeader *>(payload);
        *header = {kServerCacheMagic, kServerCacheVersion,
                   static_cast<uint16_t>(sizeof(FmoServer)),
                   static_cast<uint32_t>(count), 0u};
        memcpy(payload + sizeof(*header), s_servers, count * sizeof(FmoServer));
    }
    xSemaphoreGive(s_server_mutex);
    if (payload == nullptr) {
        ESP_LOGW(TAG, "FMO server cache snapshot allocation failed");
        return;
    }
    const bool saved = SERVER_LIST_STORE_Write(SERVER_LIST_FMO, payload,
                                                payload_size);
    free(payload);
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    if (saved) {
        s_server_last_save_us = now;
        s_server_save_due_us = s_server_generation == generation
                                   ? 0
                                   : now + kServerSaveMinIntervalUs;
    } else {
        s_server_save_due_us = now + kServerSaveMinIntervalUs;
    }
    xSemaphoreGive(s_server_mutex);
    if (saved) ESP_LOGI(TAG, "persisted %u FMO servers",
                        static_cast<unsigned>(count));
}

static void configSnapshot(FmoConfig *config, uint32_t *generation)
{
    portENTER_CRITICAL(&s_lock);
    *config = s_config;
    if (generation != nullptr) *generation = s_config_generation;
    portEXIT_CRITICAL(&s_lock);
}

static void setLastError(const esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.last_error = static_cast<int>(error);
    portEXIT_CRITICAL(&s_lock);
}

// Compare base callsigns only: any "-SSID" suffix and letter case are
// ignored ("BG9JYT-14" == "bg9jyt").  The server list may carry an SSID
// while the certificate never does, so own-server checks must strip it.
static bool baseCallsEqual(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) return false;
    size_t na = 0, nb = 0;
    while (a[na] != '\0' && a[na] != '-') ++na;
    while (b[nb] != '\0' && b[nb] != '-') ++nb;
    return na > 0 && na == nb && strncasecmp(a, b, na) == 0;
}

static size_t initialRoleIndex(const FmoServer &server)
{
    // Own server (certificate callsign matches the server callsign) starts as
    // "super", anything else as "user"; compared case-insensitively.
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) == ESP_OK && identity.ready &&
        baseCallsEqual(identity.callsign, server.callsign)) {
        return 1u; // kRoleSequence[1] == "super"
    }
    return 0u;     // kRoleSequence[0] == "user"
}

static const char *currentRole(const FmoServer &server)
{
    portENTER_CRITICAL(&s_lock);
    const uint8_t tried = s_role_tried;
    size_t index = s_role_index;
    portEXIT_CRITICAL(&s_lock);
    if (tried == 0u) {
        index = initialRoleIndex(server);
        portENTER_CRITICAL(&s_lock);
        s_role_index = index;
        s_role_tried = static_cast<uint8_t>(1u << index);
        portEXIT_CRITICAL(&s_lock);
    }
    return kRoleSequence[index];
}

static void resetRoleState(void)
{
    // Cleared to 0 so the next startClient re-derives the initial role.
    portENTER_CRITICAL(&s_lock);
    s_role_tried = 0u;
    portEXIT_CRITICAL(&s_lock);
}

static void rotateRole(const int return_code)
{
    size_t current = 0u;
    uint8_t tried = 0u;
    portENTER_CRITICAL(&s_lock);
    current = s_role_index;
    tried = s_role_tried;
    portEXIT_CRITICAL(&s_lock);
    for (size_t i = 0u; i < kRoleCount; ++i) {
        if ((tried & static_cast<uint8_t>(1u << i)) != 0u) continue;
        ESP_LOGW(TAG, "role %s rejected by server (code=%d), retrying as %s",
                 kRoleSequence[current], return_code, kRoleSequence[i]);
        portENTER_CRITICAL(&s_lock);
        s_role_index = i;
        s_role_tried = tried | static_cast<uint8_t>(1u << i);
        s_role_retry = true;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    ESP_LOGE(TAG, "authentication rejected for all roles (user/super/admin)");
    resetRoleState();
}

static size_t decodeAdpcm(const uint8_t *data, const size_t data_size,
                          const int16_t sample, const uint8_t index,
                          int16_t *pcm, const size_t capacity)
{
    return FMO_ADPCM_Decode(data, data_size, sample, index, pcm, capacity);
}

static bool decodeBlock(void *, const FmoFrameCodec codec, const uint8_t *data,
                        const size_t data_size, const int16_t adpcm_sample,
                        const uint8_t adpcm_index)
{
    int16_t pcm[960];
    int samples = -1;
    if (codec == FMO_FRAME_ADPCM) {
        samples = static_cast<int>(decodeAdpcm(data, data_size, adpcm_sample,
                                               adpcm_index, pcm,
                                               sizeof(pcm) / sizeof(pcm[0])));
    } else if (codec == FMO_FRAME_OPUS && s_opus_decoder != nullptr) {
        samples = OPUS_VOICE_DecProcess(s_opus_decoder, data, data_size, pcm,
                                        sizeof(pcm) / sizeof(pcm[0]));
    }
    if (samples <= 0) return true;
    AudioRouter_PushFrame(AUDIO_SRC_FMO_DOWNLINK, 8000u, pcm,
                          static_cast<size_t>(samples));
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.voice_codec, sizeof(s_status.voice_codec), "%s",
             codec == FMO_FRAME_ADPCM ? "ADPCM" : "OPUS");
    s_voice_until_us = esp_timer_get_time() + kVoiceHoldUs;
    portEXIT_CRITICAL(&s_lock);
    STATUS_IO_SetFmoPttActive(true);
    return true;
}

static void processRaw(const uint8_t *data, const size_t size)
{
    FmoFrameInfo info = {};
    if (!FMO_FRAME_Parse(data, size, &info, decodeBlock, nullptr)) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.parse_errors;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.voice_callsign, sizeof(s_status.voice_callsign), "%s",
             info.callsign);
    s_status.receiving = true;
    ++s_status.rx_frames;
    s_voice_until_us = esp_timer_get_time() + kVoiceHoldUs;
    portEXIT_CRITICAL(&s_lock);
    STATUS_IO_SetFmoPttActive(true);
}

static bool rawTopic(const esp_mqtt_event_t *event)
{
    static const char topic[] = "FMO/RAW";
    return event->topic != nullptr &&
           event->topic_len == static_cast<int>(sizeof(topic) - 1u) &&
           memcmp(event->topic, topic, sizeof(topic) - 1u) == 0;
}

// "FMO/LATE/UID_V1/<uid>" heartbeat topic: parse the trailing decimal uid
// (uid 0 counts too). Only the first data chunk carries the topic.
static bool heartbeatUid(const esp_mqtt_event_t *event, uint32_t *uid)
{
    static const char prefix[] = "FMO/LATE/UID_V1/";
    if (event->topic == nullptr || event->current_data_offset != 0 ||
        event->topic_len <= static_cast<int>(sizeof(prefix) - 1u) ||
        memcmp(event->topic, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }
    // event->topic is not NUL-terminated; copy the uid suffix first.
    char tail[12];
    const size_t tail_len =
        static_cast<size_t>(event->topic_len) - (sizeof(prefix) - 1u);
    if (tail_len == 0u || tail_len >= sizeof(tail)) return false;
    memcpy(tail, event->topic + sizeof(prefix) - 1u, tail_len);
    tail[tail_len] = '\0';
    char *end = nullptr;
    const unsigned long parsed = strtoul(tail, &end, 10);
    if (end == tail || *end != '\0' || parsed > UINT32_MAX) return false;
    *uid = static_cast<uint32_t>(parsed);
    return true;
}

// "FMO/QSO/UID/<uid>" established-QSO record topic (only our own uid is
// subscribed). Only the first data chunk carries the topic.
static bool qsoRecordTopic(const esp_mqtt_event_t *event)
{
    static const char prefix[] = "FMO/QSO/UID/";
    return event->topic != nullptr && event->current_data_offset == 0 &&
           event->topic_len > static_cast<int>(sizeof(prefix) - 1u) &&
           memcmp(event->topic, prefix, sizeof(prefix) - 1u) == 0;
}

static void mqttEvent(void *, esp_event_base_t, const int32_t event_id,
                      void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_t *>(event_data);
    if (event_id == MQTT_EVENT_CONNECTED) {
        bool no_local = true;
        portENTER_CRITICAL(&s_lock);
        no_local = s_config.mqtt_no_local;
        portEXIT_CRITICAL(&s_lock);
        esp_mqtt5_subscribe_property_config_t property = {};
        property.no_local_flag = no_local;
        const esp_err_t property_error =
            esp_mqtt5_client_set_subscribe_property(event->client, &property);
        if (property_error != ESP_OK) {
            ESP_LOGW(TAG, "failed to set MQTT 5 No Local: %s",
                     esp_err_to_name(property_error));
        }
        (void)esp_mqtt_client_subscribe(event->client, "FMO/RAW", 0);
        // Online-count source: every device heartbeats here ~once a minute.
        (void)esp_mqtt_client_subscribe(event->client, "FMO/LATE/UID_V1/#", 0);
        // Established-QSO records addressed to us (FMO QSO signaling).
        {
            FmoIdentityStatus identity = {};
            if (FMO_CERT_GetStatus(&identity) == ESP_OK && identity.ready) {
                char topic[40];
                snprintf(topic, sizeof(topic), "FMO/QSO/UID/%lu",
                         static_cast<unsigned long>(identity.uid));
                (void)esp_mqtt_client_subscribe(event->client, topic, 0);
            }
        }
        portENTER_CRITICAL(&s_lock);
        s_status.connected = true;
        s_status.last_error = ESP_OK;
        // Snapshot the role this connection actually logged in with BEFORE
        // dropping the rotation state: s_role_index still holds the role
        // picked by currentRole() for this CONNECT.
        snprintf(s_status.role, sizeof(s_status.role), "%s",
                 kRoleSequence[s_role_index]);
        s_recreate_client = false;
        s_role_tried = 0u; // drop rotation state; next reconnect re-derives the initial role
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "connected and subscribed to FMO/RAW + FMO/LATE/UID_V1/# (No Local=%u)",
                 no_local ? 1u : 0u);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        FMO_STATION_BCAST_RosterReset(); // drop the online roster (peak kept)
        portENTER_CRITICAL(&s_lock);
        s_status.connected = false;
        s_status.receiving = false;
        s_status.transmitting = false;
        s_status.role[0] = '\0';
        if (s_status.last_error == ESP_OK) s_status.last_error = ESP_FAIL;
        s_recreate_client = true;
        s_retry_at_us = esp_timer_get_time() +
                        (s_role_retry ? 0 : kReconnectMs * 1000LL);
        s_role_retry = false;
        portEXIT_CRITICAL(&s_lock);
        STATUS_IO_SetFmoPttActive(false);
    } else if (event_id == MQTT_EVENT_DATA) {
        if (rawTopic(event)) {
            if (event->current_data_offset == 0) {
                s_raw_size = 0u;
                s_raw_expected = event->total_data_len > 0 &&
                                         event->total_data_len <= static_cast<int>(kRawMaxSize)
                                     ? static_cast<size_t>(event->total_data_len)
                                     : 0u;
            }
            if (s_raw_expected == 0u || event->data_len <= 0 ||
                static_cast<size_t>(event->current_data_offset) != s_raw_size ||
                s_raw_size + static_cast<size_t>(event->data_len) > s_raw_expected) {
                s_raw_size = 0u;
                s_raw_expected = 0u;
                return;
            }
            memcpy(s_raw + s_raw_size, event->data,
                   static_cast<size_t>(event->data_len));
            s_raw_size += static_cast<size_t>(event->data_len);
            if (s_raw_size == s_raw_expected) {
                processRaw(s_raw, s_raw_size);
                s_raw_size = 0u;
                s_raw_expected = 0u;
            }
        } else if (qsoRecordTopic(event)) {
            // 分块重组（与 FMO/RAW 相同的模式，缓冲小得多）。
            if (event->current_data_offset == 0) {
                s_qso_record_size = 0u;
                s_qso_record_expected =
                    event->total_data_len > 0 &&
                            event->total_data_len <
                                static_cast<int>(sizeof(s_qso_record))
                        ? static_cast<size_t>(event->total_data_len)
                        : 0u;
            }
            if (s_qso_record_expected == 0u || event->data_len <= 0 ||
                static_cast<size_t>(event->current_data_offset) !=
                    s_qso_record_size ||
                s_qso_record_size + static_cast<size_t>(event->data_len) >
                    s_qso_record_expected) {
                s_qso_record_size = 0u;
                s_qso_record_expected = 0u;
                return;
            }
            memcpy(s_qso_record + s_qso_record_size, event->data,
                   static_cast<size_t>(event->data_len));
            s_qso_record_size += static_cast<size_t>(event->data_len);
            if (s_qso_record_size == s_qso_record_expected) {
                FMO_QSO_OnMqttRecord(s_qso_record,
                                     static_cast<int>(s_qso_record_size));
                s_qso_record_size = 0u;
                s_qso_record_expected = 0u;
            }
        } else {
            uint32_t uid = 0u;
            if (heartbeatUid(event, &uid)) {
                FMO_STATION_BCAST_FeedHeartbeat(uid);
            }
        }
    } else if (event_id == MQTT_EVENT_ERROR) {
        setLastError(ESP_FAIL);
        // CONNACK refusal: v3.1.1 reports 4/5, v5 (used here) reports 0x86/0x87.
        const esp_mqtt_error_codes_t *handle = event->error_handle;
        const int return_code = handle != nullptr
                                    ? static_cast<int>(handle->connect_return_code)
                                    : 0;
        if (handle != nullptr &&
            handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            (return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
             return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ||
             return_code == MQTT5_BAD_USERNAME_OR_PWD ||
             return_code == MQTT5_NOT_AUTHORIZED)) {
            rotateRole(return_code);
        } else {
            ESP_LOGW(TAG, "MQTT transport/authentication error");
        }
    }
}

static void stopClient(void)
{
    if (s_client_mutex != nullptr) xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client != nullptr) {
        (void)esp_mqtt_client_stop(s_client);
        (void)esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
    }
    if (s_client_mutex != nullptr) xSemaphoreGive(s_client_mutex);
    portENTER_CRITICAL(&s_lock);
    s_status.connected = false;
    s_status.receiving = false;
    s_status.transmitting = false;
    s_status.role[0] = '\0';
    s_active_generation = 0u;
    portEXIT_CRITICAL(&s_lock);
    FMO_STATION_BCAST_RosterReset();
    STATUS_IO_SetFmoPttActive(false);
}

static esp_err_t startClient(const FmoConfig &config,
                             const uint32_t generation)
{
    if (!config.enabled || !serverUsable(config.server)) {
        return ESP_ERR_INVALID_STATE;
    }
    char username[16], client_id[48];
    char *password = nullptr;
    const char *role = currentRole(config.server);
    esp_err_t error = FMO_CERT_BuildCredentials(&config.server, role, username,
                                                sizeof(username), &password);
    if (error != ESP_OK) return error;
    snprintf(client_id, sizeof(client_id), "FMO-%s-%lu-%04X", username,
             static_cast<unsigned long>(config.server.uid),
             static_cast<unsigned>(s_client_suffix));
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.client_id, sizeof(s_status.client_id), "%s", client_id);
    portEXIT_CRITICAL(&s_lock);

    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.broker.address.hostname = config.server.host;
    mqtt_config.broker.address.port = config.server.port;
    mqtt_config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    mqtt_config.credentials.username = username;
    mqtt_config.credentials.client_id = client_id;
    mqtt_config.credentials.authentication.password = password;
    mqtt_config.session.keepalive = 60;
    mqtt_config.session.protocol_ver = MQTT_PROTOCOL_V_5;
    mqtt_config.network.reconnect_timeout_ms = kReconnectMs;
    mqtt_config.network.timeout_ms = 10000;
    mqtt_config.network.disable_auto_reconnect = true;
    mqtt_config.task.stack_size = 8192;
    mqtt_config.buffer.size = kRawMaxSize;
    mqtt_config.buffer.out_size = kRawMaxSize;
    mqtt_config.outbox.limit = 16u * 1024u;

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client = esp_mqtt_client_init(&mqtt_config);
    sodium_memzero(password, strlen(password));
    free(password);
    if (s_client == nullptr) {
        xSemaphoreGive(s_client_mutex);
        return ESP_ERR_NO_MEM;
    }
    error = esp_mqtt_client_register_event(
        s_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                           mqttEvent, nullptr);
    if (error == ESP_OK) error = esp_mqtt_client_start(s_client);
    if (error != ESP_OK) {
        (void)esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        xSemaphoreGive(s_client_mutex);
        return error;
    }
    xSemaphoreGive(s_client_mutex);
    portENTER_CRITICAL(&s_lock);
    s_status.configured = true;
    s_status.last_error = ESP_OK;
    s_active_generation = generation;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "connecting %s (%s:%u, uid=%lu, client id=%s)", config.server.name,
             config.server.host, static_cast<unsigned>(config.server.port),
             static_cast<unsigned long>(config.server.uid), client_id);
    return ESP_OK;
}

static void startTimeSyncIfNeeded(void)
{
    (void)TIME_SYNC_StartIfNeeded();
}

static bool txFlush(void)
{
    if (s_tx_packet_count == 0u) return true;
    const uint8_t *packets[kTxPacketsPerFrame];
    for (size_t i = 0; i < s_tx_packet_count; ++i) packets[i] = s_tx_packets[i];
    const size_t frame_size = FMO_FRAME_BuildOpus(
        s_tx_frame, sizeof(s_tx_frame), s_tx_callsign, s_tx_session,
        s_tx_started_ms, s_tx_started_ms + s_tx_packet_total * 40u, packets,
        s_tx_packet_sizes, s_tx_packet_count, s_tx_frame_count < 3u ? 0u : 9u);
    if (frame_size == 0u) return false;
    bool connected = false;
    portENTER_CRITICAL(&s_lock);
    connected = s_status.connected;
    portEXIT_CRITICAL(&s_lock);
    bool published = false;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client != nullptr && connected) {
        published = esp_mqtt_client_publish(
                        s_client, "FMO/RAW",
                        reinterpret_cast<const char *>(s_tx_frame),
                        static_cast<int>(frame_size), 0, 0) >= 0;
    }
    xSemaphoreGive(s_client_mutex);
    s_tx_packet_total += static_cast<uint32_t>(s_tx_packet_count);
    ++s_tx_frame_count;
    s_tx_packet_count = 0u;
    return published;
}

static bool txEncodePacket(void)
{
    if (s_tx_packet_count >= kTxPacketsPerFrame || s_opus_encoder == nullptr) {
        return false;
    }
    const int encoded = OPUS_VOICE_EncProcess(
        s_opus_encoder, s_tx_pcm, kOpusFrameSamples,
        s_tx_packets[s_tx_packet_count], sizeof(s_tx_packets[s_tx_packet_count]));
    s_tx_pcm_count = 0u;
    if (encoded <= 0) return false;
    s_tx_packet_sizes[s_tx_packet_count++] = static_cast<size_t>(encoded);
    return s_tx_packet_count < kTxPacketsPerFrame || txFlush();
}

static bool txBegin(void)
{
    FmoIdentityStatus identity = {};
    FmoLinkStatus link = {};
    FMO_GetLinkStatus(&link);
    if (!link.connected || FMO_CERT_GetStatus(&identity) != ESP_OK ||
        !identity.ready) {
        return false;
    }
    s_tx_active = true;
    s_tx_pcm_count = 0u;
    s_tx_packet_count = 0u;
    s_tx_packet_total = 0u;
    s_tx_frame_count = 0u;
    s_tx_session = static_cast<uint16_t>(esp_random());
    if (s_tx_session == 0u) s_tx_session = 1u;
    const uint64_t epoch_ms = static_cast<uint64_t>(time(nullptr)) * 1000ULL +
                              static_cast<uint64_t>(esp_timer_get_time() / 1000) % 1000ULL;
    s_tx_started_ms = static_cast<uint32_t>(epoch_ms);
    snprintf(s_tx_callsign, sizeof(s_tx_callsign), "%s", identity.callsign);
    portENTER_CRITICAL(&s_lock);
    s_status.transmitting = true;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void txEnd(void)
{
    if (!s_tx_active) return;
    if (s_tx_pcm_count > 0u) {
        const int16_t last = s_tx_pcm[s_tx_pcm_count - 1u];
        while (s_tx_pcm_count < kOpusFrameSamples) s_tx_pcm[s_tx_pcm_count++] = last;
        (void)txEncodePacket();
    }
    (void)txFlush();
    s_tx_active = false;
    portENTER_CRITICAL(&s_lock);
    s_status.transmitting = false;
    portEXIT_CRITICAL(&s_lock);
}

static void fmoUplinkSink(const uint8_t source_id, const int16_t *samples,
                          const size_t sample_count, void *)
{
    if (source_id == AUDIO_SRC_MIC && NRL_BtHfp_IsConnected()) return;
    FmoConfig config = {};
    configSnapshot(&config, nullptr);
    const bool gate = config.enabled && config.transmit &&
                      (s_ptt_held ||
                       (ESPNOW_LINK_GetPttMode() == 2u &&
                        STATUS_IO_IsSqlActive()));
    if (!gate) {
        txEnd();
        return;
    }
    if (!s_tx_active && !txBegin()) return;
    for (size_t offset = 0u; offset < sample_count; ++offset) {
        s_tx_pcm[s_tx_pcm_count++] = samples[offset];
        if (s_tx_pcm_count == kOpusFrameSamples && !txEncodePacket()) {
            txEnd();
            return;
        }
    }
}

static void controlTask(void *)
{
    for (;;) {
        const int64_t now = esp_timer_get_time();
        bool expire_voice = false;
        portENTER_CRITICAL(&s_lock);
        if (s_voice_until_us != 0 && now > s_voice_until_us) {
            s_voice_until_us = 0;
            s_status.receiving = false;
            s_status.voice_callsign[0] = '\0';
            s_status.voice_codec[0] = '\0';
            expire_voice = true;
        }
        const bool recreate = s_recreate_client;
        const int64_t retry_at = s_retry_at_us;
        portEXIT_CRITICAL(&s_lock);
        if (expire_voice) STATUS_IO_SetFmoPttActive(false);

        FmoConfig config = {};
        uint32_t generation = 0u;
        configSnapshot(&config, &generation);
        portENTER_CRITICAL(&s_lock);
        s_status.configured = serverUsable(config.server);
        portEXIT_CRITICAL(&s_lock);
        if (nrlNetworkConnected()) startTimeSyncIfNeeded();

        const bool should_run = config.enabled && serverUsable(config.server) &&
                                nrlNetworkConnected();
        if (!should_run) {
            if (s_client != nullptr) stopClient();
        } else if (s_client != nullptr &&
                   (generation != s_active_generation || recreate) &&
                   now >= retry_at) {
            stopClient();
        }
        if (should_run && s_client == nullptr && now >= retry_at) {
            const esp_err_t error = startClient(config, generation);
            if (error != ESP_OK) {
                setLastError(error);
                portENTER_CRITICAL(&s_lock);
                s_retry_at_us = esp_timer_get_time() + kReconnectMs * 1000LL;
                s_recreate_client = false;
                portEXIT_CRITICAL(&s_lock);
                ESP_LOGW(TAG, "connect setup failed: %s", esp_err_to_name(error));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static uint16_t aprsPasscode(const char *callsign)
{
    uint16_t hash = 0x73e2u;
    char root[10] = {};
    size_t length = 0u;
    while (callsign[length] != '\0' && callsign[length] != '-' &&
           length + 1u < sizeof(root)) {
        root[length] = static_cast<char>(toupper(
            static_cast<unsigned char>(callsign[length])));
        ++length;
    }
    for (size_t i = 0u; i < length; i += 2u) {
        hash ^= static_cast<uint16_t>(static_cast<uint8_t>(root[i]) << 8u);
        if (i + 1u < length) hash ^= static_cast<uint8_t>(root[i + 1u]);
    }
    return hash & 0x7fffu;
}

static int connectDiscovery(void)
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(kDiscoveryHost, kDiscoveryPort, &hints, &result) != 0 ||
        result == nullptr) {
        return -1;
    }
    const int fd = socket(result->ai_family, result->ai_socktype,
                          result->ai_protocol);
    if (fd < 0 || connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
        if (fd >= 0) close(fd);
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);
    FmoIdentityStatus identity = {};
    const bool have_identity = FMO_CERT_GetStatus(&identity) == ESP_OK &&
                               identity.ready;
    const ExternalRadioConfig *radio = EXTERNAL_RADIO_GetConfig();
    const char *callsign = have_identity ? identity.callsign :
                           radio != nullptr ? radio->callsign : "NOCALL";
    const unsigned ssid = radio != nullptr ? radio->callsign_ssid : 0u;
    char station[24];
    if (ssid > 0u && ssid <= 15u) {
        snprintf(station, sizeof(station), "%s-%u", callsign, ssid);
    } else {
        snprintf(station, sizeof(station), "%s", callsign);
    }
    const int passcode = strcasecmp(callsign, "NOCALL") == 0
                             ? -1 : static_cast<int>(aprsPasscode(callsign));
    char login[112];
    const int login_size = snprintf(login, sizeof(login),
        "user %s pass %d vers NRL-ESP32-FMO 1.0 filter d/APFMO4 d/APFMO0\r\n",
        station, passcode);
    if (login_size <= 0 || static_cast<size_t>(login_size) >= sizeof(login) ||
        send(fd, login, static_cast<size_t>(login_size), 0) != login_size) {
        close(fd);
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "APRS discovery connected as %s", station);
    return fd;
}

static bool looksLikeHost(const char *text)
{
    if (text == nullptr || text[0] == '\0' || strchr(text, '.') == nullptr ||
        strchr(text, ':') != nullptr) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p != 0u; ++p) {
        if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_') return false;
    }
    return true;
}

static bool parseUint(const char *text, uint32_t *value)
{
    if (text == nullptr || text[0] == '\0') return false;
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > UINT32_MAX) return false;
    *value = static_cast<uint32_t>(parsed);
    return true;
}

static bool parseStation(char *line, FmoServer *server)
{
    char *gt = strchr(line, '>');
    char *colon = strchr(line, ':');
    if (gt == nullptr || colon == nullptr || gt >= colon) return false;
    char *marker = strstr(colon + 1, "FMO-V4,STATION,");
    if (marker == nullptr) return false;
    marker += strlen("FMO-V4,STATION,");
    memset(server, 0, sizeof(*server));
    size_t source_size = static_cast<size_t>(gt - line);
    char *dash = static_cast<char *>(memchr(line, '-', source_size));
    if (dash != nullptr) source_size = static_cast<size_t>(dash - line);
    if (source_size > 0u && source_size < sizeof(server->callsign)) {
        memcpy(server->callsign, line, source_size);
        for (size_t i = 0; i < source_size; ++i) {
            server->callsign[i] = static_cast<char>(toupper(
                static_cast<unsigned char>(server->callsign[i])));
        }
    }
    const char *display_name = nullptr;
    FmoPublicCertificate cert = {};
    bool have_cert = false;
    char *cursor = marker;
    for (size_t token_index = 0u; cursor != nullptr && token_index < 24u;
         ++token_index) {
        char *token = cursor;
        char *comma = strchr(cursor, ',');
        if (comma != nullptr) {
            *comma = '\0';
            cursor = comma + 1;
        } else {
            cursor = nullptr;
        }
        if (strncmp(token, "CERT:", 5u) == 0) {
            have_cert = FMO_PROTOCOL_ParseBeaconCertificate(token + 5u, &cert);
        } else if (token[0] == 'P' && isdigit(static_cast<unsigned char>(token[1]))) {
            uint32_t port = 0u;
            if (parseUint(token + 1u, &port) && port <= 65535u) {
                server->port = static_cast<uint16_t>(port);
            }
        } else if (token[0] == 'U' && isdigit(static_cast<unsigned char>(token[1]))) {
            char *slash = strchr(token + 1u, '/');
            if (slash != nullptr) {
                *slash = '\0';
                (void)parseUint(token + 1u, &server->online);
                (void)parseUint(slash + 1u, &server->total);
            }
        } else if (strncmp(token, "SH:", 3u) == 0 && looksLikeHost(token + 3u)) {
            snprintf(server->host, sizeof(server->host), "%s", token + 3u);
        } else if (looksLikeHost(token)) {
            snprintf(server->host, sizeof(server->host), "%s", token);
        } else if (strncmp(token, "SIG:", 4u) != 0 && token[0] != 'F' &&
                   token[0] != '\0' && strlen(token) > 2u) {
            display_name = token;
        }
    }
    if (have_cert) {
        server->uid = cert.uid;
        snprintf(server->callsign, sizeof(server->callsign), "%s", cert.callsign);
        memcpy(server->fingerprint, cert.fingerprint, 32u);
        server->has_fingerprint = true;
    }
    if (!serverUsable(*server)) return false;
    // APRS discovery commonly carries Chinese station names in GBK. Convert
    // them to UTF-8 so LCD and Web show the actual server name, not just the
    // certificate callsign fallback.
    if (display_name != nullptr) {
        MEDIA_TEXT_8BitToUtf8(
            reinterpret_cast<const uint8_t *>(display_name), strlen(display_name),
            server->name, sizeof(server->name));
    }
    if (server->name[0] == '\0') {
        snprintf(server->name, sizeof(server->name), "%s", server->callsign);
    }
    server->last_seen = static_cast<int64_t>(time(nullptr));
    return true;
}

static void upsertServer(const FmoServer &server)
{
    FmoServer merged = server;
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    size_t target = s_server_count;
    for (size_t i = 0u; i < s_server_count; ++i) {
        if (s_servers[i].uid == server.uid) {
            target = i;
            break;
        }
    }
    if (target == s_server_count && s_server_count < kServerMax) ++s_server_count;
    if (target >= kServerMax) {
        target = 0u;
        for (size_t i = 1u; i < kServerMax; ++i) {
            if (s_servers[i].last_seen < s_servers[target].last_seen) target = i;
        }
    }
    // A number of FMO nodes temporarily advertise only their certificate
    // callsign as display_name. Do not let that lower-quality beacon erase a
    // friendly Chinese name already learned and persisted for the same UID.
    if (target < s_server_count && serverNameIsCallsign(merged) &&
        !serverNameIsCallsign(s_servers[target])) {
        snprintf(merged.name, sizeof(merged.name), "%s",
                 s_servers[target].name);
    }
    if (memcmp(&s_servers[target], &merged, sizeof(merged)) != 0) {
        s_servers[target] = merged;
        scheduleServerCacheSaveLocked(esp_timer_get_time());
    }
    xSemaphoreGive(s_server_mutex);
    refreshConfiguredServerMetadata(merged);
}

static void discoveryTask(void *)
{
    int fd = -1;
    int64_t retry_at = 0;
    char line[800];
    size_t used = 0u;
    for (;;) {
        persistServerCacheIfDue();
        if (!nrlNetworkConnected()) {
            if (fd >= 0) close(fd);
            fd = -1;
            used = 0u;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (fd < 0) {
            if (now < retry_at) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            fd = connectDiscovery();
            if (fd < 0) {
                retry_at = now + 30000000LL;
                continue;
            }
        }
        char buffer[512];
        const int received = recv(fd, buffer, sizeof(buffer), 0);
        if (received == 0 ||
            (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(fd);
            fd = -1;
            used = 0u;
            retry_at = now + 30000000LL;
            continue;
        }
        if (received < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        for (int i = 0; i < received; ++i) {
            const char ch = buffer[i];
            if (ch == '\r' || ch == '\n') {
                if (used > 0u) {
                    line[used] = '\0';
                    if (strstr(line, ">APFMO0,") != nullptr &&
                        strstr(line, "::") != nullptr) {
                        // QSO 呼叫信令（APRS 消息，TOCALL=APFMO0）。
                        FMO_QSO_HandleAprsLine(line);
                    }
                    FmoServer server = {};
                    if (strstr(line, "FMO-V4,STATION,") != nullptr &&
                        parseStation(line, &server)) {
                        upsertServer(server);
                        ESP_LOGI(TAG, "discovered %s name=\"%s\" %s:%u (%lu/%lu)",
                                 server.callsign, server.name, server.host,
                                 static_cast<unsigned>(server.port),
                                 static_cast<unsigned long>(server.online),
                                 static_cast<unsigned long>(server.total));
                    }
                    used = 0u;
                }
            } else if (used + 1u < sizeof(line)) {
                line[used++] = ch;
            } else {
                used = 0u;
            }
        }
    }
}

} // namespace

extern "C" bool FMO_Init(void)
{
    if (s_initialized) return true;
    s_client_suffix = static_cast<uint16_t>(esp_random());
    ensureConfigLoaded();
    s_client_mutex = xSemaphoreCreateMutex();
    s_server_mutex = xSemaphoreCreateMutex();
    s_opus_decoder = OPUS_VOICE_DecOpenEx(8000u, 40u);
    s_opus_encoder = OPUS_VOICE_EncOpenEx(8000u, 40u, 12000u);
    if (s_client_mutex == nullptr || s_server_mutex == nullptr ||
        s_opus_decoder == nullptr || s_opus_encoder == nullptr ||
        !AudioRouter_RegisterSink(AUDIO_SINK_FMO_UPLINK, 8000u,
                                  fmoUplinkSink, nullptr)) {
        ESP_LOGE(TAG, "initialization failed");
        return false;
    }
    loadServerCache();
    if (serverUsable(s_config.server)) {
        FmoServer matched_server = {};
        xSemaphoreTake(s_server_mutex, portMAX_DELAY);
        bool found = false;
        for (size_t i = 0u; i < s_server_count; ++i) {
            if (s_servers[i].uid == s_config.server.uid) {
                found = true;
                if (serverNameIsCallsign(s_servers[i]) &&
                    !serverNameIsCallsign(s_config.server)) {
                    snprintf(s_servers[i].name, sizeof(s_servers[i].name), "%s",
                             s_config.server.name);
                    scheduleServerCacheSaveLocked(esp_timer_get_time());
                }
                matched_server = s_servers[i];
                break;
            }
        }
        if (!found && s_server_count < kServerMax) {
            s_servers[s_server_count++] = s_config.server;
            scheduleServerCacheSaveLocked(esp_timer_get_time());
        }
        xSemaphoreGive(s_server_mutex);
        if (found) refreshConfiguredServerMetadata(matched_server);
    }
    applyRoutes(s_config);
    if ((!s_config.enabled || !s_config.transmit) &&
        ESPNOW_LINK_GetPttMode() == 2u) {
        ESPNOW_LINK_SetPttMode(0u);
    }
    if (xTaskCreate(controlTask, "fmo_link", 8192, nullptr, 4,
                    &s_control_task) != pdPASS ||
        xTaskCreate(discoveryTask, "fmo_discovery", 8192, nullptr, 3,
                    &s_discovery_task) != pdPASS) {
        ESP_LOGE(TAG, "worker task creation failed");
        return false;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "FMO-V4 service ready (enabled=%u tx=%u)",
             s_config.enabled ? 1u : 0u, s_config.transmit ? 1u : 0u);
    return true;
}

extern "C" void FMO_GetConfig(FmoConfig *config)
{
    if (config == nullptr) return;
    ensureConfigLoaded();
    configSnapshot(config, nullptr);
}

extern "C" bool FMO_SetConfig(const FmoConfig *config, const bool persist)
{
    if (config == nullptr) return false;
    ensureConfigLoaded();
    FmoConfig normalized = *config;
    normalizeServer(&normalized.server);
    if (normalized.enabled && !serverUsable(normalized.server)) return false;
    if (persist && !saveConfig(normalized)) return false;
    portENTER_CRITICAL(&s_lock);
    s_config = normalized;
    ++s_config_generation;
    s_recreate_client = true;
    s_retry_at_us = 0;
    portEXIT_CRITICAL(&s_lock);
    applyRoutes(normalized);
    if (!normalized.enabled || !normalized.transmit) txEnd();
    if (!normalized.enabled || !normalized.transmit) s_ptt_held = false;
    if ((!normalized.enabled || !normalized.transmit) &&
        ESPNOW_LINK_GetPttMode() == 2u) {
        ESPNOW_LINK_SetPttMode(0u);
    }
    CONFIG_NOTIFY_Bump();
    return true;
}

extern "C" void FMO_GetLinkStatus(FmoLinkStatus *status)
{
    if (status == nullptr) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

extern "C" bool FMO_IsSuperOnOwnServer(void)
{
    FmoLinkStatus link = {};
    FMO_GetLinkStatus(&link);
    // The role snapshot is written on CONNACK success, so this is the role
    // the server actually accepted -- not the role we would derive now.
    // "admin" is intentionally not accepted as "super" (see header comment).
    if (!link.connected || strcmp(link.role, "super") != 0) return false;
    FmoConfig config = {};
    configSnapshot(&config, nullptr);
    if (config.server.callsign[0] == '\0') return false;
    FmoIdentityStatus identity = {};
    return FMO_CERT_GetStatus(&identity) == ESP_OK && identity.ready &&
           baseCallsEqual(identity.callsign, config.server.callsign);
}

extern "C" bool FMO_IsTransmitSelected(void)
{
    return ESPNOW_LINK_GetPttMode() == 2u;
}

extern "C" void FMO_SetPtt(const bool held)
{
    FmoConfig config = {};
    FMO_GetConfig(&config);
    s_ptt_held = held && config.enabled && config.transmit;
}

extern "C" bool FMO_PttActive(void)
{
    FmoConfig config = {};
    FMO_GetConfig(&config);
    return config.enabled && config.transmit &&
           (s_ptt_held ||
            (ESPNOW_LINK_GetPttMode() == 2u && STATUS_IO_IsSqlActive()));
}

extern "C" size_t FMO_ServerCount(void)
{
    if (s_server_mutex == nullptr) return 0u;
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    const size_t count = s_server_count;
    xSemaphoreGive(s_server_mutex);
    return count;
}

extern "C" bool FMO_GetServer(const size_t index, FmoServer *server)
{
    if (server == nullptr || s_server_mutex == nullptr) return false;
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    const bool valid = index < s_server_count;
    if (valid) *server = s_servers[index];
    xSemaphoreGive(s_server_mutex);
    return valid;
}

extern "C" bool FMO_SelectServer(const size_t index, const bool persist)
{
    FmoServer server = {};
    if (!FMO_GetServer(index, &server)) return false;
    FmoConfig config = {};
    FMO_GetConfig(&config);
    config.server = server;
    return FMO_SetConfig(&config, persist);
}

extern "C" bool FMO_PublishMessage(const char *topic, const char *data,
                                   const int len)
{
    if (topic == nullptr || data == nullptr || len < 0) return false;
    bool connected = false;
    portENTER_CRITICAL(&s_lock);
    connected = s_status.connected;
    portEXIT_CRITICAL(&s_lock);
    if (!connected || s_client_mutex == nullptr) return false;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    const bool published =
        s_client != nullptr &&
        esp_mqtt_client_publish(s_client, topic, data, len, 0, 0) >= 0;
    xSemaphoreGive(s_client_mutex);
    return published;
}
