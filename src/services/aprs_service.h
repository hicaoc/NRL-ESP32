#ifndef SRC_SERVICES_APRS_SERVICE_H
#define SRC_SERVICES_APRS_SERVICE_H

// APRS transceiver service (all four boards):
//
//  - Position beacons from a GPS on dedicated UART2 (NMEA RMC/GGA), falling
//    back to a configured default
//    lat/lon while the GPS is absent or has no fix.
//  - Network side: APRS-IS TCP client (configurable server, default
//    asia.aprs2.net:14580). Beacons go up as TNC2 lines; packets received from the
//    server feed the station list. Passcode is derived from the callsign.
//  - RF side: Bell 202 AFSK via the audio router. TX pushes modulated PCM
//    frames (AUDIO_SRC_APRS -> speaker -> radio mic, VOX keying); RX taps the
//    mic (AUDIO_SRC_MIC -> AUDIO_SINK_APRS), demodulates, and forwards decoded
//    frames to APRS-IS (iGate direction, qAR) besides listing them.
//  - Station list lives in PSRAM: callsign-SSID, position, altitude, comment,
//    course/speed as reported, plus distance/bearing from our own position and
//    a speed derived from successive packets of the same station.
//
// Master switch plus independent net/RF-TX/RF-RX switches, all persisted in
// NVS. Configurable from web portal and AT (AT+APRS...).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APRS_COMMENT_MAX_BYTES 219u

// Gateway forwarding directions between the three APRS channels:
// RF (radio mic/speaker AFSK), NRL (network voice AFSK) and APRS-IS (TCP).
// A decoded packet is relayed from its origin channel to every enabled
// destination; AFSK destinations additionally follow the rf_tx/nrl_tx
// transmitter switches (one modulator, its audio fans out over the enabled
// routes).
typedef enum {
    APRS_FWD_RF_TO_IS = 0,  // iGate uplink: radio RX -> APRS-IS (qAR)
    APRS_FWD_IS_TO_RF,      // iGate downlink: APRS-IS -> radio AFSK TX
    APRS_FWD_NRL_TO_IS,     // NRL network RX -> APRS-IS (qAR)
    APRS_FWD_IS_TO_NRL,     // APRS-IS -> AFSK over the NRL voice uplink
    APRS_FWD_RF_TO_NRL,     // radio RX -> AFSK over the NRL voice uplink
    APRS_FWD_NRL_TO_RF,     // NRL network RX -> radio AFSK TX
    APRS_FWD_COUNT
} AprsFwdDir;

typedef struct {
    bool enabled;         // master switch
    bool net_enabled;     // APRS-IS uplink/downlink
    bool rf_tx_enabled;   // AFSK beacon out through speaker/radio
    bool rf_rx_enabled;   // demodulate mic audio
    bool nrl_tx_enabled;  // AFSK audio out through the NRL network uplink
    bool nrl_rx_enabled;  // demodulate NRL network downlink audio
    bool fwd[APRS_FWD_COUNT]; // gateway forwarding switches, see AprsFwdDir
    uint8_t ssid;         // APRS SSID appended to the radio callsign
    char symbol_table;    // e.g. '/'
    char symbol_code;     // e.g. 'I' for the /I TCP/IP symbol
    uint16_t beacon_interval_s;
    bool auto_interval;   // SmartBeaconing-style: shorten the period while the GPS moves
    bool fixed_beacon_without_gps; // allow default-position beacons while GPS has no fresh fix
    int32_t default_lat_e6; // microdegrees, used when GPS has no fix
    int32_t default_lon_e6;
    uint16_t server_port;
    char server_host[65];
    char path[17];        // RF digi path, e.g. "WIDE1-1"
    char comment[APRS_COMMENT_MAX_BYTES + 1u]; // UTF-8 beacon comment
} AprsConfig;

typedef struct {
    char name[10];      // callsign-SSID or object/item name
    char symbol[3];
    float lat;          // degrees, +N
    float lon;          // degrees, +E
    float altitude_m;   // NAN when unknown
    float speed_kmh;    // as reported in the packet, NAN when unknown
    uint16_t course_deg;    // 0 unknown, 360 = north
    float derived_speed_kmh; // from successive positions, NAN when unknown
    float distance_km;  // from our own (GPS or default) position
    uint16_t bearing_deg;
    uint32_t age_s;     // seconds since last heard
    uint32_t pkt_count;
    uint8_t via_rf;     // 1 = heard on RF, 0 = from APRS-IS
    char comment[APRS_COMMENT_MAX_BYTES + 1u];
} AprsStationInfo;

// Live GPS/NMEA snapshot for the device UI. `connected` means UART2 has
// delivered a recognized RMC/GGA sentence recently; it is intentionally
// separate from `has_fix` so the Web page can show a receiver that is online
// but still acquiring satellites.
#define APRS_GPS_SATELLITE_MAX 32u

typedef struct {
    char talker[3];
    uint16_t prn;
    int16_t elevation_deg;
    int16_t azimuth_deg;
    int16_t snr_dbhz;
} AprsGpsSatelliteInfo;

typedef struct {
    bool uart_enabled;
    bool connected;
    bool has_fix;
    uint8_t fix_quality;       // NMEA GGA quality (0=no fix, 1=GPS, 2=DGPS, ...)
    int16_t satellites;        // -1 when unavailable
    int16_t visible_satellites;
    uint8_t satellite_detail_count;
    AprsGpsSatelliteInfo satellite_details[APRS_GPS_SATELLITE_MAX];
    uint32_t gsv_age_ms;
    double latitude;
    double longitude;
    double altitude_m;
    float speed_kmh;
    float hdop;
    uint16_t course_deg;
    bool course_valid;
    uint32_t age_ms;           // age of the latest recognized NMEA sentence
} AprsGpsInfo;

void APRS_SERVICE_Init(void);

// Master switch; persists and starts/stops the whole service.
bool APRS_SERVICE_SetEnabled(bool enabled);
bool APRS_SERVICE_IsEnabled(void);

// Sub-switches (persisted).
bool APRS_SERVICE_SetNetEnabled(bool enabled);
bool APRS_SERVICE_SetRfTxEnabled(bool enabled);
bool APRS_SERVICE_SetRfRxEnabled(bool enabled);
bool APRS_SERVICE_SetNrlTxEnabled(bool enabled);
bool APRS_SERVICE_SetNrlRxEnabled(bool enabled);
// Gateway forwarding switch for one direction (persisted).
bool APRS_SERVICE_SetFwdEnabled(AprsFwdDir dir, bool enabled);

// Configuration (persisted; NULL/out-of-range rejected).
bool APRS_SERVICE_SetServer(const char *host, uint16_t port);
bool APRS_SERVICE_SetSsid(uint8_t ssid);                    // 0..15
bool APRS_SERVICE_SetSymbol(char table, char code);
bool APRS_SERVICE_SetBeaconInterval(uint16_t seconds);      // 10..3600
// Auto interval: with a fresh GPS fix the beacon period scales down with
// speed (floor 30 s at 60+ km/h), a >300 m move beacons early, and a moving
// station that changes course substantially can corner-peg after 10 s. The
// configured interval stays the stationary/no-GPS ceiling.
bool APRS_SERVICE_SetAutoInterval(bool enabled);
bool APRS_SERVICE_SetFixedBeaconWithoutGps(bool enabled);
bool APRS_SERVICE_SetDefaultPosition(double lat, double lon);
// WGS-84 coordinates used for config input. Accepts APRS/NMEA style
// "ddmm.mmmm[N|S]" for latitude and "dddmm.mmmm[E|W]" for longitude
// (hemisphere letter optional, a leading '-' also means S/W), or plain
// decimal degrees when the integer part is too short for ddmm.mmmm
// (e.g. "45.7529"). Parse returns false on malformed or out-of-range
// input; Format always writes the ddmm.mmmm letter form.
bool APRS_SERVICE_ParseAprsCoord(const char *text, bool is_lat, double *deg_out);
void APRS_SERVICE_FormatAprsCoord(double deg, bool is_lat, char *out, size_t out_size);
bool APRS_SERVICE_SetPath(const char *path);
bool APRS_SERVICE_SetComment(const char *comment);
void APRS_SERVICE_GetConfig(AprsConfig *out);

// Queue a beacon immediately (both directions as enabled).
bool APRS_SERVICE_SendBeaconNow(void);

// Queue a manual APRS text message (":DEST     :text{seq" frame). `dest` is
// the addressee callsign (A-Z 0-9 '-', max 9 chars, case-insensitive),
// `text` the message body (UTF-8, max 67 bytes). The frame leaves over
// APRS-IS when net is linked and/or as AFSK on every enabled audio path
// (speaker RF and/or NRL uplink). Returns false on bad input or when the
// service is off; the actual transmission happens on the APRS task.
bool APRS_SERVICE_SendMessage(const char *dest, const char *text);

// Own current position: GPS fix when fresh, else the configured default.
// Returns true when the position came from a live GPS fix.
bool APRS_SERVICE_GetOwnPosition(double *lat, double *lon, double *alt_m);

// Status for UI/web.
bool APRS_SERVICE_IsNetConnected(void);
// Stricter than IsNetConnected (which goes true the moment the login line is
// sent): only true after the server answered "# logresp ... verified". A bad
// passcode leaves the link up (Listen-Only) but never sets this.
bool APRS_SERVICE_IsNetVerified(void);
// Queue one raw pre-built TNC2 line for the APRS-IS uplink (used by the FMO
// STATION broadcast). Single pending slot; returns false when the socket is
// down, a line is still queued, or the line exceeds 1023 bytes. The APRS
// task performs the actual send.
bool APRS_SERVICE_SendRawLine(const char *line);
uint32_t APRS_SERVICE_GetRxCount(void);   // frames decoded from RF
uint32_t APRS_SERVICE_GetTxCount(void);   // beacons sent
bool APRS_SERVICE_GpsHasFix(void);
void APRS_SERVICE_GetGpsInfo(AprsGpsInfo *out);

// Station list snapshot, ordered most-recently-heard first. Returns the
// number of entries copied. Bump-counter lets displays skip refreshes
// (S31 FULL-mode renders are expensive).
size_t APRS_SERVICE_GetStations(AprsStationInfo *out, size_t max_count);
uint32_t APRS_SERVICE_GetStationRevision(void);

// Last raw packet heard (TNC2 text, RF or IS) for the gezipai live ticker.
// Returns the monotonically increasing sequence of that packet (0 = none yet).
uint32_t APRS_SERVICE_GetLastPacket(char *out, size_t out_size);

// Human-readable summary of the last parsed packet for the small display:
// callsign/SSID plus distance, speed and comment for position packets; or
// callsign/SSID plus message/status text for packets without a position.
uint32_t APRS_SERVICE_GetLastSummary(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_APRS_SERVICE_H
