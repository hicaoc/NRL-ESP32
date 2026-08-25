#include "services/fmo_station_broadcast.h"

#include "services/aprs_service.h"
#include "services/fmo_cert_store.h"
#include "services/fmo_service.h"
#include "services/fmo_station_broadcast_core.h"

#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sodium.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

namespace {

constexpr const char *TAG = "FMO-BCAST";
constexpr const char *kNvsNamespace = "fmo";
constexpr const char *kNvsKey = "bcast";
constexpr const char *kNvsPeakKey = "bcast_pk"; // historical auto peak (u32)
constexpr uint32_t kPersistMagic = 0x42534d46u; // "FMSB"
// v2: name buffer grew 33 -> 97 bytes (32 *characters*, not bytes); old v1
// blobs are rejected and fall back to defaults (one-time config loss).
constexpr uint16_t kPersistVersion = 2u;
// Same wall-clock sanity floor as fmo_cert_store.cpp: time() below this means
// SNTP has not synced yet and the TBS time slot would be garbage.
constexpr int64_t kMinValidEpoch = 1700000000LL;
constexpr int64_t kMinSendIntervalMs = 60000LL;  // hard rate limit (§8.3 guard)
constexpr int64_t kTickMs = 5000LL;
// Flash wear guard for the persisted peak: at most one NVS write per minute.
constexpr int64_t kPeakSaveMinIntervalMs = 60000LL;

struct PersistedConfig {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t mode;
    uint8_t ssid;   // was "reserved" in v1; old blobs read back as ssid 0
    char country[3];
    char name[FMO_STATION_BCAST_NAME_MAX + 1u];
    char host[FMO_STATION_BCAST_HOST_MAX + 1u];
    uint16_t port;
    uint32_t cover_km;
    uint32_t online;
    uint32_t peak;
    uint32_t crc32;
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static FmoStationBroadcastConfig s_config = {};
static FmoStationBroadcastStatus s_status = {};
static bool s_config_loaded = false;
static int64_t s_last_send_ms = 0;     // last successful packet
static int64_t s_last_attempt_ms = 0;  // last send attempt (rate limiter)
static TaskHandle_t s_task = nullptr;
// Online-count roster (~4 KiB static): distinct heartbeat uids inside the
// 120 s window. s_nvs_peak is the historical peak known to be in flash.
static FmoOnlineRoster s_roster = {};
static uint32_t s_nvs_peak = 0;
static bool s_peak_loaded = false;
static int64_t s_last_peak_save_ms = 0;

// --- BEACON personal beacon state (§8.6) -----------------------------------
constexpr const char *kNvsBeaconKey = "bcn";
constexpr uint32_t kBeaconPersistMagic = 0x4e434246u; // "FBCN"
constexpr uint16_t kBeaconPersistVersion = 1u;
constexpr int64_t kBeaconIntervalMs = 10LL * 60000LL; // fixed 10-minute period
// Follow-up frames (APFMO1/APFMO2) retry until the raw-line slot accepts
// them or this deadline passes; the APRS task drains one line per ~20 ms.
constexpr int64_t kFollowupTimeoutMs = 120000LL;
constexpr size_t kBeaconLineMax = 512u; // whole wire frame, longer = dropped

struct PersistedBeaconConfig {
    uint32_t magic;
    uint16_t version;
    uint8_t enabled;
    uint8_t ssid;
    uint32_t freq_x10000;
    uint16_t height_m;
    char rig[FMO_BEACON_RIG_ANT_MAX + 1u];
    char ant[FMO_BEACON_RIG_ANT_MAX + 1u];
    char aprs_msg[FMO_BEACON_MSG_MAX + 1u];
    char notice[FMO_BEACON_NOTICE_MAX + 1u];
    char qso_msg[FMO_BEACON_NOTICE_MAX + 1u];
    uint32_t crc32;
};

static FmoBeaconConfig s_bcn_config = {};
static FmoBeaconStatus s_bcn_status = {};
static bool s_bcn_config_loaded = false;
static int64_t s_bcn_last_send_ms = 0;    // task-local, like the STATION ones
static int64_t s_bcn_last_attempt_ms = 0;
static int64_t s_follow1_deadline_ms = 0; // APFMO1 notice pending while != 0
static int64_t s_follow2_deadline_ms = 0; // APFMO2 text pending while != 0

uint32_t crcPersist(const PersistedConfig *config)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(config),
                            offsetof(PersistedConfig, crc32));
}

int64_t intervalMs(const uint8_t mode)
{
    switch (mode) {
        case FMO_STATION_BCAST_MODE_5MIN: return 5LL * 60000LL;
        case FMO_STATION_BCAST_MODE_10MIN: return 10LL * 60000LL;
        case FMO_STATION_BCAST_MODE_60MIN: return 60LL * 60000LL;
        default: return 0;
    }
}

void defaults(FmoStationBroadcastConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->mode = FMO_STATION_BCAST_MODE_10MIN;
}

bool loadConfig(void)
{
    PersistedConfig stored = {};
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(stored);
    const esp_err_t error = nvs_get_blob(nvs, kNvsKey, &stored, &size);
    nvs_close(nvs);
    if (error != ESP_OK || size != sizeof(stored) ||
        stored.magic != kPersistMagic || stored.version != kPersistVersion ||
        stored.crc32 != crcPersist(&stored)) {
        return false;
    }
    s_config.enabled = stored.enabled != 0u;
    s_config.mode = stored.mode;
    snprintf(s_config.country, sizeof(s_config.country), "%s", stored.country);
    snprintf(s_config.name, sizeof(s_config.name), "%s", stored.name);
    snprintf(s_config.host, sizeof(s_config.host), "%s", stored.host);
    s_config.port = stored.port;
    s_config.cover_km = stored.cover_km;
    s_config.online = stored.online;
    s_config.peak = stored.peak;
    s_config.ssid = stored.ssid <= 15u ? stored.ssid : 0u;
    return true;
}

void ensureConfigLoaded(void)
{
    if (s_config_loaded) return;
    defaults(&s_config);
    (void)loadConfig();
    s_config_loaded = true;
}

void ensurePeakLoaded(void)
{
    if (s_peak_loaded) return;
    s_peak_loaded = true;
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u32(nvs, kNvsPeakKey, &s_nvs_peak);
        nvs_close(nvs);
    }
}

bool savePeak(const uint32_t peak)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_u32(nvs, kNvsPeakKey, peak) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

bool saveConfig(const FmoStationBroadcastConfig &config)
{
    PersistedConfig stored = {};
    stored.magic = kPersistMagic;
    stored.version = kPersistVersion;
    stored.enabled = config.enabled ? 1u : 0u;
    stored.mode = config.mode;
    snprintf(stored.country, sizeof(stored.country), "%s", config.country);
    snprintf(stored.name, sizeof(stored.name), "%s", config.name);
    snprintf(stored.host, sizeof(stored.host), "%s", config.host);
    stored.port = config.port;
    stored.cover_km = config.cover_km;
    stored.online = config.online;
    stored.peak = config.peak;
    stored.ssid = config.ssid <= 15u ? config.ssid : 0u;
    stored.crc32 = crcPersist(&stored);
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_blob(nvs, kNvsKey, &stored, sizeof(stored)) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

void setReject(const int reject)
{
    portENTER_CRITICAL(&s_lock);
    s_status.last_reject = reject;
    portEXIT_CRITICAL(&s_lock);
}

void setBeaconReject(const int reject)
{
    portENTER_CRITICAL(&s_lock);
    s_bcn_status.last_reject = reject;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t crcBeaconPersist(const PersistedBeaconConfig *config)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(config),
                            offsetof(PersistedBeaconConfig, crc32));
}

bool loadBeaconConfig(void)
{
    PersistedBeaconConfig stored = {};
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(stored);
    const esp_err_t error = nvs_get_blob(nvs, kNvsBeaconKey, &stored, &size);
    nvs_close(nvs);
    if (error != ESP_OK || size != sizeof(stored) ||
        stored.magic != kBeaconPersistMagic ||
        stored.version != kBeaconPersistVersion ||
        stored.crc32 != crcBeaconPersist(&stored)) {
        return false;
    }
    s_bcn_config.enabled = stored.enabled != 0u;
    s_bcn_config.ssid = stored.ssid <= 15u ? stored.ssid : 0u;
    s_bcn_config.freq_x10000 = stored.freq_x10000;
    s_bcn_config.height_m = stored.height_m;
    snprintf(s_bcn_config.rig, sizeof(s_bcn_config.rig), "%s", stored.rig);
    snprintf(s_bcn_config.ant, sizeof(s_bcn_config.ant), "%s", stored.ant);
    snprintf(s_bcn_config.aprs_msg, sizeof(s_bcn_config.aprs_msg), "%s",
             stored.aprs_msg);
    snprintf(s_bcn_config.notice, sizeof(s_bcn_config.notice), "%s",
             stored.notice);
    snprintf(s_bcn_config.qso_msg, sizeof(s_bcn_config.qso_msg), "%s",
             stored.qso_msg);
    return true;
}

void ensureBeaconConfigLoaded(void)
{
    if (s_bcn_config_loaded) return;
    memset(&s_bcn_config, 0, sizeof(s_bcn_config));
    (void)loadBeaconConfig();
    s_bcn_config_loaded = true;
}

bool saveBeaconConfig(const FmoBeaconConfig &config)
{
    PersistedBeaconConfig stored = {};
    stored.magic = kBeaconPersistMagic;
    stored.version = kBeaconPersistVersion;
    stored.enabled = config.enabled ? 1u : 0u;
    stored.ssid = config.ssid <= 15u ? config.ssid : 0u;
    stored.freq_x10000 = config.freq_x10000;
    stored.height_m = config.height_m;
    snprintf(stored.rig, sizeof(stored.rig), "%s", config.rig);
    snprintf(stored.ant, sizeof(stored.ant), "%s", config.ant);
    snprintf(stored.aprs_msg, sizeof(stored.aprs_msg), "%s", config.aprs_msg);
    snprintf(stored.notice, sizeof(stored.notice), "%s", config.notice);
    snprintf(stored.qso_msg, sizeof(stored.qso_msg), "%s", config.qso_msg);
    stored.crc32 = crcBeaconPersist(&stored);
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_blob(nvs, kNvsBeaconKey, &stored, sizeof(stored)) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

// Shared rule for the beacon text fields: at most max_chars Unicode
// characters and no ASCII comma (it would break the comma-separated wire
// format). The wire text is UTF-8, so no GBK mappability is required; the
// whole-frame <=512 check drops oversized frames at send time.
bool validBeaconText(const char *text, const size_t max_chars)
{
    if (text == nullptr) return false;
    if (strchr(text, ',') != nullptr) return false;
    size_t chars = 0u;
    for (const char *p = text; *p != '\0'; ++p) {
        if ((static_cast<unsigned char>(*p) & 0xc0u) != 0x80u) ++chars;
    }
    return chars <= max_chars;
}

bool validCountry(const char *country)
{
    return isalpha(static_cast<unsigned char>(country[0])) &&
           isalpha(static_cast<unsigned char>(country[1])) &&
           country[2] == '\0';
}

// Station name rules aligned with the reference firmware's custom-server
// config: at most 32 Unicode characters and no ASCII comma (it would break
// the comma-separated wire format). The wire name is UTF-8 (the protocol
// requires it and the map server rejects GBK), so no GBK mappability is
// required; the line buffer/oversize checks bound the byte length.
bool validStationName(const char *name)
{
    if (name == nullptr) return false;
    if (strchr(name, ',') != nullptr) return false;
    size_t chars = 0u;
    for (const char *p = name; *p != '\0'; ++p) {
        if ((static_cast<unsigned char>(*p) & 0xc0u) != 0x80u) ++chars;
    }
    return chars <= 32u;
}

// Fill host/port/name from the FMO link configuration when the operator left
// them empty. Returns false when the result is still not broadcastable.
bool completeConfig(FmoStationBroadcastConfig *config)
{
    FmoConfig fmo = {};
    FMO_GetConfig(&fmo);
    if (config->host[0] == '\0') {
        snprintf(config->host, sizeof(config->host), "%s", fmo.server.host);
    }
    if (config->port == 0u) {
        config->port = fmo.server.port;
    }
    if (config->name[0] == '\0') {
        snprintf(config->name, sizeof(config->name), "%.*s",
                 static_cast<int>(sizeof(config->name) - 1u), fmo.server.name);
    }
    if (config->name[0] == '\0' ||
        strcasecmp(config->name, fmo.server.callsign) == 0) {
        // A callsign placeholder name is fine, but prefer the certificate's
        // canonical spelling for it.
        FmoIdentityStatus identity = {};
        if (FMO_CERT_GetStatus(&identity) == ESP_OK && identity.ready) {
            snprintf(config->name, sizeof(config->name), "%s", identity.callsign);
        }
    }
    return validCountry(config->country) && config->host[0] != '\0' &&
           config->port != 0u && config->name[0] != '\0';
}

// Build and queue one STATION packet. Only called with s_config.enabled and a
// valid mode; every gate is re-checked here, right before sending.
void broadcastOnce(const FmoStationBroadcastConfig &config)
{
    if (!FMO_IsSuperOnOwnServer()) {
        setReject(FMO_STATION_BCAST_REJECT_NOT_SUPER);
        return;
    }
    if (!APRS_SERVICE_IsNetVerified()) {
        setReject(FMO_STATION_BCAST_REJECT_NOT_VERIFIED);
        return;
    }
    const time_t now = time(nullptr);
    if (now < kMinValidEpoch) {
        setReject(FMO_STATION_BCAST_REJECT_NO_TIME);
        return;
    }
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) != ESP_OK || !identity.ready) {
        setReject(FMO_STATION_BCAST_REJECT_CERT);
        return;
    }

    // CERT blob: deterministic re-encode of the stored cert_user JSON.
    uint8_t blob[512];
    size_t blob_size = 0u;
    if (FMO_CERT_RebuildUserCertBlob(blob, sizeof(blob), &blob_size) != ESP_OK) {
        setReject(FMO_STATION_BCAST_REJECT_CERT);
        return;
    }

    // Position prefix and the TBS lat/lon share one formatting call so the
    // strings are byte-identical (§8.3 cross-check).
    double lat = 0.0, lon = 0.0;
    (void)APRS_SERVICE_GetOwnPosition(&lat, &lon, nullptr);
    char lat_str[10], lon_str[11];
    FMO_STATION_CORE_FormatLat(lat, lat_str);
    FMO_STATION_CORE_FormatLon(lon, lon_str);

    FmoStationTbsParams params = {};
    snprintf(params.callsign, sizeof(params.callsign), "%s", identity.callsign);
    // Header and TBS SSID always carry the same configured value (0 = bare
    // callsign); the reference firmware takes it from its APRS-IS login.
    params.ssid = config.ssid <= 15u ? config.ssid : 0u;
    snprintf(params.lat, sizeof(params.lat), "%s", lat_str);
    snprintf(params.lon, sizeof(params.lon), "%s", lon_str);
    crypto_hash_sha256(params.cert_blob_hash, blob, blob_size);
    snprintf(params.country, sizeof(params.country), "%s", config.country);
    params.name_utf8 = config.name;
    params.host = config.host;
    params.port = config.port;
    params.cover_km = config.cover_km;
    // 0 = automatic (from the heartbeat roster), >0 = manual override.
    uint32_t auto_online = 0u, auto_peak = 0u;
    FMO_STATION_BCAST_GetAutoCounts(&auto_online, &auto_peak);
    params.online = FMO_STATION_CORE_EffectiveCount(config.online, auto_online);
    params.peak = FMO_STATION_CORE_EffectiveCount(config.peak, auto_peak);
    params.time_slot = static_cast<uint64_t>(now) / 600u;

    uint8_t tbs[512];
    size_t tbs_size = 0u;
    if (!FMO_STATION_CORE_BuildTbs(&params, tbs, sizeof(tbs), &tbs_size)) {
        setReject(FMO_STATION_BCAST_REJECT_CONFIG);
        return;
    }
    uint8_t signature[64];
    if (FMO_CERT_SignWithDeviceKey(tbs, tbs_size, signature) != ESP_OK) {
        setReject(FMO_STATION_BCAST_REJECT_CERT);
        return;
    }

    char cert_b64[720];
    char sig_b64[96];
    if (FMO_STATION_CORE_Base64UrlEncode(blob, blob_size, cert_b64,
                                         sizeof(cert_b64)) == 0u ||
        FMO_STATION_CORE_Base64UrlEncode(signature, sizeof(signature), sig_b64,
                                         sizeof(sig_b64)) == 0u) {
        setReject(FMO_STATION_BCAST_REJECT_CONFIG);
        return;
    }

    // The on-air name is UTF-8, byte-identical to the signed TBS name (the
    // protocol requires UTF-8; the map server rejects GBK names).
    char upper[16];
    snprintf(upper, sizeof(upper), "%s", identity.callsign);
    for (char *p = upper; *p != '\0'; ++p) {
        *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
    }
    char source[32];
    if (params.ssid > 0u) {
        snprintf(source, sizeof(source), "%s-%u", upper,
                 static_cast<unsigned>(params.ssid));
    } else {
        snprintf(source, sizeof(source), "%s", upper);
    }
    char country_upper[3];
    snprintf(country_upper, sizeof(country_upper), "%s", config.country);
    for (char *p = country_upper; *p != '\0'; ++p) {
        *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
    }

    char line[1024];
    const int line_size = snprintf(
        line, sizeof(line),
        "%s>APFMO4,TCPIP*:=%sF%sEiFMO-V4,STATION,CERT:%s,%s,%s,%s,"
        "P%u,F%uKM,U%lu/%lu,SIG:%s",
        source, lat_str, lon_str, cert_b64, country_upper,
        config.name, config.host,
        static_cast<unsigned>(config.port),
        static_cast<unsigned>(config.cover_km),
        static_cast<unsigned long>(params.online),
        static_cast<unsigned long>(params.peak), sig_b64);
    if (line_size <= 0 || static_cast<size_t>(line_size) >= sizeof(line) ||
        !APRS_SERVICE_SendRawLine(line)) {
        setReject(FMO_STATION_BCAST_REJECT_SEND);
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000LL;
    portENTER_CRITICAL(&s_lock);
    ++s_status.tx_count;
    s_status.last_sent_epoch = static_cast<uint32_t>(now);
    s_status.last_reject = FMO_STATION_BCAST_REJECT_NONE;
    portEXIT_CRITICAL(&s_lock);
    s_last_send_ms = now_ms != 0 ? now_ms : 1;
    ensureBeaconConfigLoaded();
    portENTER_CRITICAL(&s_lock);
    const bool want_notice = s_bcn_config.notice[0] != '\0';
    portEXIT_CRITICAL(&s_lock);
    if (want_notice) {
        // The APFMO1 login notice follows the STATION frame; the raw-line
        // slot is still busy, so the task loop retries until the deadline.
        s_follow1_deadline_ms = now_ms + kFollowupTimeoutMs;
    }
    ESP_LOGI(TAG, "STATION broadcast queued: %s", line);
}

void beaconSource(const char *callsign, const uint8_t ssid, char out[32])
{
    char upper[16];
    snprintf(upper, sizeof(upper), "%s", callsign);
    for (char *p = upper; *p != '\0'; ++p) {
        *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
    }
    if (ssid > 0u && ssid <= 15u) {
        snprintf(out, 32u, "%s-%u", upper, static_cast<unsigned>(ssid));
    } else {
        snprintf(out, 32u, "%s", upper);
    }
}

// Build and queue one BEACON frame. Gates (re-checked right before sending):
// APRS-IS verified login, sane wall clock, freq > 0, ready certificate. No
// super/own-server gate -- the personal beacon is independent of any server.
void beaconOnce(const FmoBeaconConfig &config)
{
    if (!APRS_SERVICE_IsNetVerified()) {
        setBeaconReject(FMO_BEACON_REJECT_NOT_VERIFIED);
        return;
    }
    const time_t now = time(nullptr);
    if (now < kMinValidEpoch) {
        setBeaconReject(FMO_BEACON_REJECT_NO_TIME);
        return;
    }
    if (config.freq_x10000 == 0u) {
        setBeaconReject(FMO_BEACON_REJECT_CONFIG);
        return;
    }
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) != ESP_OK || !identity.ready) {
        setBeaconReject(FMO_BEACON_REJECT_CERT);
        return;
    }
    uint8_t blob[512];
    size_t blob_size = 0u;
    if (FMO_CERT_RebuildUserCertBlob(blob, sizeof(blob), &blob_size) != ESP_OK) {
        setBeaconReject(FMO_BEACON_REJECT_CERT);
        return;
    }

    // Position prefix and the TBS lat/lon share one formatting call so the
    // strings are byte-identical (same cross-check as the STATION broadcast).
    double lat = 0.0, lon = 0.0;
    (void)APRS_SERVICE_GetOwnPosition(&lat, &lon, nullptr);
    char lat_str[10], lon_str[11];
    FMO_STATION_CORE_FormatLat(lat, lat_str);
    FMO_STATION_CORE_FormatLon(lon, lon_str);

    FmoBeaconTbsParams params = {};
    snprintf(params.callsign, sizeof(params.callsign), "%s", identity.callsign);
    params.ssid = config.ssid <= 15u ? config.ssid : 0u;
    snprintf(params.lat, sizeof(params.lat), "%s", lat_str);
    snprintf(params.lon, sizeof(params.lon), "%s", lon_str);
    crypto_hash_sha256(params.cert_blob_hash, blob, blob_size);
    // Exact "%.4f" text without float rounding concerns.
    snprintf(params.freq, sizeof(params.freq), "%lu.%04lu",
             static_cast<unsigned long>(config.freq_x10000 / 10000u),
             static_cast<unsigned long>(config.freq_x10000 % 10000u));
    params.height_m = config.height_m;
    params.rig_utf8 = config.rig; // empty strings are omitted from the TBS
    params.ant_utf8 = config.ant;
    params.time_slot = static_cast<uint64_t>(now) / 600u;

    uint8_t tbs[512];
    size_t tbs_size = 0u;
    if (!FMO_STATION_CORE_BuildBeaconTbs(&params, tbs, sizeof(tbs), &tbs_size)) {
        setBeaconReject(FMO_BEACON_REJECT_CONFIG);
        return;
    }
    uint8_t signature[64];
    if (FMO_CERT_SignWithDeviceKey(tbs, tbs_size, signature) != ESP_OK) {
        setBeaconReject(FMO_BEACON_REJECT_CERT);
        return;
    }
    char cert_b64[720];
    char sig_b64[96];
    if (FMO_STATION_CORE_Base64UrlEncode(blob, blob_size, cert_b64,
                                         sizeof(cert_b64)) == 0u ||
        FMO_STATION_CORE_Base64UrlEncode(signature, sizeof(signature), sig_b64,
                                         sizeof(sig_b64)) == 0u) {
        setBeaconReject(FMO_BEACON_REJECT_CONFIG);
        return;
    }

    // Optional wire fields; RIG/ANT go out as the configured UTF-8 text,
    // byte-identical to the signed TBS values.
    char extras[160];
    size_t used = 0u;
    if (config.height_m > 0u) {
        used += static_cast<size_t>(snprintf(
            extras + used, sizeof(extras) - used, ",HEIGHT:%u",
            static_cast<unsigned>(config.height_m)));
    }
    if (config.rig[0] != '\0') {
        used += static_cast<size_t>(snprintf(
            extras + used, sizeof(extras) - used, ",RIG:%s", config.rig));
    }
    if (config.ant[0] != '\0') {
        used += static_cast<size_t>(snprintf(
            extras + used, sizeof(extras) - used, ",ANT:%s", config.ant));
    }

    char source[32];
    beaconSource(identity.callsign, config.ssid, source);
    char line[1024];
    const int line_size = snprintf(
        line, sizeof(line),
        "%s>APFMO4,TCPIP*:=%sF%sEiFMO-V4,BEACON,CERT:%s,FREQ:%s%s,SIG:%s",
        source, lat_str, lon_str, cert_b64, params.freq, extras, sig_b64);
    if (line_size <= 0 || static_cast<size_t>(line_size) >= sizeof(line)) {
        setBeaconReject(FMO_BEACON_REJECT_SEND);
        return;
    }
    if (static_cast<size_t>(line_size) > kBeaconLineMax) {
        // Oversized frames are dropped, never truncated (§8.6 wire format).
        setBeaconReject(FMO_BEACON_REJECT_TOO_LONG);
        return;
    }
    if (!APRS_SERVICE_SendRawLine(line)) {
        setBeaconReject(FMO_BEACON_REJECT_SEND);
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000LL;
    portENTER_CRITICAL(&s_lock);
    ++s_bcn_status.tx_count;
    s_bcn_status.last_sent_epoch = static_cast<uint32_t>(now);
    s_bcn_status.last_reject = FMO_BEACON_REJECT_NONE;
    portEXIT_CRITICAL(&s_lock);
    s_bcn_last_send_ms = now_ms != 0 ? now_ms : 1;
    if (config.aprs_msg[0] != '\0') {
        s_follow2_deadline_ms = now_ms + kFollowupTimeoutMs;
    }
    ESP_LOGI(TAG, "BEACON queued: %s", line);
}

// APFMO2 personalized text (unsigned UTF-8 free text), queued after a BEACON.
// Returns true when sent or when there is nothing (left) to send.
bool trySendAprs2(void)
{
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) != ESP_OK || !identity.ready) return true;
    FmoBeaconConfig config = {};
    portENTER_CRITICAL(&s_lock);
    config = s_bcn_config;
    portEXIT_CRITICAL(&s_lock);
    if (config.aprs_msg[0] == '\0') return true;
    char source[32];
    beaconSource(identity.callsign, config.ssid, source);
    char line[256];
    const int line_size = snprintf(line, sizeof(line), "%s>APFMO2,TCPIP*:>%s",
                                   source, config.aprs_msg);
    if (line_size <= 0 || static_cast<size_t>(line_size) >= sizeof(line)) {
        return true;
    }
    if (!APRS_SERVICE_SendRawLine(line)) return false; // slot busy, retry
    ESP_LOGI(TAG, "APFMO2 text queued: %s", line);
    return true;
}

// APFMO1 login notice, queued after a STATION broadcast:
//   ><nameUTF8>,正常,在线/峰值:<online>/<peak>[,<noticeUTF8>]
// with the STATION broadcast's effective online/peak values. All text is
// UTF-8 (the reference firmware's APFMO1 is UTF-8 on the wire).
bool trySendAprs1(void)
{
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) != ESP_OK || !identity.ready) return true;
    FmoBeaconConfig bcn = {};
    portENTER_CRITICAL(&s_lock);
    bcn = s_bcn_config;
    portEXIT_CRITICAL(&s_lock);
    if (bcn.notice[0] == '\0') return true;
    FmoStationBroadcastConfig bcast = {};
    portENTER_CRITICAL(&s_lock);
    bcast = s_config;
    portEXIT_CRITICAL(&s_lock);
    (void)completeConfig(&bcast); // same advertised name as broadcastOnce

    uint32_t auto_online = 0u, auto_peak = 0u;
    FMO_STATION_BCAST_GetAutoCounts(&auto_online, &auto_peak);
    const uint32_t online =
        FMO_STATION_CORE_EffectiveCount(bcast.online, auto_online);
    const uint32_t peak = FMO_STATION_CORE_EffectiveCount(bcast.peak, auto_peak);

    char source[32];
    beaconSource(identity.callsign, bcast.ssid, source);
    char line[512];
    const int line_size = snprintf(
        line, sizeof(line), "%s>APFMO1,TCPIP*:>%s,正常,在线/峰值:%lu/%lu,%s",
        source, bcast.name,
        static_cast<unsigned long>(online), static_cast<unsigned long>(peak),
        bcn.notice);
    if (line_size <= 0 || static_cast<size_t>(line_size) >= sizeof(line)) {
        return true;
    }
    if (!APRS_SERVICE_SendRawLine(line)) return false; // slot busy, retry
    ESP_LOGI(TAG, "APFMO1 notice queued: %s", line);
    return true;
}

void processFollowups(const int64_t now_ms)
{
    if (s_follow2_deadline_ms != 0 &&
        (now_ms > s_follow2_deadline_ms || trySendAprs2())) {
        s_follow2_deadline_ms = 0;
    }
    if (s_follow1_deadline_ms != 0 &&
        (now_ms > s_follow1_deadline_ms || trySendAprs1())) {
        s_follow1_deadline_ms = 0;
    }
}

void broadcastTask(void *)
{
    for (;;) {
        ensureConfigLoaded();
        FmoStationBroadcastConfig config = {};
        portENTER_CRITICAL(&s_lock);
        config = s_config;
        portEXIT_CRITICAL(&s_lock);

        const int64_t interval = intervalMs(config.mode);
        const int64_t now_ms = esp_timer_get_time() / 1000LL;
        const bool active = config.enabled && interval > 0;
        bool gated = false;
        bool configured = true;
        if (active) {
            gated = !FMO_IsSuperOnOwnServer() || !APRS_SERVICE_IsNetVerified();
            FmoStationBroadcastConfig completed = config;
            configured = completeConfig(&completed);
        }
        portENTER_CRITICAL(&s_lock);
        s_status.gated = gated;
        s_status.configured = configured;
        portEXIT_CRITICAL(&s_lock);

        if (active && configured) {
            const bool due = s_last_send_ms == 0 ||
                             now_ms - s_last_send_ms >= interval;
            if (due) {
                if (s_last_attempt_ms != 0 &&
                    now_ms - s_last_attempt_ms < kMinSendIntervalMs) {
                    setReject(FMO_STATION_BCAST_REJECT_RATE_LIMIT);
                } else {
                    s_last_attempt_ms = now_ms != 0 ? now_ms : 1;
                    FmoStationBroadcastConfig completed = config;
                    (void)completeConfig(&completed);
                    broadcastOnce(completed);
                }
            }
        }

        // --- BEACON personal beacon: independent 10-minute timer (§8.6).
        // Unlike the STATION broadcast there is no super/own-server gate.
        ensureBeaconConfigLoaded();
        FmoBeaconConfig bcn = {};
        portENTER_CRITICAL(&s_lock);
        bcn = s_bcn_config;
        portEXIT_CRITICAL(&s_lock);
        bool bcn_gated = false;
        if (bcn.enabled) {
            FmoIdentityStatus identity = {};
            bcn_gated = bcn.freq_x10000 == 0u ||
                        !APRS_SERVICE_IsNetVerified() ||
                        FMO_CERT_GetStatus(&identity) != ESP_OK ||
                        !identity.ready;
        }
        portENTER_CRITICAL(&s_lock);
        s_bcn_status.gated = bcn_gated;
        portEXIT_CRITICAL(&s_lock);
        if (bcn.enabled) {
            const bool due = s_bcn_last_send_ms == 0 ||
                             now_ms - s_bcn_last_send_ms >= kBeaconIntervalMs;
            if (due) {
                if (s_bcn_last_attempt_ms != 0 &&
                    now_ms - s_bcn_last_attempt_ms < kMinSendIntervalMs) {
                    setBeaconReject(FMO_BEACON_REJECT_RATE_LIMIT);
                } else {
                    s_bcn_last_attempt_ms = now_ms != 0 ? now_ms : 1;
                    beaconOnce(bcn);
                }
            }
        }

        // Follow-up frames (APFMO1 notice / APFMO2 text) pending from the
        // last STATION/BEACON send; the raw-line slot is usually free again
        // by the next tick.
        processFollowups(now_ms);
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

} // namespace

extern "C" bool FMO_STATION_BCAST_Init(void)
{
    if (s_task != nullptr) return true;
    ensureConfigLoaded();
    ensureBeaconConfigLoaded();
    if (xTaskCreate(broadcastTask, "fmo_bcast", 8192, nullptr, 3,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "broadcast task creation failed");
        return false;
    }
    ESP_LOGI(TAG, "STATION broadcast ready (enabled=%u mode=%u)",
             s_config.enabled ? 1u : 0u, static_cast<unsigned>(s_config.mode));
    return true;
}

extern "C" void FMO_STATION_BCAST_GetConfig(FmoStationBroadcastConfig *config)
{
    if (config == nullptr) return;
    ensureConfigLoaded();
    portENTER_CRITICAL(&s_lock);
    *config = s_config;
    portEXIT_CRITICAL(&s_lock);
}

extern "C" bool FMO_STATION_BCAST_SetConfig(
    const FmoStationBroadcastConfig *config, const bool persist)
{
    if (config == nullptr) return false;
    ensureConfigLoaded();
    FmoStationBroadcastConfig normalized = *config;
    normalized.country[sizeof(normalized.country) - 1u] = '\0';
    normalized.name[sizeof(normalized.name) - 1u] = '\0';
    normalized.host[sizeof(normalized.host) - 1u] = '\0';
    // Name rules apply even while disabled, so a bad name can never be
    // persisted; an empty name is fine (completeConfig fills it later).
    if (normalized.name[0] != '\0' && !validStationName(normalized.name)) {
        return false;
    }
    if (normalized.enabled) {
        if (intervalMs(normalized.mode) == 0 ||
            !validCountry(normalized.country)) {
            return false;
        }
        FmoStationBroadcastConfig completed = normalized;
        if (!completeConfig(&completed)) return false;
        // Up-front validation of gates 1-3 (MQTT connected, actual login role
        // "super", server callsign == certificate callsign). Gates 4/5 can
        // only be evaluated at send time.
        if (!FMO_IsSuperOnOwnServer()) return false;
        normalized = completed;
    }
    if (persist && !saveConfig(normalized)) return false;
    portENTER_CRITICAL(&s_lock);
    s_config = normalized;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

extern "C" void FMO_STATION_BCAST_GetStatus(FmoStationBroadcastStatus *status)
{
    if (status == nullptr) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

extern "C" bool FMO_STATION_BCAST_GatesOk(void)
{
    return FMO_IsSuperOnOwnServer();
}

extern "C" void FMO_STATION_BCAST_FeedHeartbeat(const uint32_t uid)
{
    ensurePeakLoaded();
    const int64_t now_ms = esp_timer_get_time() / 1000LL;
    const uint32_t now_s = static_cast<uint32_t>(now_ms / 1000LL);
    uint32_t new_peak = 0u;
    portENTER_CRITICAL(&s_lock);
    FMO_STATION_CORE_RosterFeed(&s_roster, uid, now_s);
    if (s_roster.session_peak > s_nvs_peak &&
        (s_last_peak_save_ms == 0 ||
         now_ms - s_last_peak_save_ms >= kPeakSaveMinIntervalMs)) {
        s_last_peak_save_ms = now_ms != 0 ? now_ms : 1;
        new_peak = s_roster.session_peak;
    }
    portEXIT_CRITICAL(&s_lock);
    // Throttled peak persist. When the write is skipped by the throttle the
    // next heartbeat retries, because session_peak still exceeds s_nvs_peak.
    if (new_peak != 0u && savePeak(new_peak)) {
        portENTER_CRITICAL(&s_lock);
        if (new_peak > s_nvs_peak) s_nvs_peak = new_peak;
        portEXIT_CRITICAL(&s_lock);
    }
}

extern "C" void FMO_STATION_BCAST_RosterReset(void)
{
    portENTER_CRITICAL(&s_lock);
    FMO_STATION_CORE_RosterReset(&s_roster);
    portEXIT_CRITICAL(&s_lock);
}

extern "C" void FMO_STATION_BCAST_GetAutoCounts(uint32_t *online, uint32_t *peak)
{
    ensurePeakLoaded();
    const uint32_t now_s =
        static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
    portENTER_CRITICAL(&s_lock);
    const uint32_t current = FMO_STATION_CORE_RosterOnline(&s_roster, now_s);
    const uint32_t best = s_roster.session_peak > s_nvs_peak
                              ? s_roster.session_peak
                              : s_nvs_peak;
    portEXIT_CRITICAL(&s_lock);
    if (online != nullptr) *online = current;
    if (peak != nullptr) *peak = best;
}

extern "C" void FMO_BEACON_GetConfig(FmoBeaconConfig *config)
{
    if (config == nullptr) return;
    ensureBeaconConfigLoaded();
    portENTER_CRITICAL(&s_lock);
    *config = s_bcn_config;
    portEXIT_CRITICAL(&s_lock);
}

extern "C" bool FMO_BEACON_SetConfig(const FmoBeaconConfig *config,
                                     const bool persist)
{
    if (config == nullptr) return false;
    ensureBeaconConfigLoaded();
    FmoBeaconConfig normalized = *config;
    normalized.rig[sizeof(normalized.rig) - 1u] = '\0';
    normalized.ant[sizeof(normalized.ant) - 1u] = '\0';
    normalized.aprs_msg[sizeof(normalized.aprs_msg) - 1u] = '\0';
    normalized.notice[sizeof(normalized.notice) - 1u] = '\0';
    normalized.qso_msg[sizeof(normalized.qso_msg) - 1u] = '\0';
    // Text rules apply even while disabled, so bad text can never persist.
    // qso_msg is stored only (传输机制待研究) but follows the notice rules.
    if (!validBeaconText(normalized.rig, 16u) ||
        !validBeaconText(normalized.ant, 16u) ||
        !validBeaconText(normalized.aprs_msg, 64u) ||
        !validBeaconText(normalized.notice, 128u) ||
        !validBeaconText(normalized.qso_msg, 128u)) {
        return false;
    }
    // 20-500 MHz; 0 stays allowed (beacon gated off) but cannot be enabled.
    if (normalized.freq_x10000 != 0u &&
        (normalized.freq_x10000 < 200000u ||
         normalized.freq_x10000 > 5000000u)) {
        return false;
    }
    if (normalized.enabled && normalized.freq_x10000 == 0u) return false;
    if (normalized.ssid > 15u) normalized.ssid = 0u;
    if (persist && !saveBeaconConfig(normalized)) return false;
    portENTER_CRITICAL(&s_lock);
    s_bcn_config = normalized;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

extern "C" void FMO_BEACON_GetStatus(FmoBeaconStatus *status)
{
    if (status == nullptr) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_bcn_status;
    portEXIT_CRITICAL(&s_lock);
}
