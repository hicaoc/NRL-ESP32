#include "services/aprs_service.h"
#include "services/fmo_service.h"

#include "audio/audio_router.h"
#include "audio/audio_focus.h"
#include "driver/bh4tdv_rf_io.h"
#include "driver/board_pins.h"
#include "driver/display.h"
#include "driver/external_radio.h"
#include "driver/gps_serial.h"
#include "driver/serial_port_config.h"
#include "lib/aprs/aprs_ax25.h"
#include "lib/aprs/aprs_modem.h"
#include "lib/aprs/parse_aprs.h"
#include "lib/aprs/pbuf.h"
#include "lib/nrl_version.h"
#include "lib/nrl_audio_bridge.h"
#include "lib/nrl_psram.h"
#include "lib/nrl_wifi.h"
#include "services/config_notify.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <nvs.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

static const char *TAG = "APRS";

namespace {

constexpr const char *kNvsNamespace = "aprs";
constexpr const char *kNvsKey = "cfg";

constexpr uint32_t kPersistMagic = 0x41505253u; // 'APRS'
constexpr uint8_t kPersistVersion = 1u;
constexpr const char *kDefaultAprsServer = "asia.aprs2.net";

constexpr uint16_t kMinBeaconIntervalS = 10u;
constexpr uint16_t kMaxBeaconIntervalS = 3600u;
// Auto-interval (SmartBeaconing-style) pacing. With a fresh GPS fix the
// effective period scales inversely with speed -- 60+ km/h beacons at the
// 30 s floor, slower proportionally longer -- and a >kAutoMoveKm jump from
// the last-beaconed position sends one early. The configured
// beacon_interval_s always remains the stationary/no-GPS ceiling.
constexpr uint16_t kAutoMinIntervalS = 30u;
constexpr uint16_t kAutoTurnMinIntervalS = 10u;
constexpr uint32_t kStatusSatelliteUpdateMs = 60000u;
constexpr double kAutoFastSpeedKmh = 60.0; // at/above: beacon at the floor rate
constexpr double kAutoSlowSpeedKmh = 3.0;  // below: treated as stationary
constexpr double kAutoTurnMinSpeedKmh = 8.0; // reject noisy walking/stationary course
constexpr double kAutoMoveKm = 0.3;        // early beacon after moving this far
// WGS-84 default: 3153.3100N, 11848.8460E.
constexpr int32_t kDefaultLatE6 = 31888500;
constexpr int32_t kDefaultLonE6 = 118814100;
constexpr uint32_t kGpsFixFreshMs = 10000u;   // fix older than this = stale
constexpr uint32_t kGpsDataFreshMs = 5000u;   // recognized NMEA older than this = disconnected
constexpr uint32_t kIsReconnectMs = 30000u;   // APRS-IS reconnect backoff
constexpr size_t kStationCount = 64u;         // PSRAM station table size
constexpr size_t kRxRingSamples = 16384u;     // ~1 s of 16 kHz mic tap
constexpr size_t kTxChunkSamples = 160u;      // 20 ms at MODEM_TX_SAMPLE_RATE
// Match the level of known-decodable NRL AFSK captures (~12032 PCM peak).
// The previous value decoded to ~23040 after G.711: DCD locked, but the
// correlator never produced a valid HDLC frame because the tones were driven
// well outside the receiver's reliable level window.
constexpr int16_t kTxAmplitude = 94;
constexpr uint32_t kTxLeadSamples = kTxChunkSamples;
constexpr size_t kPersistCommentLegacyBytes = 40u;
constexpr size_t kPersistCommentV2Bytes = 96u;

// Persisted blob (NVS). New fields append at the END: loadConfig accepts a
// shorter (older) blob and the missing tail stays zero-initialised, so
// existing configs survive upgrades without a version bump.
struct PersistBlob {
    uint32_t magic;
    uint8_t version;
    uint8_t enabled;
    uint8_t net_enabled;
    uint8_t rf_tx_enabled;
    uint8_t rf_rx_enabled;
    uint8_t ssid;
    char symbol_table;
    char symbol_code;
    uint16_t beacon_interval_s;
    int32_t default_lat_e6;
    int32_t default_lon_e6;
    uint16_t server_port;
    char server_host[65];
    char path[17];
    char comment_legacy[kPersistCommentLegacyBytes + 1u]; // pre-0.8.13 field; keep its offset for migration
    uint8_t auto_interval; // appended field: absent in pre-0.6.7 blobs
    char comment_v2[kPersistCommentV2Bytes + 1u]; // appended in 0.8.13
    uint8_t fixed_beacon_without_gps; // appended in 0.8.18
    char comment_v3[APRS_COMMENT_MAX_BYTES + 1u]; // appended in 0.8.18
    uint8_t nrl_tx_enabled;  // appended in 0.8.49: AFSK out over the NRL uplink
    uint8_t nrl_rx_enabled;  // appended in 0.8.49: demodulate NRL downlink
    uint8_t fwd[APRS_FWD_COUNT]; // appended in 0.8.50: gateway forwarding switches
    uint8_t gps_power_enabled; // appended in 0.8.83: GPS module power switch
} __attribute__((packed));

struct StationRec {
    char name[10];
    char symbol[3];
    float lat;
    float lon;
    float altitude_m;
    float speed_kmh;
    uint16_t course_deg;
    float derived_speed_kmh;
    float distance_km;
    uint16_t bearing_deg;
    uint32_t last_heard_ms;
    uint32_t pkt_count;
    uint8_t via_rf;
    char comment[APRS_COMMENT_MAX_BYTES + 1u];
    bool used;
};

AprsConfig s_cfg = {};
SemaphoreHandle_t s_cfg_mutex = nullptr;
SemaphoreHandle_t s_station_mutex = nullptr;

NRL_PSRAM_BSS StationRec s_station_storage[kStationCount];
StationRec *s_stations = s_station_storage;
uint32_t s_station_revision = 0;

TaskHandle_t s_task = nullptr;

ParseAPRS s_parser;

// GPS state (updated from the task's NMEA parsing only)
volatile bool s_gps_fix = false;
double s_gps_lat = 0.0;
double s_gps_lon = 0.0;
double s_gps_alt_m = 0.0;
float s_gps_speed_kmh = -1.0f;
float s_gps_hdop = NAN;
uint16_t s_gps_course = 0;
bool s_gps_course_valid = false;
int16_t s_gps_satellites = -1;
uint8_t s_gps_fix_quality = 0;
uint32_t s_gps_last_fix_ms = 0;
uint32_t s_gps_last_nmea_ms = 0;
uint32_t s_gps_last_rmc_ms = 0;
uint32_t s_gps_last_gga_ms = 0;
AprsGpsSatelliteInfo s_gps_visible[APRS_GPS_SATELLITE_MAX] = {};
uint32_t s_gps_visible_seen_ms[APRS_GPS_SATELLITE_MAX] = {};
uint8_t s_gps_visible_count = 0;
uint32_t s_gps_last_gsv_ms = 0;
portMUX_TYPE s_gps_mux = portMUX_INITIALIZER_UNLOCKED;

// NMEA ring, filled by the APRS task from dedicated UART2.
constexpr size_t kNmeaRingSize = 1024u;
NRL_PSRAM_BSS uint8_t s_nmea_ring[kNmeaRingSize];
volatile size_t s_nmea_head = 0;
volatile size_t s_nmea_tail = 0;
char s_nmea_line[128];
size_t s_nmea_line_len = 0;

// RX PCM rings (producer: audio task via router sink, consumer: aprs task).
// Two independent rings so the mic tap and the NRL downlink tap demodulate
// in parallel on separate modem instances instead of mixing into one stream:
// ring 0 = AUDIO_SRC_MIC (demodulator 0), ring 1 = AUDIO_SRC_NRL_DOWNLINK
// (demodulator 1). Decoded frames carry the demodulator index back out.
NRL_PSRAM_BSS int16_t s_rx_ring_storage[kRxRingSamples];
int16_t *s_rx_ring = s_rx_ring_storage;
volatile size_t s_rx_head = 0;
volatile size_t s_rx_tail = 0;
NRL_PSRAM_BSS int16_t s_rx_nrl_ring_storage[kRxRingSamples];
int16_t *s_rx_nrl_ring = s_rx_nrl_ring_storage;
volatile size_t s_rx_nrl_head = 0;
volatile size_t s_rx_nrl_tail = 0;
// 16000 -> 9600 linear-interpolation resampler state (Q16 phase), one per ring
uint32_t s_resample_phase = 0;
int16_t s_resample_last = 0;
uint32_t s_resample_nrl_phase = 0;
int16_t s_resample_nrl_last = 0;
// running RMS estimate of each tap, for the modem's signal report
uint32_t s_rx_level_acc = 0;
uint16_t s_rx_level_mv = 0;
uint32_t s_rx_level_n = 0;
uint32_t s_rx_nrl_level_acc = 0;
uint16_t s_rx_nrl_level_mv = 0;
uint32_t s_rx_nrl_level_n = 0;

// APRS-IS client state. The socket is owned exclusively by the APRS task;
// other threads request a reconnect via the flag instead of closing it.
int s_is_socket = -1;
bool s_is_logged_in = false;
// Set only after the server answers "# logresp ... verified"; a wrong
// passcode keeps the link up (Listen-Only) but must not allow STATION
// broadcasts (fmo-sim docs/firmware-analysis.md section 8.3/8.6).
bool s_is_verified = false;
bool s_is_status_sent = false;
int16_t s_is_status_satellites = -2; // -2=never sent, -1=no current fix
int s_is_status_battery_mv = -1;
uint32_t s_is_last_status_ms = 0;
volatile bool s_is_reconnect_requested = false;
uint32_t s_is_last_attempt_ms = 0;
char s_is_line[600];
size_t s_is_line_len = 0;

// counters / last packet
uint32_t s_rx_count = 0;
uint32_t s_tx_count = 0;
char s_last_packet[300] = {0};
char s_last_packet_summary[160] = {0};
uint32_t s_last_packet_seq = 0;

// TX pacing
bool s_tx_pumping = false;
uint32_t s_last_beacon_ms = 0; // 0 = beacon at the next enabled task tick

// Manual text message: single pending slot filled by the public API from any
// task, consumed (and transmitted) by the APRS task that owns the APRS-IS
// socket and the modem TX queue.
constexpr size_t kMsgDestMax = 9u;   // AX.25 callsign-SSID field width
constexpr size_t kMsgTextMax = 67u;  // APRS message body limit (bytes)
char s_msg_dest[kMsgDestMax + 1u] = {};
char s_msg_text[kMsgTextMax + 1u] = {};
volatile bool s_msg_pending = false;
uint32_t s_msg_seq = 0;
// Raw uplink slot for pre-built TNC2 lines (FMO STATION broadcast):
// filled through APRS_SERVICE_SendRawLine() from any task, transmitted
// by the APRS task that owns the socket (same pattern as s_msg_pending).
constexpr size_t kRawLineMax = 1023u;
char s_raw_line[kRawLineMax + 1u] = {};
volatile bool s_raw_pending = false;
// Gateway: pacing for relayed AFSK frames so a busy APRS-IS feed or a
// chattering RF channel cannot peg the modulator (beacons/messages share it).
constexpr uint32_t kFwdAfskMinGapMs = 1000u;
uint32_t s_last_fwd_afsk_ms = 0;
// Position when the last beacon left with a live fix, for the auto-interval
// movement trigger. Task-private (aprsTask writes and reads).
double s_last_beacon_lat = 0.0;
double s_last_beacon_lon = 0.0;
bool s_last_beacon_pos_valid = false;
uint16_t s_last_beacon_course = 0u;
bool s_last_beacon_course_valid = false;

inline uint32_t nowMs()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// Return the longest valid UTF-8 prefix that fits in max_bytes. APRS comments
// are persisted and displayed as UTF-8; never leave half of a multibyte code
// point at the end of a fixed-size buffer.
size_t validUtf8Prefix(const char *text, size_t text_len, size_t max_bytes)
{
    if (text == nullptr) return 0u;
    const size_t limit = text_len < max_bytes ? text_len : max_bytes;
    size_t i = 0u;
    while (i < limit) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        size_t width = 0u;
        if (c < 0x80u) width = 1u;
        else if (c >= 0xC2u && c <= 0xDFu) width = 2u;
        else if (c >= 0xE0u && c <= 0xEFu) width = 3u;
        else if (c >= 0xF0u && c <= 0xF4u) width = 4u;
        else break;
        if (i + width > limit) break;
        bool valid = true;
        for (size_t j = 1u; j < width; ++j) {
            if ((static_cast<uint8_t>(text[i + j]) & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
        }
        if (!valid ||
            (width == 3u && c == 0xE0u && static_cast<uint8_t>(text[i + 1u]) < 0xA0u) ||
            (width == 3u && c == 0xEDu && static_cast<uint8_t>(text[i + 1u]) >= 0xA0u) ||
            (width == 4u && c == 0xF0u && static_cast<uint8_t>(text[i + 1u]) < 0x90u) ||
            (width == 4u && c == 0xF4u && static_cast<uint8_t>(text[i + 1u]) >= 0x90u)) {
            break;
        }
        i += width;
    }
    return i;
}

void copyUtf8Text(char *out, size_t out_size, const char *text, size_t text_len)
{
    if (out == nullptr || out_size == 0u) return;
    const size_t bytes = validUtf8Prefix(text, text_len, out_size - 1u);
    if (bytes > 0u) memcpy(out, text, bytes);
    out[bytes] = '\0';
}

const char *boardType()
{
#if NRL_BOARD == NRL_BOARD_GEZIPAI_4G
    return "gezipai_4g";
#elif NRL_BOARD == NRL_BOARD_GEZIPAI
    return "gezipai";
#elif NRL_BOARD_IS_BI4UMD_FAMILY
    return "bi4umd";
#elif NRL_BOARD == NRL_BOARD_BH4TDV
    return "bh4tdv";
#elif NRL_BOARD == NRL_BOARD_S31_KORVO
    return "s31_korvo";
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    return "s31_function_coreboard";
#else
    return "unknown";
#endif
}

void defaultConfig(AprsConfig &cfg)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = false;
    cfg.net_enabled = true;
    cfg.rf_tx_enabled = false;
    cfg.rf_rx_enabled = false;
    cfg.nrl_tx_enabled = false;
    cfg.nrl_rx_enabled = false;
    memset(cfg.fwd, 0, sizeof(cfg.fwd));
    // Classic iGate behaviour on by default: locally heard traffic is
    // uploaded to APRS-IS; everything else stays off until opted in.
    cfg.fwd[APRS_FWD_RF_TO_IS] = true;
    cfg.fwd[APRS_FWD_NRL_TO_IS] = true;
    cfg.ssid = 5;
    cfg.symbol_table = '/';
    cfg.symbol_code = 'I'; // APRS TCP/IP symbol (/I)
    cfg.beacon_interval_s = 60;
    cfg.auto_interval = false;
    cfg.fixed_beacon_without_gps = true;
    cfg.gps_power_enabled = true;
    cfg.default_lat_e6 = kDefaultLatE6;
    cfg.default_lon_e6 = kDefaultLonE6;
    cfg.server_port = 14580;
    snprintf(cfg.server_host, sizeof(cfg.server_host), "%s", kDefaultAprsServer);
    snprintf(cfg.path, sizeof(cfg.path), "WIDE1-1");
    cfg.comment[0] = '\0';
}

void persistConfig()
{
    PersistBlob blob = {};
    blob.magic = kPersistMagic;
    blob.version = kPersistVersion;
    blob.enabled = s_cfg.enabled ? 1 : 0;
    blob.net_enabled = s_cfg.net_enabled ? 1 : 0;
    blob.rf_tx_enabled = s_cfg.rf_tx_enabled ? 1 : 0;
    blob.rf_rx_enabled = s_cfg.rf_rx_enabled ? 1 : 0;
    blob.ssid = s_cfg.ssid;
    blob.symbol_table = s_cfg.symbol_table;
    blob.symbol_code = s_cfg.symbol_code;
    blob.beacon_interval_s = s_cfg.beacon_interval_s;
    blob.auto_interval = s_cfg.auto_interval ? 1 : 0;
    blob.fixed_beacon_without_gps = s_cfg.fixed_beacon_without_gps ? 1 : 0;
    blob.default_lat_e6 = s_cfg.default_lat_e6;
    blob.default_lon_e6 = s_cfg.default_lon_e6;
    blob.server_port = s_cfg.server_port;
    memcpy(blob.server_host, s_cfg.server_host, sizeof(blob.server_host));
    memcpy(blob.path, s_cfg.path, sizeof(blob.path));
    copyUtf8Text(blob.comment_legacy, sizeof(blob.comment_legacy),
                 s_cfg.comment, strlen(s_cfg.comment));
    copyUtf8Text(blob.comment_v2, sizeof(blob.comment_v2),
                 s_cfg.comment, strlen(s_cfg.comment));
    copyUtf8Text(blob.comment_v3, sizeof(blob.comment_v3),
                 s_cfg.comment, strlen(s_cfg.comment));
    blob.nrl_tx_enabled = s_cfg.nrl_tx_enabled ? 1 : 0;
    blob.nrl_rx_enabled = s_cfg.nrl_rx_enabled ? 1 : 0;
    for (size_t i = 0; i < APRS_FWD_COUNT; ++i) {
        blob.fwd[i] = s_cfg.fwd[i] ? 1 : 0;
    }
    blob.gps_power_enabled = s_cfg.gps_power_enabled ? 1 : 0;

    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_blob(nvs, kNvsKey, &blob, sizeof(blob));
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    CONFIG_NOTIFY_Bump();
}

void loadConfig()
{
    defaultConfig(s_cfg);

    PersistBlob blob = {};
    size_t size = sizeof(blob);
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    const esp_err_t err = nvs_get_blob(nvs, kNvsKey, &blob, &size);
    nvs_close(nvs);
    // Accept older, shorter blobs: appended tail fields keep their zero init.
    if (err != ESP_OK || size < offsetof(PersistBlob, auto_interval) ||
        size > sizeof(blob) ||
        blob.magic != kPersistMagic || blob.version != kPersistVersion) {
        return;
    }

    s_cfg.enabled = blob.enabled != 0;
    s_cfg.net_enabled = blob.net_enabled != 0;
    s_cfg.rf_tx_enabled = blob.rf_tx_enabled != 0;
    s_cfg.rf_rx_enabled = blob.rf_rx_enabled != 0;
    s_cfg.ssid = blob.ssid & 0x0F;
    s_cfg.symbol_table = blob.symbol_table ? blob.symbol_table : '/';
    s_cfg.symbol_code = blob.symbol_code ? blob.symbol_code : 'I';
    const bool migrate_tcpip_symbol =
        s_cfg.symbol_table == '/' && s_cfg.symbol_code == '&';
    if (migrate_tcpip_symbol) {
        s_cfg.symbol_code = 'I';
    }
    s_cfg.beacon_interval_s = blob.beacon_interval_s;
    if (s_cfg.beacon_interval_s < kMinBeaconIntervalS ||
        s_cfg.beacon_interval_s > kMaxBeaconIntervalS) {
        s_cfg.beacon_interval_s = 60;
    }
    s_cfg.auto_interval = blob.auto_interval != 0;
    const bool has_fixed_beacon_switch =
        size >= offsetof(PersistBlob, fixed_beacon_without_gps) +
                    sizeof(blob.fixed_beacon_without_gps);
    s_cfg.fixed_beacon_without_gps = has_fixed_beacon_switch
                                         ? blob.fixed_beacon_without_gps != 0
                                         : true;
    const bool migrate_zero_position =
        blob.default_lat_e6 == 0 && blob.default_lon_e6 == 0;
    s_cfg.default_lat_e6 = migrate_zero_position ? kDefaultLatE6 : blob.default_lat_e6;
    s_cfg.default_lon_e6 = migrate_zero_position ? kDefaultLonE6 : blob.default_lon_e6;
    s_cfg.server_port = blob.server_port ? blob.server_port : 14580;
    memcpy(s_cfg.server_host, blob.server_host, sizeof(s_cfg.server_host));
    s_cfg.server_host[sizeof(s_cfg.server_host) - 1] = '\0';
    const bool migrate_aprs_server =
        s_cfg.server_host[0] == '\0' || strcmp(s_cfg.server_host, "aprs.tv") == 0;
    if (migrate_aprs_server) {
        snprintf(s_cfg.server_host, sizeof(s_cfg.server_host), "%s", kDefaultAprsServer);
    }
    memcpy(s_cfg.path, blob.path, sizeof(s_cfg.path));
    s_cfg.path[sizeof(s_cfg.path) - 1] = '\0';
    const bool has_comment_v2 =
        size >= offsetof(PersistBlob, comment_v2) + sizeof(blob.comment_v2);
    const bool has_comment_v3 =
        size >= offsetof(PersistBlob, comment_v3) + sizeof(blob.comment_v3);
    const char *saved_comment = has_comment_v3
                                    ? blob.comment_v3
                                    : (has_comment_v2 ? blob.comment_v2
                                                      : blob.comment_legacy);
    const size_t saved_capacity = has_comment_v3
                                      ? sizeof(blob.comment_v3)
                                      : (has_comment_v2 ? sizeof(blob.comment_v2)
                                                        : sizeof(blob.comment_legacy));
    copyUtf8Text(s_cfg.comment, sizeof(s_cfg.comment), saved_comment,
                 strnlen(saved_comment, saved_capacity));
    const bool has_nrl_switches =
        size >= offsetof(PersistBlob, nrl_rx_enabled) + sizeof(blob.nrl_rx_enabled);
    if (has_nrl_switches) {
        s_cfg.nrl_tx_enabled = blob.nrl_tx_enabled != 0;
        s_cfg.nrl_rx_enabled = blob.nrl_rx_enabled != 0;
    }
    const bool has_fwd_switches =
        size >= offsetof(PersistBlob, fwd) + sizeof(blob.fwd);
    if (has_fwd_switches) {
        for (size_t i = 0; i < APRS_FWD_COUNT; ++i) {
            s_cfg.fwd[i] = blob.fwd[i] != 0;
        }
    } // else keep the defaults (RF/NRL -> IS on)
    const bool has_gps_power_switch =
        size >= offsetof(PersistBlob, gps_power_enabled) +
                    sizeof(blob.gps_power_enabled);
    s_cfg.gps_power_enabled = has_gps_power_switch
                                  ? blob.gps_power_enabled != 0
                                  : true;
    const bool migrate_extended_comment = !has_comment_v3;
    if (migrate_zero_position || migrate_aprs_server || migrate_tcpip_symbol ||
        migrate_extended_comment) {
        ESP_LOGI(TAG, "migrating APRS defaults: position=%d server=%d symbol=%d comment=%d",
                 (int)migrate_zero_position, (int)migrate_aprs_server,
                 (int)migrate_tcpip_symbol, (int)migrate_extended_comment);
        persistConfig();
    }
}

void lockCfg() { xSemaphoreTake(s_cfg_mutex, portMAX_DELAY); }
void unlockCfg() { xSemaphoreGive(s_cfg_mutex); }
void lockStations() { xSemaphoreTake(s_station_mutex, portMAX_DELAY); }
void unlockStations() { xSemaphoreGive(s_station_mutex); }

// ---------------------------------------------------------------- callsign --

void myCallsign(char *out, size_t out_size)
{
    const ExternalRadioConfig *rc = EXTERNAL_RADIO_GetConfig();
    const char *call = (rc != nullptr && rc->callsign[0] != '\0') ? rc->callsign : "N0CALL";
    if (s_cfg.ssid > 0) {
        snprintf(out, out_size, "%s-%u", call, (unsigned)s_cfg.ssid);
    } else {
        snprintf(out, out_size, "%s", call);
    }
}

// -------------------------------------------------------------------- NMEA --

double nmeaCoordToDeg(const char *field, const char *hemi)
{
    // ddmm.mmmm / dddmm.mmmm
    const double raw = atof(field);
    const double deg = floor(raw / 100.0);
    const double min = raw - deg * 100.0;
    double val = deg + min / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') {
        val = -val;
    }
    return val;
}

// Split a NMEA sentence into comma fields (in place). Returns field count.
size_t splitNmea(char *line, const char *fields[], size_t max_fields)
{
    size_t count = 0;
    char *p = line;
    while (count < max_fields) {
        fields[count++] = p;
        char *comma = strchr(p, ',');
        if (comma == nullptr) {
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    return count;
}

int16_t nmeaOptionalInt(const char *field, int minimum, int maximum)
{
    if (field == nullptr || field[0] == '\0') return -1;
    char *end = nullptr;
    const long value = strtol(field, &end, 10);
    return end != field && *end == '\0' && value >= minimum && value <= maximum
               ? static_cast<int16_t>(value)
               : -1;
}

void updateGsvSatellites(const char talker[3], const AprsGpsSatelliteInfo *satellites,
                         const size_t count, const bool starts_cycle,
                         const uint32_t received_ms)
{
    portENTER_CRITICAL(&s_gps_mux);
    if (starts_cycle) {
        size_t write = 0;
        for (size_t read = 0; read < s_gps_visible_count; ++read) {
            if (strncmp(s_gps_visible[read].talker, talker, 2) != 0) {
                if (write != read) {
                    s_gps_visible[write] = s_gps_visible[read];
                    s_gps_visible_seen_ms[write] = s_gps_visible_seen_ms[read];
                }
                ++write;
            }
        }
        s_gps_visible_count = static_cast<uint8_t>(write);
    }
    for (size_t incoming = 0; incoming < count; ++incoming) {
        size_t target = s_gps_visible_count;
        for (size_t existing = 0; existing < s_gps_visible_count; ++existing) {
            if (s_gps_visible[existing].prn == satellites[incoming].prn &&
                strncmp(s_gps_visible[existing].talker, talker, 2) == 0) {
                target = existing;
                break;
            }
        }
        if (target == s_gps_visible_count) {
            if (s_gps_visible_count >= APRS_GPS_SATELLITE_MAX) continue;
            ++s_gps_visible_count;
        }
        s_gps_visible[target] = satellites[incoming];
        s_gps_visible_seen_ms[target] = received_ms;
    }
    s_gps_last_gsv_ms = received_ms;
    portEXIT_CRITICAL(&s_gps_mux);
}

void handleNmeaLine(char *line)
{
    // strip checksum
    char *star = strchr(line, '*');
    if (star != nullptr) {
        *star = '\0';
    }
    if (line[0] != '$' || strlen(line) < 10) {
        return;
    }
    const char *type = line + 3; // skip "$GP"/"$GN"/"$BD"...

    const char *f[20] = {nullptr};
    if (strncmp(type, "RMC", 3) == 0) {
        const size_t n = splitNmea(line, f, 20);
        const uint32_t received_ms = nowMs();
        // $xxRMC,time,status,lat,NS,lon,EW,speed_kn,course,date,...
        if (n >= 9 && f[2][0] == 'A' && f[3][0] != '\0' && f[5][0] != '\0') {
            const double latitude = nmeaCoordToDeg(f[3], f[4]);
            const double longitude = nmeaCoordToDeg(f[5], f[6]);
            const float speed_kmh = (f[7][0] != '\0')
                                        ? static_cast<float>(atof(f[7]) * 1.852)
                                        : NAN;
            const bool course_valid = f[8][0] != '\0';
            const uint16_t course = (f[8][0] != '\0')
                                        ? static_cast<uint16_t>(atof(f[8]))
                                        : 0u;
            portENTER_CRITICAL(&s_gps_mux);
            s_gps_last_nmea_ms = received_ms;
            s_gps_last_rmc_ms = received_ms;
            s_gps_lat = latitude;
            s_gps_lon = longitude;
            s_gps_speed_kmh = speed_kmh;
            s_gps_course = course;
            s_gps_course_valid = course_valid;
            s_gps_last_fix_ms = received_ms;
            s_gps_fix = true;
            portEXIT_CRITICAL(&s_gps_mux);
        } else if (n >= 3 && f[2][0] == 'V') {
            portENTER_CRITICAL(&s_gps_mux);
            s_gps_last_nmea_ms = received_ms;
            s_gps_last_rmc_ms = received_ms;
            s_gps_fix = false;
            portEXIT_CRITICAL(&s_gps_mux);
        } else {
            portENTER_CRITICAL(&s_gps_mux);
            s_gps_last_nmea_ms = received_ms;
            s_gps_last_rmc_ms = received_ms;
            portEXIT_CRITICAL(&s_gps_mux);
        }
    } else if (strncmp(type, "GGA", 3) == 0) {
        const size_t n = splitNmea(line, f, 20);
        const uint32_t received_ms = nowMs();
        // $xxGGA,time,lat,NS,lon,EW,fix,sats,hdop,alt,M,...
        const long quality_value = (n >= 7 && f[6][0] != '\0')
                                       ? strtol(f[6], nullptr, 10)
                                       : 0l;
        const uint8_t quality = (quality_value >= 0l && quality_value <= 8l)
                                    ? static_cast<uint8_t>(quality_value)
                                    : 0u;
        const long satellite_value = (n >= 8 && f[7][0] != '\0')
                                         ? strtol(f[7], nullptr, 10)
                                         : -1l;
        const int16_t satellites = (satellite_value >= 0l && satellite_value <= 99l)
                                       ? static_cast<int16_t>(satellite_value)
                                       : -1;
        const float hdop = (n >= 9 && f[8][0] != '\0')
                               ? static_cast<float>(atof(f[8]))
                               : NAN;
        const bool valid_fix = n >= 10 && quality > 0u && f[2][0] != '\0' &&
                               f[4][0] != '\0';
        const double latitude = valid_fix ? nmeaCoordToDeg(f[2], f[3]) : 0.0;
        const double longitude = valid_fix ? nmeaCoordToDeg(f[4], f[5]) : 0.0;
        const bool altitude_valid = valid_fix && f[9][0] != '\0';
        const double altitude_m = altitude_valid ? atof(f[9]) : 0.0;

        portENTER_CRITICAL(&s_gps_mux);
        s_gps_last_nmea_ms = received_ms;
        s_gps_last_gga_ms = received_ms;
        s_gps_fix_quality = quality;
        s_gps_satellites = satellites;
        s_gps_hdop = hdop;
        if (valid_fix) {
            s_gps_lat = latitude;
            s_gps_lon = longitude;
            if (altitude_valid) s_gps_alt_m = altitude_m;
            s_gps_last_fix_ms = received_ms;
            s_gps_fix = true;
        } else {
            s_gps_fix = false;
        }
        portEXIT_CRITICAL(&s_gps_mux);
    } else if (strncmp(type, "GSV", 3) == 0) {
        const size_t n = splitNmea(line, f, 20);
        const int16_t message_number = n >= 3 ? nmeaOptionalInt(f[2], 1, 99) : -1;
        if (n >= 4 && message_number > 0) {
            AprsGpsSatelliteInfo parsed[4] = {};
            size_t parsed_count = 0;
            const char talker[3] = {line[1], line[2], '\0'};
            for (size_t base = 4; base + 3 < n && parsed_count < 4; base += 4) {
                const int16_t prn = nmeaOptionalInt(f[base], 1, 999);
                if (prn < 0) continue;
                AprsGpsSatelliteInfo &satellite = parsed[parsed_count++];
                satellite.talker[0] = talker[0];
                satellite.talker[1] = talker[1];
                satellite.talker[2] = '\0';
                satellite.prn = static_cast<uint16_t>(prn);
                satellite.elevation_deg = nmeaOptionalInt(f[base + 1], 0, 90);
                satellite.azimuth_deg = nmeaOptionalInt(f[base + 2], 0, 359);
                satellite.snr_dbhz = nmeaOptionalInt(f[base + 3], 0, 99);
            }
            updateGsvSatellites(talker, parsed, parsed_count, message_number == 1,
                                nowMs());
        }
    }
}

void drainNmeaRing()
{
    while (s_nmea_tail != s_nmea_head) {
        const uint8_t c = s_nmea_ring[s_nmea_tail];
        s_nmea_tail = (s_nmea_tail + 1) % kNmeaRingSize;
        if (c == '\n' || c == '\r') {
            if (s_nmea_line_len > 0) {
                s_nmea_line[s_nmea_line_len] = '\0';
                handleNmeaLine(s_nmea_line);
                s_nmea_line_len = 0;
            }
        } else if (s_nmea_line_len < sizeof(s_nmea_line) - 1) {
            if (s_nmea_line_len == 0 && c != '$') {
                continue; // resync on sentence start
            }
            s_nmea_line[s_nmea_line_len++] = (char)c;
        } else {
            s_nmea_line_len = 0;
        }
    }
}

void pollGpsSerial()
{
    // GPS power off: leave the UART2 driver down (GPS_SERIAL_Read would
    // otherwise re-install it on the next call).
    if (!s_cfg.gps_power_enabled) return;
    uint8_t bytes[192];
    for (;;) {
        const size_t count = GPS_SERIAL_Read(bytes, sizeof(bytes));
        if (count == 0u) break;
        size_t head = s_nmea_head;
        for (size_t i = 0; i < count; ++i) {
            const size_t next = (head + 1u) % kNmeaRingSize;
            if (next == s_nmea_tail) break;
            s_nmea_ring[head] = bytes[i];
            head = next;
        }
        s_nmea_head = head;
        if (count < sizeof(bytes)) break;
    }
    drainNmeaRing();
}

// Applies the persisted GPS power switch: the PCA9555 GPS_EN line on
// bh4tdv_rf (a stub returning false on boards without the hardware) plus
// the UART2 NMEA intake. Powering off also drops the cached fix/NMEA
// timestamps so GpsHasFix/GetGpsInfo report "disconnected" right away and
// beacons fall back to the default position per the fixed-beacon rule.
void applyGpsPowerState()
{
    (void)BH4TDV_RF_IO_SetGpsPower(s_cfg.gps_power_enabled);
    if (s_cfg.gps_power_enabled) {
        if (!GPS_SERIAL_Init()) {
            ESP_LOGW(TAG, "GPS UART2 initialization failed; using default position");
        }
        return;
    }
    GPS_SERIAL_Stop();
    portENTER_CRITICAL(&s_gps_mux);
    s_gps_fix = false;
    s_gps_fix_quality = 0;
    s_gps_satellites = -1;
    s_gps_last_fix_ms = 0;
    s_gps_last_nmea_ms = 0;
    s_gps_last_rmc_ms = 0;
    s_gps_last_gga_ms = 0;
    s_gps_last_gsv_ms = 0;
    s_gps_visible_count = 0;
    portEXIT_CRITICAL(&s_gps_mux);
}

bool ownPosition(double *lat, double *lon, double *alt_m)
{
    const bool fresh = s_gps_fix && (nowMs() - s_gps_last_fix_ms) < kGpsFixFreshMs;
    if (fresh) {
        *lat = s_gps_lat;
        *lon = s_gps_lon;
        if (alt_m != nullptr) {
            *alt_m = s_gps_alt_m;
        }
        return true;
    }
    *lat = (double)s_cfg.default_lat_e6 / 1e6;
    *lon = (double)s_cfg.default_lon_e6 / 1e6;
    if (alt_m != nullptr) {
        *alt_m = 0.0;
    }
    return false;
}

bool gpsFixFresh()
{
    return s_gps_fix && (nowMs() - s_gps_last_fix_ms) < kGpsFixFreshMs;
}

bool beaconPositionAllowed()
{
    return gpsFixFresh() || s_cfg.fixed_beacon_without_gps;
}

// ---------------------------------------------------------------- stations --

void copyDisplayText(char *out, size_t out_size, const char *text, size_t text_len)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    size_t used = 0u;
    bool pending_space = false;
    for (size_t i = 0u; i < text_len && used + 1u < out_size; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            pending_space = used != 0u;
            continue;
        }
        if (c < 0x20u || c == 0x7fu) {
            continue;
        }
        if (pending_space) {
            if (used + 2u >= out_size) {
                break;
            }
            out[used++] = ' ';
        }
        pending_space = false;
        if (used + 1u >= out_size) {
            break;
        }
        out[used++] = static_cast<char>(c);
    }
    out[used] = '\0';
}

void buildPacketSummary(const struct pbuf_t *pb, char *out, size_t out_size)
{
    if (pb == nullptr || out == nullptr || out_size == 0u) {
        return;
    }
    out[0] = '\0';

    char source[12] = {};
    size_t source_len = 0u;
    if (pb->srccall_end != nullptr && pb->srccall_end >= pb->data) {
        source_len = static_cast<size_t>(pb->srccall_end - pb->data);
    }
    if (source_len == 0u || source_len >= sizeof(source)) {
        source_len = pb->srcname_len < sizeof(source) - 1u
                         ? pb->srcname_len
                         : sizeof(source) - 1u;
        if (pb->srcname != nullptr && source_len > 0u) {
            memcpy(source, pb->srcname, source_len);
        }
    } else {
        memcpy(source, pb->data, source_len);
    }
    source[source_len] = '\0';
    if (source[0] == '\0') {
        snprintf(source, sizeof(source), "UNKNOWN");
    }

    char description[APRS_COMMENT_MAX_BYTES + 32u] = {};
    if (pb->comment != nullptr && pb->comment_len > 0u) {
        copyDisplayText(description, sizeof(description), pb->comment, pb->comment_len);
    } else if (pb->info_start != nullptr && pb->packet_len > 0) {
        const char *packet_end = pb->data + pb->packet_len;
        if (pb->info_start < packet_end) {
            const char *body = pb->info_start + 1;
            size_t body_len = static_cast<size_t>(packet_end - body);
            // APRS text message: :ADDRESSEE:body{message-id
            if (pb->info_start[0] == ':' && body_len >= 10u && body[9] == ':') {
                body += 10;
                body_len -= 10u;
                const char *message_id = static_cast<const char *>(memchr(body, '{', body_len));
                if (message_id != nullptr) {
                    body_len = static_cast<size_t>(message_id - body);
                }
            }
            copyDisplayText(description, sizeof(description), body, body_len);
        }
    }

    if (pb->flags & F_HASPOS) {
        char distance[16] = "--km";
        double own_lat = 0.0, own_lon = 0.0;
        ownPosition(&own_lat, &own_lon, nullptr);
        if (own_lat != 0.0 || own_lon != 0.0) {
            snprintf(distance, sizeof(distance), "%.1fkm",
                     s_parser.distance(own_lon, own_lat, pb->lng, pb->lat));
        }
        char speed[20] = {};
        if (pb->flags & F_CSRSPD) {
            snprintf(speed, sizeof(speed), " %.0fkm/h", pb->speed);
        }
        snprintf(out, out_size, "%s %s%s%s%s", source, distance, speed,
                 description[0] != '\0' ? " " : "", description);
    } else {
        snprintf(out, out_size, "%s%s%s", source,
                 description[0] != '\0' ? " " : "", description);
    }
}

void touchLastPacket(const char *tnc2, const char *summary)
{
    lockStations(); // shared with the last-packet UI snapshot getters
    strncpy(s_last_packet, tnc2, sizeof(s_last_packet) - 1);
    s_last_packet[sizeof(s_last_packet) - 1] = '\0';
    strncpy(s_last_packet_summary, summary, sizeof(s_last_packet_summary) - 1);
    s_last_packet_summary[sizeof(s_last_packet_summary) - 1] = '\0';
    s_last_packet_seq++;
    unlockStations();
}

void updateStation(const struct pbuf_t *pb, bool via_rf)
{
    if (!(pb->flags & F_HASPOS) || pb->srcname == nullptr || pb->srcname_len == 0) {
        return;
    }

    char name[10] = {0};
    const size_t name_len =
        (pb->srcname_len < sizeof(name) - 1) ? pb->srcname_len : sizeof(name) - 1;
    memcpy(name, pb->srcname, name_len);

    const uint32_t now = nowMs();

    lockStations();
    StationRec *slot = nullptr;
    StationRec *free_slot = nullptr;
    StationRec *oldest = nullptr;
    for (size_t i = 0; i < kStationCount; ++i) {
        StationRec &rec = s_stations[i];
        if (rec.used && strcmp(rec.name, name) == 0) {
            slot = &rec;
            break;
        }
        if (!rec.used) {
            if (free_slot == nullptr) {
                free_slot = &rec;
            }
        } else if (oldest == nullptr ||
                   (int32_t)(rec.last_heard_ms - oldest->last_heard_ms) < 0) {
            oldest = &rec;
        }
    }
    if (slot == nullptr) {
        slot = (free_slot != nullptr) ? free_slot : oldest;
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->name, name, sizeof(slot->name));
        slot->altitude_m = NAN;
        slot->speed_kmh = NAN;
        slot->derived_speed_kmh = NAN;
        slot->distance_km = NAN;
        slot->used = true;
    }

    // speed derived from consecutive packets of the same station (>5 s apart
    // so position jitter doesn't explode the figure)
    if (slot->pkt_count > 0) {
        const uint32_t dt_ms = now - slot->last_heard_ms;
        if (dt_ms > 5000u) {
            const double moved_km =
                s_parser.distance(slot->lon, slot->lat, pb->lng, pb->lat);
            const double hours = (double)dt_ms / 3600000.0;
            if (moved_km < 1000.0) {
                slot->derived_speed_kmh = (float)(moved_km / hours);
            }
        }
    }

    slot->lat = pb->lat;
    slot->lon = pb->lng;
    memcpy(slot->symbol, pb->symbol, sizeof(slot->symbol));
    if (pb->flags & F_ALT) {
        slot->altitude_m = (float)pb->altitude;
    }
    if (pb->flags & F_CSRSPD) {
        slot->speed_kmh = (float)pb->speed;
        slot->course_deg = pb->course;
    }
    if (pb->comment != nullptr && pb->comment_len > 0) {
        copyUtf8Text(slot->comment, sizeof(slot->comment),
                     pb->comment, pb->comment_len);
    }

    double own_lat = 0.0, own_lon = 0.0;
    ownPosition(&own_lat, &own_lon, nullptr);
    if (own_lat != 0.0 || own_lon != 0.0) {
        slot->distance_km =
            (float)s_parser.distance(own_lon, own_lat, slot->lon, slot->lat);
        slot->bearing_deg =
            (uint16_t)s_parser.direction(own_lon, own_lat, slot->lon, slot->lat);
    }

    slot->last_heard_ms = now;
    slot->pkt_count++;
    slot->via_rf = via_rf ? 1 : 0;
    s_station_revision++;
    unlockStations();
}

// ------------------------------------------------------------- TNC2 parsing --

// Prepare a pbuf_t from a TNC2 monitor line ("SRC>DST,PATH:info") and run the
// aprsc parser over it (same pointer preparation as upstream dispWindow()).
bool parseTnc2(const char *line_in, bool via_rf)
{
    size_t len = strlen(line_in);
    if (len < 10 || len >= 290) {
        return false;
    }

    // third-party traffic ("...:}SRC>DST,...") -- parse the inner packet
    const char *third = strstr(line_in, ":}");
    if (third != nullptr) {
        const char *inner = third + 2;
        const char *gt = strchr(inner, '>');
        if (gt != nullptr && (gt - inner) > 3 && (gt - inner) < 12) {
            line_in = inner;
            len = strlen(line_in);
        }
    }

    const char *gt = strchr(line_in, '>');
    const char *colon = strchr(line_in, ':');
    if (gt == nullptr || colon == nullptr || (gt - line_in) < 4 ||
        (gt - line_in) > 11 || colon < gt) {
        return false;
    }

    static struct pbuf_t pb; // large; keep off the task stack
    memset(&pb, 0, sizeof(pb));
    pb.buf_len = sizeof(pb.data);
    pb.packet_len = (int)len;
    memcpy(pb.data, line_in, len);
    pb.data[len] = '\0';

    const size_t gt_off = (size_t)(gt - line_in);
    const size_t colon_off = (size_t)(colon - line_in);

    const char *comma = strchr(line_in + gt_off, ',');
    size_t end_ssid_off =
        (comma != nullptr && (size_t)(comma - line_in) < colon_off)
            ? (size_t)(comma - line_in)
            : colon_off;

    const char *dash = strchr(line_in + gt_off, '-');
    if (dash != nullptr && (size_t)(dash - line_in) < end_ssid_off) {
        pb.dstcall_end_or_ssid = &pb.data[dash - line_in];
    } else {
        pb.dstcall_end_or_ssid = &pb.data[end_ssid_off];
    }
    pb.info_start = &pb.data[colon_off + 1];
    pb.dstname = &pb.data[gt_off + 1];
    pb.dstname_len = (uint8_t)(end_ssid_off - gt_off - 1);
    pb.dstcall_end = &pb.data[end_ssid_off];
    pb.srccall_end = &pb.data[gt_off];
    // The parser replaces this with an object/item name when appropriate.
    // Ordinary station packets otherwise need the TNC2 source as their name.
    pb.srcname = pb.data;
    pb.srcname_len = static_cast<uint8_t>(gt_off);

    if (s_parser.parse_aprs(&pb) == 0) {
        return false;
    }

    updateStation(&pb, via_rf);
    char summary[sizeof(s_last_packet_summary)] = {};
    buildPacketSummary(&pb, summary, sizeof(summary));
    touchLastPacket(line_in, summary);
    return true;
}

// --------------------------------------------------------------- APRS-IS ----

// Gateway helpers implemented further down in the RF RX section.
bool fwdIsOwnSource(const char *line, const char *call);
bool fwdToAfsk(const char *line, bool sanitize, bool add_own_digi);

void isDisconnect()
{
    if (s_is_socket >= 0) {
        close(s_is_socket);
        s_is_socket = -1;
    }
    s_is_logged_in = false;
    s_is_verified = false;
    s_is_status_sent = false;
    s_is_status_satellites = -2;
    s_is_status_battery_mv = -1;
    s_is_last_status_ms = 0;
    s_is_line_len = 0;
}

void isSendLine(const char *line)
{
    if (s_is_socket < 0) {
        return;
    }
    char buf[1024];
    const int n = snprintf(buf, sizeof(buf), "%s\r\n", line);
    if (n > 0) {
        if (send(s_is_socket, buf, (size_t)n, 0) < 0) {
            ESP_LOGW(TAG, "APRS-IS send failed, reconnecting");
            isDisconnect();
        }
    }
}

void isTryConnect()
{
    const uint32_t now = nowMs();
    if ((now - s_is_last_attempt_ms) < kIsReconnectMs && s_is_last_attempt_ms != 0) {
        return;
    }
    s_is_last_attempt_ms = now;

    char host[65];
    uint16_t port;
    lockCfg();
    strncpy(host, s_cfg.server_host, sizeof(host));
    host[sizeof(host) - 1] = '\0';
    port = s_cfg.server_port;
    unlockCfg();

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) {
        ESP_LOGW(TAG, "APRS-IS DNS failed for %s", host);
        return;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return;
    }
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "APRS-IS connect to %s:%u failed", host, (unsigned)port);
        close(sock);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);

    // non-blocking from here on; the task polls recv()
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    s_is_socket = sock;

    char call[16];
    lockCfg();
    myCallsign(call, sizeof(call));
    unlockCfg();
    char basecall[16];
    strncpy(basecall, call, sizeof(basecall));
    basecall[sizeof(basecall) - 1] = '\0';
    char *dash = strchr(basecall, '-');
    if (dash != nullptr) {
        *dash = '\0';
    }
    const uint16_t pass = s_parser.passCode(basecall);

    char login[160];
    snprintf(login, sizeof(login),
             "user %s pass %u vers NRL-ESP32 1.0 filter m/100", call, (unsigned)pass);
    isSendLine(login);
    s_is_logged_in = true;
    s_is_verified = false; // until "# logresp ... verified" arrives
    ESP_LOGI(TAG, "APRS-IS connected to %s:%u as %s", host, (unsigned)port, call);
}

// '#' lines are server chatter. The only one acted on is the login
// verdict: "# logresp <call> verified, server ..." (or "unverified" on
// a bad passcode). s_is_logged_in is set optimistically when the login
// line goes out; the FMO STATION broadcast additionally requires this
// verified flag.
void handleIsServerLine(const char *line)
{
    if (strncmp(line, "# logresp", 9) != 0) {
        return;
    }
    if (strstr(line, "unverified") != nullptr) {
        s_is_verified = false;
        ESP_LOGW(TAG, "APRS-IS login rejected: %s", line);
    } else if (strstr(line, "verified") != nullptr) {
        s_is_verified = true;
        ESP_LOGI(TAG, "APRS-IS login verified: %s", line);
    }
}

void isPoll()
{
    if (s_is_socket < 0) {
        return;
    }
    char buf[256];
    for (;;) {
        const int n = recv(s_is_socket, buf, sizeof(buf), 0);
        if (n == 0) {
            ESP_LOGW(TAG, "APRS-IS closed by server");
            isDisconnect();
            return;
        }
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return;
            }
            ESP_LOGW(TAG, "APRS-IS recv error %d", errno);
            isDisconnect();
            return;
        }
        for (int i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (s_is_line_len > 0) {
                    s_is_line[s_is_line_len] = '\0';
                    if (s_is_line[0] == '#') { // '#' = server chatter
                        handleIsServerLine(s_is_line);
                    } else {
                        parseTnc2(s_is_line, false);
                        // Gateway: relay IS traffic onto the AFSK channels.
                        // Each direction also needs its transmitter route on;
                        // IS-only header entries get stripped before TX.
                        lockCfg();
                        const bool is_to_afsk =
                            (s_cfg.fwd[APRS_FWD_IS_TO_RF] && s_cfg.rf_tx_enabled) ||
                            (s_cfg.fwd[APRS_FWD_IS_TO_NRL] && s_cfg.nrl_tx_enabled);
                        char call[16];
                        myCallsign(call, sizeof(call));
                        unlockCfg();
                        if (is_to_afsk && !fwdIsOwnSource(s_is_line, call)) {
                            fwdToAfsk(s_is_line, true, false);
                        }
                    }
                    s_is_line_len = 0;
                }
            } else if (s_is_line_len < sizeof(s_is_line) - 1) {
                s_is_line[s_is_line_len++] = c;
            } else {
                s_is_line_len = 0;
            }
        }
    }
}

// ----------------------------------------------------------------- beacons --

// "!DDMM.mmN/DDDMM.mmE&123/045/A=001234 comment"
bool buildPositionInfo(char *out, size_t out_size, const char *comment)
{
    if (!beaconPositionAllowed()) {
        if (out != nullptr && out_size > 0u) {
            out[0] = '\0';
        }
        return false;
    }

    double lat = 0.0, lon = 0.0, alt = 0.0;
    ownPosition(&lat, &lon, &alt);

    const std::string lat_str = s_parser.deg2lat(lat);
    const std::string lon_str = s_parser.deg2lon(lon);
    const long alt_ft = lround(alt * 3.28084);

    // APRS course/speed data extension: ccc/sss, degrees and knots. Only
    // advertise it while the RMC fix is fresh; a configured fallback position
    // must not look like a live moving GPS station. APRS represents north as
    // 360 degrees rather than 000.
    char motion[9] = {};
    const bool gps_fresh = s_gps_fix &&
                           (nowMs() - s_gps_last_fix_ms) < kGpsFixFreshMs;
    if (gps_fresh && isfinite(s_gps_speed_kmh) && s_gps_speed_kmh >= 0.0f) {
        const unsigned course = (s_gps_course % 360u) == 0u
                                    ? 360u
                                    : static_cast<unsigned>(s_gps_course % 360u);
        long speed_knots = lround(static_cast<double>(s_gps_speed_kmh) / 1.852);
        if (speed_knots < 0) speed_knots = 0;
        if (speed_knots > 999) speed_knots = 999;
        snprintf(motion, sizeof(motion), "%03u/%03ld", course, speed_knots);
    }

    if (!gps_fresh) {
        // The configured fallback coordinates keep the station visible, but
        // do not attach zero/stale altitude or motion fields to them.
        if (comment != nullptr && comment[0] != '\0') {
            snprintf(out, out_size, "!%s%c%s%c %s",
                     lat_str.c_str(), s_cfg.symbol_table, lon_str.c_str(),
                     s_cfg.symbol_code, comment);
        } else {
            snprintf(out, out_size, "!%s%c%s%c",
                     lat_str.c_str(), s_cfg.symbol_table, lon_str.c_str(),
                     s_cfg.symbol_code);
        }
    } else if (comment != nullptr && comment[0] != '\0') {
        snprintf(out, out_size, "!%s%c%s%c%s/A=%06ld %s",
                 lat_str.c_str(), s_cfg.symbol_table, lon_str.c_str(),
                 s_cfg.symbol_code, motion, alt_ft, comment);
    } else {
        snprintf(out, out_size, "!%s%c%s%c%s/A=%06ld",
                 lat_str.c_str(), s_cfg.symbol_table, lon_str.c_str(),
                 s_cfg.symbol_code, motion, alt_ft);
    }
    return true;
}

bool sendBeacon()
{
    lockCfg();
    const bool net = s_cfg.net_enabled;
    // One modem TX queue feeds both audio destinations; the router fan-out
    // (see applyRouteState) decides speaker and/or NRL uplink.
    const bool rf = s_cfg.rf_tx_enabled || s_cfg.nrl_tx_enabled;
    const bool can_beacon = beaconPositionAllowed();
    char call[16];
    myCallsign(call, sizeof(call));
    char path[17];
    strncpy(path, s_cfg.path, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    char rf_info[257];

    const ExternalRadioConfig *radio = EXTERNAL_RADIO_GetConfig();
    const char *nrl_host = (radio != nullptr && radio->server_host[0] != '\0')
                               ? radio->server_host
                               : "unknown";
    const uint16_t nrl_port = (radio != nullptr) ? radio->server_port : 0u;
    char net_comment[300];
    // Speed/course/altitude already use APRS-standard ccc/sss and /A= fields
    // in buildPositionInfo(). Do not repeat them in the free-text comment:
    // map clients render those standard fields themselves.
    snprintf(net_comment, sizeof(net_comment), "@udp://%s:%u%s%s",
             nrl_host, (unsigned)nrl_port,
             s_cfg.comment[0] != '\0' ? " " : "", s_cfg.comment);
    char net_info[336];
    const bool have_rf_info = can_beacon &&
        buildPositionInfo(rf_info, sizeof(rf_info), s_cfg.comment);
    const bool have_net_info = can_beacon &&
        buildPositionInfo(net_info, sizeof(net_info), net_comment);
    unlockCfg();

    if (!have_rf_info && !have_net_info) {
        return false;
    }

    if (net && have_net_info && s_is_socket >= 0 && s_is_logged_in) {
        char line[384];
        snprintf(line, sizeof(line), "%s>NRLBOX,TCPIP*:%s", call, net_info);
        isSendLine(line);
        // Keep satellite count on the map site's board/firmware status row,
        // not in the position comment. Send immediately at login and when a
        // fix first appears; later count changes are rate-limited to avoid an
        // extra APRS-IS packet for every small constellation fluctuation.
        const uint32_t status_now = nowMs();
        const bool status_gps_fresh = s_gps_fix &&
                                      (status_now - s_gps_last_fix_ms) < kGpsFixFreshMs;
        const int16_t status_satellites =
            (status_gps_fresh && s_gps_satellites >= 0) ? s_gps_satellites : -1;
#if NRL_BOARD == NRL_BOARD_GEZIPAI || NRL_BOARD_IS_BI4UMD_FAMILY
        // Report the ADC result verbatim, including 0 V or implausible
        // high/low readings: those values are useful battery/sense diagnostics
        // and must not be hidden by a "reasonable voltage" filter.
        const int status_battery_mv = Display_GetBatteryCalibratedMv();
#else
        const int status_battery_mv = -1;
#endif
        const bool first_fix = s_is_status_satellites < 0 && status_satellites >= 0;
        const bool lost_fix = s_is_status_satellites >= 0 && status_satellites < 0;
        const bool satellite_changed = status_satellites != s_is_status_satellites;
        const bool battery_changed = status_battery_mv >= 0 &&
            (s_is_status_battery_mv < 0 ||
             abs(status_battery_mv - s_is_status_battery_mv) >= 100);
        const bool status_due = !s_is_status_sent ||
            ((satellite_changed || battery_changed) &&
             (first_fix || lost_fix ||
              (status_now - s_is_last_status_ms) >= kStatusSatelliteUpdateMs));
        if (status_due && s_is_socket >= 0 && s_is_logged_in) {
            snprintf(line, sizeof(line), "%s>NRLBOX,TCPIP*:>NRL-ESP32,%s,v%s",
                     call, boardType(), NRL_FIRMWARE_VERSION);
            size_t used = strlen(line);
            if (status_satellites >= 0 && used < sizeof(line)) {
                used += static_cast<size_t>(snprintf(
                    line + used, sizeof(line) - used, ",SAT=%d",
                    static_cast<int>(status_satellites)));
            }
            if (status_battery_mv >= 0 && used < sizeof(line)) {
                snprintf(line + used, sizeof(line) - used, ",BAT=%d.%02dV",
                         status_battery_mv / 1000,
                         (status_battery_mv % 1000) / 10);
            }
            isSendLine(line);
            s_is_status_sent = s_is_socket >= 0 && s_is_logged_in;
            if (s_is_status_sent) {
                s_is_status_satellites = status_satellites;
                s_is_status_battery_mv = status_battery_mv;
                s_is_last_status_ms = status_now;
            }
        }
        s_tx_count++;
    }

    if (rf && have_rf_info) {
        char frame_txt[320];
        if (path[0] != '\0') {
            snprintf(frame_txt, sizeof(frame_txt), "%s>NRLBOX,%s:%s", call, path, rf_info);
        } else {
            snprintf(frame_txt, sizeof(frame_txt), "%s>NRLBOX:%s", call, rf_info);
        }
        ax25frame frame;
        if (ax25_encode(frame, frame_txt, (int)strlen(frame_txt))) {
            static AX25Ctx ctx;
            static uint8_t raw[AX25_FRAME_MAX_SIZE];
            const int size = hdlcFrame(raw, sizeof(raw), &ctx, &frame);
            if (size > 0 && Ax25WriteTxFrame(raw, (uint16_t)size) != nullptr) {
                Ax25TransmitBuffer();
                s_tx_count++;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------------ RF TX ---

// Transmit the pending manual text message on every enabled path. Runs on
// the APRS task (socket + modem TX queue owner). The APRS message format is
// ":ADDRESSEE:payload" with the addressee field space-padded to 9 chars;
// a {seq suffix marks it as an acknowledged-style message.
void sendMessage()
{
    char dest[kMsgDestMax + 1u];
    char text[kMsgTextMax + 1u];
    snprintf(dest, sizeof(dest), "%s", s_msg_dest);
    snprintf(text, sizeof(text), "%s", s_msg_text);

    lockCfg();
    const bool net = s_cfg.net_enabled;
    const bool rf = s_cfg.rf_tx_enabled || s_cfg.nrl_tx_enabled;
    char call[16];
    myCallsign(call, sizeof(call));
    char path[17];
    strncpy(path, s_cfg.path, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    unlockCfg();

    // 9-char addressee field, right-padded with spaces per APRS spec.
    char addr[kMsgDestMax + 1u];
    memset(addr, ' ', kMsgDestMax);
    addr[kMsgDestMax] = '\0';
    memcpy(addr, dest, strlen(dest));

    s_msg_seq = (s_msg_seq + 1u) % 100000u;
    char info[200];
    snprintf(info, sizeof(info), ":%s:%s{%lu", addr, text,
             static_cast<unsigned long>(s_msg_seq));

    if (net && s_is_socket >= 0 && s_is_logged_in) {
        char line[384];
        snprintf(line, sizeof(line), "%s>NRLBOX,TCPIP*:%s", call, info);
        isSendLine(line);
        s_tx_count++;
        ESP_LOGI(TAG, "message -> IS: %s", info);
    }

    if (rf) {
        char frame_txt[320];
        if (path[0] != '\0') {
            snprintf(frame_txt, sizeof(frame_txt), "%s>NRLBOX,%s:%s", call, path, info);
        } else {
            snprintf(frame_txt, sizeof(frame_txt), "%s>NRLBOX:%s", call, info);
        }
        ax25frame frame;
        if (ax25_encode(frame, frame_txt, (int)strlen(frame_txt))) {
            static AX25Ctx ctx;
            static uint8_t raw[AX25_FRAME_MAX_SIZE];
            const int size = hdlcFrame(raw, sizeof(raw), &ctx, &frame);
            if (size > 0 && Ax25WriteTxFrame(raw, (uint16_t)size) != nullptr) {
                Ax25TransmitBuffer();
                s_tx_count++;
                ESP_LOGI(TAG, "message -> AFSK: %s", info);
            }
        }
    }
}

void txPump()
{
    static int16_t chunk[kTxChunkSamples];
    static int64_t tx_start_us = 0;
    static uint32_t tx_pushed_samples = 0;

    if (!ModemIsTransmitting()) {
        if (s_tx_pumping) {
            AudioFocus_NotifyVoiceEnd();
        }
        s_tx_pumping = false;
        return;
    }
    if (!s_tx_pumping) {
        s_tx_pumping = true;
        tx_start_us = esp_timer_get_time();
        tx_pushed_samples = 0;
        AudioFocus_NotifyVoiceStart(); // pause music playback for the beacon
    }
    lockCfg();
    const bool nrl_tx = s_cfg.enabled && s_cfg.nrl_tx_enabled;
    unlockCfg();
    // Keep only one 20 ms frame ahead of wall clock. A larger lead is harmless
    // for the queued speaker path, but the NRL sink sends synchronously: the
    // previous 200 ms lead emitted roughly ten voice packets back-to-back at
    // burst start and could destroy the AFSK preamble through loss/jitter.
    // One frame still covers normal task wake-up jitter without bursting the
    // network uplink.
    const int64_t elapsed_us = esp_timer_get_time() - tx_start_us;
    const uint32_t target = (uint32_t)(elapsed_us * MODEM_TX_SAMPLE_RATE / 1000000) +
                            kTxLeadSamples;
    while (tx_pushed_samples < target && ModemIsTransmitting()) {
        for (size_t i = 0; i < kTxChunkSamples; ++i) {
            const uint8_t s = MODEM_BAUDRATE_TIMER_HANDLER();
            chunk[i] = (int16_t)(((int)s - 128) * kTxAmplitude);
            if (!ModemIsTransmitting()) {
                // frame tail reached mid-chunk: pad with silence
                for (size_t k = i + 1; k < kTxChunkSamples; ++k) {
                    chunk[k] = 0;
                }
                break;
            }
        }
        AudioRouter_PushFrame(AUDIO_SRC_APRS, MODEM_TX_SAMPLE_RATE, chunk,
                              kTxChunkSamples);
        if (nrl_tx && !NRLAudioBridge_SendGeneratedPcm8k(chunk, kTxChunkSamples)) {
            ESP_LOGW(TAG, "NRL AFSK packet send failed");
        }
        tx_pushed_samples += kTxChunkSamples;
    }
}

// ------------------------------------------------------------------ RF RX ---

// ------------------------------------------------------------- gateway fwd --

// True when the TNC2 line was originated by this station (our own
// callsign-SSID right before '>'); such packets must never be relayed back
// towards any channel or the gateway loops on its own traffic.
bool fwdIsOwnSource(const char *line, const char *call)
{
    const size_t n = strlen(call);
    return strncmp(line, call, n) == 0 && line[n] == '>';
}

// True when the header path contains an APRS-IS q construct (",qAR," etc.):
// the packet already passed a gate and must not be gated to IS again.
bool fwdHasQConstruct(const char *line)
{
    const char *colon = strchr(line, ':');
    const size_t head_len = (colon != nullptr)
                                ? (size_t)(colon - line)
                                : strlen(line);
    for (size_t i = 1; i + 2 < head_len; ++i) {
        if (line[i] == ',' && line[i + 1] == 'q' && isalpha((unsigned char)line[i + 2])) {
            return true;
        }
    }
    return false;
}

// True when the header path already lists `call` as a digipeater. Used to
// stop RF<->NRL ping-pong between two gateway nodes: each hop appends its
// own callsign when relaying, so a returning copy is recognized and dropped.
bool fwdPathContainsCall(const char *line, const char *call)
{
    const char *gt = strchr(line, '>');
    const char *colon = strchr(line, ':');
    if (gt == nullptr || colon == nullptr || colon <= gt) {
        return false;
    }
    const size_t call_len = strlen(call);
    for (const char *p = gt + 1; p < colon; ++p) {
        if (*p != ',') continue;
        if ((size_t)(colon - (p + 1)) >= call_len &&
            strncmp(p + 1, call, call_len) == 0) {
            const char after = p[1 + call_len];
            if (after == ',' || after == ':' || after == '*') {
                return true;
            }
        }
    }
    return false;
}

// Upload a locally heard packet to APRS-IS with the qAR construct (iGate).
void fwdToIs(const char *line, const char *call)
{
    const char *colon = strchr(line, ':');
    if (colon == nullptr) {
        return;
    }
    char fwd[440];
    snprintf(fwd, sizeof(fwd), "%.*s,qAR,%s%s",
             (int)(colon - line), line, call, colon);
    isSendLine(fwd);
}

// Rebuild a TNC2 header stripping APRS-IS-only digipeaters (q constructs,
// TCPIP) so the frame is legal on an AFSK channel. Returns false when the
// line is malformed.
bool fwdSanitizeHeader(const char *line, char *out, size_t out_size)
{
    const char *colon = strchr(line, ':');
    const char *gt = strchr(line, '>');
    if (colon == nullptr || gt == nullptr || colon <= gt) {
        return false;
    }
    size_t used = 0;
    // Keep SRC>DST intact. The destination is the first token after '>' and
    // must not be emitted as a comma-prefixed digipeater.
    const char *first_comma = strchr(gt + 1, ',');
    const char *dst_end = (first_comma != nullptr && first_comma < colon)
                              ? first_comma
                              : colon;
    const size_t prefix_len = (size_t)(dst_end - line);
    if (dst_end == gt + 1 || prefix_len >= out_size) {
        return false;
    }
    memcpy(out, line, prefix_len);
    used = prefix_len;
    out[used] = '\0';
    // Walk the digipeater list. A q construct and the following APRS-IS
    // server/gateway form a pair, and neither belongs on an RF path.
    const char *p = (dst_end < colon) ? dst_end + 1 : colon;
    bool drop_q_argument = false;
    while (p < colon) {
        const char *comma = strchr(p, ',');
        const char *end = (comma != nullptr && comma < colon) ? comma : colon;
        const size_t tok_len = (size_t)(end - p);
        char tok[12] = {};
        const bool is_q = tok_len >= 2 && (p[0] == 'q' || p[0] == 'Q') &&
                           isalpha((unsigned char)p[1]);
        const bool is_tcpip = (tok_len == 5 || (tok_len == 6 && p[5] == '*')) &&
                              strncasecmp(p, "TCPIP", 5) == 0;
        const bool drop = drop_q_argument || is_q || is_tcpip;
        drop_q_argument = is_q;
        if (!drop && tok_len > 0 && tok_len < sizeof(tok)) {
            memcpy(tok, p, tok_len);
            // strip a trailing '*' (heard marker) -- we re-add ourselves below
            if (tok_len > 1 && tok[tok_len - 1] == '*') {
                tok[tok_len - 1] = '\0';
            }
            if (used + 1 + strlen(tok) + 1 < out_size) {
                out[used++] = ',';
                memcpy(out + used, tok, strlen(tok));
                used += strlen(tok);
                out[used] = '\0';
            }
        }
        p = (comma != nullptr && comma < colon) ? comma + 1 : colon;
    }
    if (used + strlen(colon) + 1 > out_size) {
        return false;
    }
    memcpy(out + used, colon, strlen(colon) + 1);
    return true;
}

// Inject a TNC2 line into the modem TX queue (AFSK out over whatever routes
// are enabled: speaker and/or NRL uplink). `sanitize` strips IS-only header
// entries first; `add_own_digi` appends our callsign to the path so other
// gateway nodes can detect the hop and break RF<->NRL loops. Runs on the
// APRS task only.
bool fwdToAfsk(const char *line, bool sanitize, bool add_own_digi)
{
    const uint32_t now = nowMs();
    if (now - s_last_fwd_afsk_ms < kFwdAfskMinGapMs) {
        return false;
    }
    char txt[400];
    if (sanitize) {
        if (!fwdSanitizeHeader(line, txt, sizeof(txt))) {
            return false;
        }
    } else {
        snprintf(txt, sizeof(txt), "%s", line);
    }
    if (add_own_digi) {
        char call[16];
        lockCfg();
        myCallsign(call, sizeof(call));
        unlockCfg();
        char *colon = strchr(txt, ':');
        if (colon != nullptr && strlen(txt) + strlen(call) + 1 < sizeof(txt)) {
            memmove(colon + strlen(call) + 1, colon, strlen(colon) + 1);
            *colon = ',';
            memcpy(colon + 1, call, strlen(call));
        }
    }
    ax25frame frame;
    if (!ax25_encode(frame, txt, (int)strlen(txt))) {
        return false;
    }
    static AX25Ctx ctx;
    static uint8_t raw[AX25_FRAME_MAX_SIZE];
    const int size = hdlcFrame(raw, sizeof(raw), &ctx, &frame);
    if (size <= 0 || Ax25WriteTxFrame(raw, (uint16_t)size) == nullptr) {
        return false;
    }
    Ax25TransmitBuffer();
    s_last_fwd_afsk_ms = now;
    ESP_LOGI(TAG, "gateway -> AFSK: %s", txt);
    return true;
}

// Audio-router sink callback (runs on the audio task): stash each tap into
// its own PSRAM ring; the APRS task resamples and demodulates both. Ring 0
// takes the mic (RF), ring 1 the NRL downlink.
void aprsSinkWrite(uint8_t source_id, const int16_t *samples,
                   size_t sample_count, void *)
{
    if (samples == nullptr || sample_count == 0u) {
        return;
    }
    int16_t *ring = s_rx_ring;
    volatile size_t *head_ptr = &s_rx_head;
    volatile size_t tail = s_rx_tail;
    if (source_id == AUDIO_SRC_NRL_DOWNLINK) {
        if (s_rx_nrl_ring == nullptr) {
            return;
        }
        ring = s_rx_nrl_ring;
        head_ptr = &s_rx_nrl_head;
        tail = s_rx_nrl_tail;
    } else if (s_rx_ring == nullptr) {
        return;
    }
    size_t head = *head_ptr;
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t next = (head + 1) % kRxRingSamples;
        if (next == tail) {
            break; // ring full; drop the rest
        }
        ring[head] = samples[i];
        head = next;
    }
    *head_ptr = head;
}

// Resample one 16 kHz ring to 9600 Hz (linear interpolation, 5:3) and feed
// the given demodulator instance sample-by-sample.
void rxDemodRing(const int16_t *ring, volatile size_t *head_ptr,
                 volatile size_t *tail_ptr, uint32_t *phase, int16_t *last,
                 uint32_t *level_acc, uint16_t *level_mv, uint32_t *level_n,
                 uint8_t demod)
{
    constexpr uint32_t kStep = (uint32_t)(65536.0 * 16000.0 / MODEM_RX_SAMPLE_RATE);
    size_t tail = *tail_ptr;
    const size_t head = *head_ptr;
    while (tail != head) {
        const int16_t sample = ring[tail];
        tail = (tail + 1) % kRxRingSamples;

        *level_acc += (uint32_t)abs(sample);
        if (++(*level_n) >= 1024) {
            // rough mean-abs -> mV-ish figure for the modem signal report
            *level_mv = (uint16_t)((*level_acc / 1024) >> 4);
            *level_acc = 0;
            *level_n = 0;
        }

        *phase += 65536u;
        while (*phase >= kStep) {
            *phase -= kStep;
            // interpolate between the previous and current input sample --
            // linear is plenty for AFSK tones at this oversampling. frac in
            // [0..256]: 0 = at the current sample, 256 = at the previous one.
            const int32_t frac =
                (int32_t)(((uint64_t)*phase << 8) / kStep);
            const int32_t out =
                (int32_t)sample +
                ((((int32_t)*last - (int32_t)sample) * frac) >> 8);
            MODEM_DECODE_CH((int16_t)out, *level_mv, demod);
        }
        *last = sample;
    }
    *tail_ptr = tail;
}

void rxDemodPoll()
{
    if (s_rx_ring != nullptr) {
        rxDemodRing(s_rx_ring, &s_rx_head, &s_rx_tail, &s_resample_phase,
                    &s_resample_last, &s_rx_level_acc, &s_rx_level_mv,
                    &s_rx_level_n, 0);
    }
    if (s_rx_nrl_ring != nullptr) {
        rxDemodRing(s_rx_nrl_ring, &s_rx_nrl_head, &s_rx_nrl_tail,
                    &s_resample_nrl_phase, &s_resample_nrl_last,
                    &s_rx_nrl_level_acc, &s_rx_nrl_level_mv,
                    &s_rx_nrl_level_n, 1);
    }
}

void rxFramePoll()
{
    while (Ax25NewRxFrames()) {
        uint8_t *frame = nullptr;
        uint16_t size = 0;
        int8_t peak = 0, valley = 0;
        uint8_t level = 0, corrected = 0;
        uint16_t mv = 0;
        uint8_t rx_source = 0;
        if (!Ax25ReadNextRxFrame(&frame, &size, &peak, &valley, &level,
                                 &corrected, &mv, &rx_source)) {
            break;
        }
        AX25Msg msg = {};
        ax25_decode(frame, size, mv, &msg);
        if (msg.ctrl != AX25_CTRL_UI || msg.pid != AX25_PID_NOLAYER3) {
            continue;
        }

        // rebuild the TNC2 monitor line
        char line[400];
        int off = snprintf(line, sizeof(line), "%s", msg.src.call);
        if (msg.src.ssid > 0) {
            off += snprintf(line + off, sizeof(line) - off, "-%u", msg.src.ssid);
        }
        off += snprintf(line + off, sizeof(line) - off, ">%s", msg.dst.call);
        if (msg.dst.ssid > 0) {
            off += snprintf(line + off, sizeof(line) - off, "-%u", msg.dst.ssid);
        }
        for (uint8_t i = 0; i < msg.rpt_count && off < (int)sizeof(line) - 16; ++i) {
            off += snprintf(line + off, sizeof(line) - off, ",%s", msg.rpt_list[i].call);
            if (msg.rpt_list[i].ssid > 0) {
                off += snprintf(line + off, sizeof(line) - off, "-%u", msg.rpt_list[i].ssid);
            }
            if (msg.rpt_flags & (1u << i)) {
                off += snprintf(line + off, sizeof(line) - off, "*");
            }
        }
        const size_t info_len =
            (msg.len < sizeof(line) - (size_t)off - 2) ? msg.len : sizeof(line) - off - 2;
        off += snprintf(line + off, sizeof(line) - off, ":");
        memcpy(line + off, msg.info, info_len);
        line[off + (int)info_len] = '\0';

        // Demodulator index tags the origin channel: 0 = mic (RF),
        // 1 = NRL network downlink.
        const bool via_nrl = rx_source == 1u;
        s_rx_count++;
        ESP_LOGI(TAG, "%s RX: %s", via_nrl ? "NRL" : "RF", line);
        parseTnc2(line, true);

        // Gateway: relay the packet per the forwarding switches. Own traffic
        // and already-gated packets (q constructs) are never re-gated to IS;
        // RF<->NRL hops append our callsign so returning copies are dropped.
        lockCfg();
        const bool net = s_cfg.net_enabled;
        const bool fwd_is = s_cfg.fwd[via_nrl ? APRS_FWD_NRL_TO_IS
                                              : APRS_FWD_RF_TO_IS];
        const bool fwd_afsk_sw = s_cfg.fwd[via_nrl ? APRS_FWD_NRL_TO_RF
                                                   : APRS_FWD_RF_TO_NRL];
        // AFSK relay only makes sense when the destination transmitter is on:
        // RF->NRL needs the NRL uplink, NRL->RF the speaker route.
        const bool fwd_afsk = fwd_afsk_sw &&
            (via_nrl ? s_cfg.rf_tx_enabled : s_cfg.nrl_tx_enabled);
        char call[16];
        myCallsign(call, sizeof(call));
        unlockCfg();
        if (fwdIsOwnSource(line, call)) {
            continue;
        }
        if (fwd_is && net && s_is_socket >= 0 && s_is_logged_in &&
            !fwdHasQConstruct(line)) {
            fwdToIs(line, call);
        }
        if (fwd_afsk && !fwdPathContainsCall(line, call)) {
            fwdToAfsk(line, false, true);
        }
    }
}

// -------------------------------------------------------------------- task --

void modemPtt(bool on)
{
    // VOX keying only: the TXDelay preamble gives the radio's VOX time to
    // open. Kept as a hook for boards that grow a hard PTT line.
    ESP_LOGI(TAG, "PTT %s", on ? "on" : "off");
    // Exclusive NRL uplink while the AFSK frame is on air: the modem audio
    // must be the only producer in the G.711/Opus accumulator, so the mic
    // (and BT headset mic) routes are parked for the duration.
    lockCfg();
    const bool nrl_tx = s_cfg.enabled && s_cfg.nrl_tx_enabled;
    unlockCfg();
    if (nrl_tx && !FMO_IsTransmitSelected()) {
        AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_NRL_UPLINK, !on);
        AudioRouter_SetRoute(AUDIO_SRC_BT_HFP_MIC, AUDIO_SINK_NRL_UPLINK, !on);
    }
}

void applyRouteState()
{
    lockCfg();
    const bool rx = s_cfg.enabled && s_cfg.rf_rx_enabled;
    const bool tx = s_cfg.enabled && s_cfg.rf_tx_enabled;
    const bool nrl_rx = s_cfg.enabled && s_cfg.nrl_rx_enabled;
    unlockCfg();
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_APRS, rx);
    AudioRouter_SetRoute(AUDIO_SRC_APRS, AUDIO_SINK_SPEAKER, tx);
    // NRL network audio path: 8 kHz AFSK is upsampled to the 16 kHz uplink
    // sink at the router edge; the downlink arrives 8 kHz and is upsampled
    // to the 16 kHz demodulator tap the same way.
    // APRS network TX bypasses the generic 16 kHz voice sink in txPump();
    // keep this route disabled to avoid duplicate packets and resampling.
    AudioRouter_SetRoute(AUDIO_SRC_APRS, AUDIO_SINK_NRL_UPLINK, false);
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_APRS, nrl_rx);
}

void aprsTask(void *)
{
    for (;;) {
        lockCfg();
        const bool enabled = s_cfg.enabled;
        const bool net = s_cfg.net_enabled;
        const uint16_t interval_s = s_cfg.beacon_interval_s;
        const bool auto_interval = s_cfg.auto_interval;
        unlockCfg();

        // UART2 is independent of SCI. Keep the latest fix warm even while
        // APRS transmission is disabled so enabling can beacon immediately.
        pollGpsSerial();

        if (!enabled) {
            if (s_is_socket >= 0) {
                isDisconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (s_is_reconnect_requested) {
            s_is_reconnect_requested = false;
            isDisconnect();
            s_is_last_attempt_ms = 0; // retry immediately with the new server
        }

        if (net) {
            if (s_is_socket < 0 && wifiIsConnected()) {
                isTryConnect();
            }
            isPoll();
        } else if (s_is_socket >= 0) {
            isDisconnect();
        }

        const uint32_t now = nowMs();
        uint32_t effective_ms = (uint32_t)interval_s * 1000u;
        bool moved_far = false;
        bool turned = false;
        if (auto_interval) {
            double lat = 0.0, lon = 0.0;
            if (ownPosition(&lat, &lon, nullptr)) { // true only on a fresh fix
                const double speed = (double)s_gps_speed_kmh;
                if (speed >= kAutoSlowSpeedKmh) {
                    double scaled_s = (double)kAutoMinIntervalS * kAutoFastSpeedKmh / speed;
                    if (scaled_s < (double)kAutoMinIntervalS) {
                        scaled_s = (double)kAutoMinIntervalS;
                    }
                    const uint32_t scaled_ms = (uint32_t)(scaled_s * 1000.0);
                    if (scaled_ms < effective_ms) {
                        effective_ms = scaled_ms;
                    }
                }
                if (s_last_beacon_pos_valid &&
                    s_parser.distance(s_last_beacon_lon, s_last_beacon_lat, lon, lat) >=
                        kAutoMoveKm) {
                    moved_far = true;
                }
                if (speed >= kAutoTurnMinSpeedKmh && s_gps_course_valid &&
                    s_last_beacon_course_valid) {
                    const unsigned current = s_gps_course % 360u;
                    const unsigned previous = s_last_beacon_course % 360u;
                    const unsigned raw_delta = current > previous
                                                   ? current - previous
                                                   : previous - current;
                    const unsigned turn_delta = raw_delta > 180u
                                                    ? 360u - raw_delta
                                                    : raw_delta;
                    // SmartBeaconing-style corner pegging: require a larger
                    // turn at low speed, where GPS course is less stable, and
                    // become more responsive as road speed increases.
                    double turn_threshold = 15.0 + 300.0 / speed;
                    if (turn_threshold < 20.0) turn_threshold = 20.0;
                    if (turn_threshold > 60.0) turn_threshold = 60.0;
                    turned = (double)turn_delta >= turn_threshold;
                }
            }
        }
        const uint32_t elapsed_ms = now - s_last_beacon_ms;
        const bool due = s_last_beacon_ms == 0 || elapsed_ms >= effective_ms;
        // A large position jump beacons early, but never more often than the
        // auto floor -- a fast mover is already pacing at that rate.
        const bool early = moved_far && elapsed_ms >= (uint32_t)kAutoMinIntervalS * 1000u;
        const bool corner = turned &&
            elapsed_ms >= (uint32_t)kAutoTurnMinIntervalS * 1000u;
        if (due || early || corner) {
            if (sendBeacon()) {
                s_last_beacon_ms = (now != 0u) ? now : 1u;
                s_last_beacon_pos_valid =
                    ownPosition(&s_last_beacon_lat, &s_last_beacon_lon, nullptr);
                s_last_beacon_course_valid = s_last_beacon_pos_valid &&
                                             s_gps_course_valid &&
                                             (double)s_gps_speed_kmh >= kAutoTurnMinSpeedKmh;
                if (s_last_beacon_course_valid) {
                    s_last_beacon_course = s_gps_course;
                }
            } else {
                const uint32_t retry_ms = 5000u;
                s_last_beacon_ms = (now > effective_ms)
                                       ? now - effective_ms + retry_ms
                                       : (now != 0u ? now : 1u);
                s_last_beacon_pos_valid = false;
                s_last_beacon_course_valid = false;
            }
        }

        Ax25TransmitCheck();
        if (s_msg_pending) {
            s_msg_pending = false;
            sendMessage();
        }
        if (s_raw_pending) {
            s_raw_pending = false;
            // Queued by APRS_SERVICE_SendRawLine (FMO STATION broadcast).
            if (s_is_socket >= 0 && s_is_logged_in) {
                isSendLine(s_raw_line);
                s_tx_count++;
            }
        }
        txPump();
        rxDemodPoll();
        rxFramePoll();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

} // namespace

// ------------------------------------------------------------------ public --

extern "C" void APRS_SERVICE_Init(void)
{
    if (s_task != nullptr) {
        return;
    }
    s_cfg_mutex = xSemaphoreCreateMutex();
    s_station_mutex = xSemaphoreCreateMutex();
    loadConfig();
    // The PCA9555 is not up yet at this point of boot, so the GPS_EN line
    // is (re)applied from main after BH4TDV_RF_IO_Init; the UART2 side of
    // the switch is fully handled here.
    applyGpsPowerState();

    memset(s_stations, 0, sizeof(s_station_storage));

    ModemConfig.modem = MODEM_1200;
    ModemConfig.flatAudioIn = 0;
    ModemInit();
    Ax25Init(0);
    ModemSetPttCallback(modemPtt);

    // Mic tap for the demodulator. Registered at 16 kHz: raw mic frames pass
    // through unconverted and 8 kHz AEC frames get upsampled at the edge.
    AudioRouter_RegisterSink(AUDIO_SINK_APRS, 16000u, aprsSinkWrite, nullptr);
    applyRouteState();

    if (xTaskCreatePinnedToCoreWithCaps(aprsTask, "aprs", 8192, nullptr, 5,
                                        &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        // PSRAM stack unavailable -- fall back to an internal-RAM stack
        if (xTaskCreate(aprsTask, "aprs", 6144, nullptr, 5, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "task create failed");
            s_task = nullptr;
        }
    }
    ESP_LOGI(TAG, "init: enabled=%d net=%d rf_tx=%d rf_rx=%d nrl_tx=%d nrl_rx=%d server=%s:%u",
             (int)s_cfg.enabled, (int)s_cfg.net_enabled, (int)s_cfg.rf_tx_enabled,
             (int)s_cfg.rf_rx_enabled, (int)s_cfg.nrl_tx_enabled,
             (int)s_cfg.nrl_rx_enabled, s_cfg.server_host, (unsigned)s_cfg.server_port);
}

extern "C" bool APRS_SERVICE_SetEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    s_last_beacon_ms = 0; // beacon immediately on (re)enable
    return true;
}

extern "C" bool APRS_SERVICE_IsEnabled(void)
{
    return s_cfg.enabled;
}

extern "C" bool APRS_SERVICE_SetNetEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.net_enabled = enabled;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetRfTxEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.rf_tx_enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    return true;
}

extern "C" bool APRS_SERVICE_SetRfRxEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.rf_rx_enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    return true;
}

extern "C" bool APRS_SERVICE_SetNrlTxEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.nrl_tx_enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    return true;
}

extern "C" bool APRS_SERVICE_SetNrlRxEnabled(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.nrl_rx_enabled = enabled;
    persistConfig();
    unlockCfg();
    applyRouteState();
    return true;
}

extern "C" bool APRS_SERVICE_SetFwdEnabled(AprsFwdDir dir, bool enabled)
{
    if (s_cfg_mutex == nullptr || (int)dir < 0 || dir >= APRS_FWD_COUNT) {
        return false;
    }
    lockCfg();
    s_cfg.fwd[dir] = enabled;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetServer(const char *host, uint16_t port)
{
    if (s_cfg_mutex == nullptr || host == nullptr || host[0] == '\0' || port == 0) {
        return false;
    }
    lockCfg();
    strncpy(s_cfg.server_host, host, sizeof(s_cfg.server_host) - 1);
    s_cfg.server_host[sizeof(s_cfg.server_host) - 1] = '\0';
    s_cfg.server_port = port;
    persistConfig();
    unlockCfg();
    s_is_reconnect_requested = true; // the APRS task owns the socket
    return true;
}

extern "C" bool APRS_SERVICE_SetSsid(uint8_t ssid)
{
    if (s_cfg_mutex == nullptr || ssid > 15) {
        return false;
    }
    lockCfg();
    s_cfg.ssid = ssid;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetSymbol(char table, char code)
{
    if (s_cfg_mutex == nullptr || table < '!' || table > '~' || code < '!' || code > '~') {
        return false;
    }
    lockCfg();
    s_cfg.symbol_table = table;
    s_cfg.symbol_code = code;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetBeaconInterval(uint16_t seconds)
{
    if (s_cfg_mutex == nullptr || seconds < kMinBeaconIntervalS ||
        seconds > kMaxBeaconIntervalS) {
        return false;
    }
    lockCfg();
    s_cfg.beacon_interval_s = seconds;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetAutoInterval(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.auto_interval = enabled;
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetFixedBeaconWithoutGps(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.fixed_beacon_without_gps = enabled;
    persistConfig();
    unlockCfg();
    if (enabled) {
        s_last_beacon_ms = 0;
    }
    return true;
}

extern "C" bool APRS_SERVICE_SetGpsPower(bool enabled)
{
    if (s_cfg_mutex == nullptr) {
        return false;
    }
    lockCfg();
    s_cfg.gps_power_enabled = enabled;
    persistConfig();
    unlockCfg();
    applyGpsPowerState();
    return true;
}

extern "C" bool APRS_SERVICE_GpsPowerEnabled(void)
{
    return s_cfg.gps_power_enabled;
}

extern "C" bool APRS_SERVICE_ParseAprsCoord(const char *text, bool is_lat, double *deg_out)
{
    if (text == nullptr || deg_out == nullptr) {
        return false;
    }
    while (*text == ' ') {
        ++text;
    }
    char buf[24] = {};
    strncpy(buf, text, sizeof(buf) - 1);
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == ' ') {
        buf[--len] = '\0';
    }
    if (len == 0) {
        return false;
    }

    double sign = 1.0;
    bool has_hemi = false;
    const char hemi = (char)toupper((unsigned char)buf[len - 1]);
    if (hemi == 'N' || hemi == 'S' || hemi == 'E' || hemi == 'W') {
        if (is_lat && hemi != 'N' && hemi != 'S') {
            return false;
        }
        if (!is_lat && hemi != 'E' && hemi != 'W') {
            return false;
        }
        sign = (hemi == 'S' || hemi == 'W') ? -1.0 : 1.0;
        buf[--len] = '\0';
        has_hemi = true;
    }
    if (len == 0) {
        return false;
    }

    char *end = nullptr;
    double v = strtod(buf, &end);
    if (end == buf || (end != nullptr && *end != '\0') || !isfinite(v)) {
        return false;
    }
    if (v < 0.0) {
        if (has_hemi) {
            return false; // "-3113.82N" is ambiguous
        }
        sign = -1.0;
        v = -v;
    }

    // ddmm.mmmm needs at least 3 (lat) / 4 (lon) integer digits; anything
    // smaller is read as plain decimal degrees (e.g. "45.7529").
    double deg = 0.0;
    if (v >= (is_lat ? 100.0 : 1000.0)) {
        const double d = floor(v / 100.0);
        const double m = v - d * 100.0;
        if (m >= 60.0) {
            return false;
        }
        deg = d + m / 60.0;
    } else {
        deg = v;
    }
    if ((is_lat && deg > 90.0) || (!is_lat && deg > 180.0)) {
        return false;
    }
    *deg_out = sign * deg;
    return true;
}

extern "C" void APRS_SERVICE_FormatAprsCoord(double deg, bool is_lat, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return;
    }
    const char hemi = (deg >= 0.0) ? (is_lat ? 'N' : 'E') : (is_lat ? 'S' : 'W');
    deg = fabs(deg);
    int d = (int)deg;
    double m = (deg - (double)d) * 60.0;
    if (m >= 59.99995) { // avoid "60.0000" after rounding
        m = 0.0;
        ++d;
    }
    snprintf(out, out_size, is_lat ? "%02d%07.4f%c" : "%03d%07.4f%c", d, m, hemi);
}

extern "C" bool APRS_SERVICE_SetDefaultPosition(double lat, double lon)
{
    if (s_cfg_mutex == nullptr || lat < -90.0 || lat > 90.0 || lon < -180.0 ||
        lon > 180.0) {
        return false;
    }
    lockCfg();
    s_cfg.default_lat_e6 = (int32_t)lround(lat * 1e6);
    s_cfg.default_lon_e6 = (int32_t)lround(lon * 1e6);
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetPath(const char *path)
{
    if (s_cfg_mutex == nullptr || path == nullptr) {
        return false;
    }
    lockCfg();
    strncpy(s_cfg.path, path, sizeof(s_cfg.path) - 1);
    s_cfg.path[sizeof(s_cfg.path) - 1] = '\0';
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" bool APRS_SERVICE_SetComment(const char *comment)
{
    if (s_cfg_mutex == nullptr || comment == nullptr) {
        return false;
    }
    const size_t comment_len = strlen(comment);
    if (comment_len > APRS_COMMENT_MAX_BYTES ||
        validUtf8Prefix(comment, comment_len, comment_len) != comment_len) {
        return false;
    }
    lockCfg();
    copyUtf8Text(s_cfg.comment, sizeof(s_cfg.comment), comment, comment_len);
    persistConfig();
    unlockCfg();
    return true;
}

extern "C" void APRS_SERVICE_GetConfig(AprsConfig *out)
{
    if (out == nullptr) {
        return;
    }
    if (s_cfg_mutex == nullptr) {
        defaultConfig(*out);
        return;
    }
    lockCfg();
    *out = s_cfg;
    unlockCfg();
}

extern "C" bool APRS_SERVICE_SendBeaconNow(void)
{
    if (s_task == nullptr || !s_cfg.enabled) {
        return false;
    }
    lockCfg();
    const bool can_beacon = beaconPositionAllowed();
    unlockCfg();
    if (!can_beacon) {
        return false;
    }
    s_last_beacon_ms = 0;
    return true;
}

extern "C" bool APRS_SERVICE_SendMessage(const char *dest, const char *text)
{
    if (s_task == nullptr || !s_cfg.enabled || dest == nullptr || text == nullptr) {
        return false;
    }
    // Addressee: A-Z 0-9 '-' only, at most 9 characters (callsign plus SSID).
    const size_t dest_len = strlen(dest);
    if (dest_len == 0u || dest_len > kMsgDestMax) {
        return false;
    }
    for (size_t i = 0u; i < dest_len; ++i) {
        const char c = dest[i];
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-';
        if (!valid) {
            return false;
        }
    }
    // Message body: valid UTF-8 only, bounded by the APRS message width.
    const size_t text_len = strlen(text);
    if (text_len == 0u || text_len > kMsgTextMax ||
        validUtf8Prefix(text, text_len, text_len) != text_len) {
        return false;
    }
    if (s_msg_pending) {
        return false; // the previous manual message has not gone out yet
    }
    char upper[kMsgDestMax + 1u];
    for (size_t i = 0u; i < dest_len; ++i) {
        const char c = dest[i];
        upper[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    upper[dest_len] = '\0';
    snprintf(s_msg_dest, sizeof(s_msg_dest), "%s", upper);
    copyUtf8Text(s_msg_text, sizeof(s_msg_text), text, text_len);
    s_msg_pending = true;
    return true;
}

extern "C" bool APRS_SERVICE_GetOwnPosition(double *lat, double *lon, double *alt_m)
{
    double la = 0.0, lo = 0.0, al = 0.0;
    const bool fix = ownPosition(&la, &lo, &al);
    if (lat != nullptr) *lat = la;
    if (lon != nullptr) *lon = lo;
    if (alt_m != nullptr) *alt_m = al;
    return fix;
}

extern "C" bool APRS_SERVICE_IsNetConnected(void)
{
    return s_is_socket >= 0 && s_is_logged_in;
}

extern "C" bool APRS_SERVICE_IsNetVerified(void)
{
    return s_is_socket >= 0 && s_is_logged_in && s_is_verified;
}

extern "C" bool APRS_SERVICE_SendRawLine(const char *line)
{
    if (line == nullptr || s_is_socket < 0 || !s_is_logged_in || s_raw_pending) {
        return false;
    }
    const size_t len = strlen(line);
    if (len == 0u || len > kRawLineMax) {
        return false;
    }
    memcpy(s_raw_line, line, len + 1u);
    s_raw_pending = true;
    return true;
}

extern "C" uint32_t APRS_SERVICE_GetRxCount(void) { return s_rx_count; }
extern "C" uint32_t APRS_SERVICE_GetTxCount(void) { return s_tx_count; }

extern "C" bool APRS_SERVICE_GpsHasFix(void)
{
    // Lightweight check: avoids allocating the full AprsGpsInfo (~450 B) on the
    // caller's stack. The nrl_main_loop task has only 6 KB and the display
    // refresh chain is already deep; the old path (GetGpsInfo into a local
    // AprsGpsInfo) overflowed it and caused a CPU lockup.
    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    if (!serial.uart2_enabled) return false;

    bool fix = false;
    uint32_t last_nmea = 0u;
    uint32_t last_fix = 0u;
    portENTER_CRITICAL(&s_gps_mux);
    fix = s_gps_fix;
    last_nmea = s_gps_last_nmea_ms;
    last_fix = s_gps_last_fix_ms;
    portEXIT_CRITICAL(&s_gps_mux);

    const uint32_t now = nowMs();
    const bool connected = last_nmea != 0u && (now - last_nmea) < kGpsDataFreshMs;
    return connected && fix && last_fix != 0u && (now - last_fix) < kGpsFixFreshMs;
}

extern "C" void APRS_SERVICE_GetGpsInfo(AprsGpsInfo *out)
{
    if (out == nullptr) return;

    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);

    AprsGpsInfo snapshot{};
    const uint32_t now = nowMs();
    uint32_t last_nmea_ms = 0u;
    uint32_t last_fix_ms = 0u;
    uint32_t last_rmc_ms = 0u;
    uint32_t last_gga_ms = 0u;
    uint32_t last_gsv_ms = 0u;
    portENTER_CRITICAL(&s_gps_mux);
    snapshot.has_fix = s_gps_fix;
    snapshot.fix_quality = s_gps_fix_quality;
    snapshot.satellites = s_gps_satellites;
    snapshot.latitude = s_gps_lat;
    snapshot.longitude = s_gps_lon;
    snapshot.altitude_m = s_gps_alt_m;
    snapshot.speed_kmh = s_gps_speed_kmh;
    snapshot.hdop = s_gps_hdop;
    snapshot.course_deg = s_gps_course;
    snapshot.course_valid = s_gps_course_valid;
    last_nmea_ms = s_gps_last_nmea_ms;
    last_fix_ms = s_gps_last_fix_ms;
    last_rmc_ms = s_gps_last_rmc_ms;
    last_gga_ms = s_gps_last_gga_ms;
    last_gsv_ms = s_gps_last_gsv_ms;
    for (size_t i = 0; i < s_gps_visible_count &&
                       snapshot.satellite_detail_count < APRS_GPS_SATELLITE_MAX; ++i) {
        if ((now - s_gps_visible_seen_ms[i]) < kGpsDataFreshMs) {
            snapshot.satellite_details[snapshot.satellite_detail_count++] =
                s_gps_visible[i];
        }
    }
    portEXIT_CRITICAL(&s_gps_mux);

    snapshot.uart_enabled = serial.uart2_enabled;
    snapshot.age_ms = last_nmea_ms != 0u ? now - last_nmea_ms : UINT32_MAX;
    snapshot.connected = snapshot.uart_enabled && last_nmea_ms != 0u &&
                         snapshot.age_ms < kGpsDataFreshMs;
    snapshot.has_fix = snapshot.connected && snapshot.has_fix &&
                       last_fix_ms != 0u && (now - last_fix_ms) < kGpsFixFreshMs;

    const bool rmc_fresh = last_rmc_ms != 0u &&
                           (now - last_rmc_ms) < kGpsDataFreshMs;
    const bool gga_fresh = last_gga_ms != 0u &&
                           (now - last_gga_ms) < kGpsDataFreshMs;
    const bool gsv_fresh = last_gsv_ms != 0u &&
                           (now - last_gsv_ms) < kGpsDataFreshMs;
    snapshot.gsv_age_ms = last_gsv_ms != 0u ? now - last_gsv_ms : UINT32_MAX;
    snapshot.visible_satellites = gsv_fresh ? snapshot.satellite_detail_count : -1;
    if (!gsv_fresh) snapshot.satellite_detail_count = 0u;
    if (!snapshot.has_fix) {
        snapshot.latitude = NAN;
        snapshot.longitude = NAN;
    }
    if (!snapshot.has_fix || !rmc_fresh) {
        snapshot.speed_kmh = NAN;
        snapshot.course_deg = 0u;
        snapshot.course_valid = false;
    }
    if (!snapshot.connected || !gga_fresh) {
        snapshot.fix_quality = 0u;
        snapshot.satellites = -1;
        snapshot.hdop = NAN;
        snapshot.altitude_m = NAN;
    } else if (!snapshot.has_fix) {
        snapshot.altitude_m = NAN;
    }
    *out = snapshot;
}

extern "C" size_t APRS_SERVICE_GetStations(AprsStationInfo *out, size_t max_count)
{
    if (out == nullptr || max_count == 0 || s_stations == nullptr) {
        return 0;
    }
    const uint32_t now = nowMs();
    lockStations();
    // collect used slots, most recent first (selection by repeated max-scan;
    // list is small)
    size_t count = 0;
    bool taken[kStationCount] = {false};
    while (count < max_count) {
        int best = -1;
        for (size_t i = 0; i < kStationCount; ++i) {
            if (!s_stations[i].used || taken[i]) {
                continue;
            }
            if (best < 0 ||
                (int32_t)(s_stations[i].last_heard_ms - s_stations[best].last_heard_ms) > 0) {
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }
        taken[best] = true;
        const StationRec &rec = s_stations[best];
        AprsStationInfo &info = out[count++];
        memcpy(info.name, rec.name, sizeof(info.name));
        memcpy(info.symbol, rec.symbol, sizeof(info.symbol));
        info.lat = rec.lat;
        info.lon = rec.lon;
        info.altitude_m = rec.altitude_m;
        info.speed_kmh = rec.speed_kmh;
        info.course_deg = rec.course_deg;
        info.derived_speed_kmh = rec.derived_speed_kmh;
        info.distance_km = rec.distance_km;
        info.bearing_deg = rec.bearing_deg;
        info.age_s = (now - rec.last_heard_ms) / 1000u;
        info.pkt_count = rec.pkt_count;
        info.via_rf = rec.via_rf;
        memcpy(info.comment, rec.comment, sizeof(info.comment));
    }
    unlockStations();
    return count;
}

extern "C" uint32_t APRS_SERVICE_GetStationRevision(void)
{
    return s_station_revision;
}

extern "C" uint32_t APRS_SERVICE_GetLastPacket(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    lockStations();
    strncpy(out, s_last_packet, out_size - 1);
    out[out_size - 1] = '\0';
    const uint32_t seq = s_last_packet_seq;
    unlockStations();
    return seq;
}

extern "C" uint32_t APRS_SERVICE_GetLastSummary(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    lockStations();
    strncpy(out, s_last_packet_summary, out_size - 1);
    out[out_size - 1] = '\0';
    const uint32_t seq = s_last_packet_seq;
    unlockStations();
    return seq;
}
