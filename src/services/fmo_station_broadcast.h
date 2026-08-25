#ifndef SRC_SERVICES_FMO_STATION_BROADCAST_H
#define SRC_SERVICES_FMO_STATION_BROADCAST_H

// FMO-V4 STATION server broadcast (fmo-sim docs/firmware-analysis.md §8.3):
// when this device runs its own FMO server (its certificate callsign matches
// the configured server and the SAS login succeeded with role "super"), it
// may advertise the server on APRS-IS with a signed
//   <call>>APFMO4,TCPIP*:=<lat>F<lon>EiFMO-V4,STATION,CERT:<b64url>,...
// packet every 5/10/60 minutes.
//
// Send gates (all required, re-checked before every packet):
//   1. the FMO MQTT link is connected;
//   2. the role the link actually logged in with is "super"
//      (the CONNECTED-time snapshot, not a re-derived guess);
//   3. the server callsign equals this device's certificate callsign;
//   4. APRS-IS answered the login with "# logresp ... verified";
//   5. wall clock is sane (SNTP synced), checked via a 2023 time floor.
// A 60 s minimum rate limit applies on top of the configured period.
//
// Not implemented (by design): the FMO/REGU/SERVER_REMOTE_CONTROL remote
// shutdown channel from the reference firmware. There is no remote kill
// switch here; broadcast stops only via config, gate loss, or cert expiry.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Broadcast period selector, values aligned with the reference firmware's
// setIntervalByMode: 0 = off, 2 = 5 min, 3 = 10 min (default), 4 = 60 min.
#define FMO_STATION_BCAST_MODE_OFF 0u
#define FMO_STATION_BCAST_MODE_5MIN 2u
#define FMO_STATION_BCAST_MODE_10MIN 3u
#define FMO_STATION_BCAST_MODE_60MIN 4u

#define FMO_STATION_BCAST_NAME_MAX 96u // UTF-8 bytes: the reference firmware
                                       // allows 32 *characters* (32 CJK chars
                                       // = 96 UTF-8 bytes on air)
#define FMO_STATION_BCAST_HOST_MAX 63u

typedef struct {
    bool enabled;
    uint8_t mode;                    // FMO_STATION_BCAST_MODE_*
    char country[3];                 // 2-letter code, e.g. "CN" (manual; no GeoIP)
    char name[FMO_STATION_BCAST_NAME_MAX + 1u]; // UTF-8 station name
    char host[FMO_STATION_BCAST_HOST_MAX + 1u]; // advertised host (manual)
    uint16_t port;                   // advertised port
    uint32_t cover_km;               // coverage radius F<km>KM
    uint32_t online;                 // U<online>/<peak>: 0 = automatic (default,
    uint32_t peak;                   // counted locally), >0 = manual override
    uint8_t ssid;                    // APRS SSID of the broadcast callsign (0-15,
                                     // 0 = bare callsign). The reference firmware
                                     // follows the SSID of its APRS-IS login
                                     // callsign (e.g. BD4VKI-15); the packet
                                     // header and the signed TBS always carry
                                     // the same value.
} FmoStationBroadcastConfig;

typedef struct {
    bool configured;         // config complete enough to broadcast
    bool gated;              // a gate currently blocks transmission
    uint32_t tx_count;       // packets sent since boot
    uint32_t last_sent_epoch;   // time(NULL) of last packet, 0 = none
    int last_reject;         // FMO_STATION_BCAST_REJECT_* of last skip
} FmoStationBroadcastStatus;

// last_reject values (0 = sent / never tried).
enum {
    FMO_STATION_BCAST_REJECT_NONE = 0,
    FMO_STATION_BCAST_REJECT_NOT_DUE = 1,
    FMO_STATION_BCAST_REJECT_RATE_LIMIT = 2,
    FMO_STATION_BCAST_REJECT_NOT_SUPER = 3,     // gates 1-3
    FMO_STATION_BCAST_REJECT_NOT_VERIFIED = 4,  // gate 4 (APRS-IS logresp)
    FMO_STATION_BCAST_REJECT_NO_TIME = 5,       // gate 5 (clock not synced)
    FMO_STATION_BCAST_REJECT_NO_POSITION = 6,
    FMO_STATION_BCAST_REJECT_CERT = 7,
    FMO_STATION_BCAST_REJECT_CONFIG = 8,
    FMO_STATION_BCAST_REJECT_SEND = 9,
};

bool FMO_STATION_BCAST_Init(void);

// ---------------------------------------------------------------------------
// FMO-V4 BEACON personal beacon (docs/firmware-analysis.md §8.6 in the
// fmo-sim project): unlike the STATION broadcast this is NOT gated on the
// own-server/super role -- it only needs an APRS-IS "verified" login, a ready
// identity certificate and a configured frequency. Fixed 10-minute period
// (independent timer inside the same fmo_bcast task) plus a 60 s hard rate
// limit. Wire frame (<=512 chars, longer frames are dropped):
//   CALL[-SSID]>APFMO4,TCPIP*:=<lat>F<lon>EiFMO-V4,BEACON,CERT:<b64url>,
//   FREQ:%.4f[,HEIGHT:%u][,RIG:<UTF-8>][,ANT:<UTF-8>],SIG:<b64url>
// Follow-ups (each queued once its trigger frame was accepted):
//   APFMO2: after a BEACON, when aprs_msg is set -- unsigned UTF-8 free text
//     CALL[-SSID]>APFMO2,TCPIP*:><UTF-8 text>
//   APFMO1: after a STATION broadcast, when notice is set -- login notice
//     CALL[-SSID]>APFMO1,TCPIP*:><nameUTF8>,正常,在线/峰值:<online>/<peak>[,<noticeUTF8>]
//     using the STATION broadcast's effective online/peak values.
// qso_msg is persisted but never transmitted (传输机制待研究).

// UTF-8 byte limits (CJK chars take 3 UTF-8 bytes on air):
#define FMO_BEACON_RIG_ANT_MAX 48u  // rig/ant: 16 characters
#define FMO_BEACON_MSG_MAX 192u     // aprs_msg: 64 characters
#define FMO_BEACON_NOTICE_MAX 384u  // notice/qso_msg: 128 characters

typedef struct {
    bool enabled;
    uint8_t ssid;      // APRS SSID of the beacon callsign (0-15, 0 = bare)
    uint32_t freq_x10000; // beacon frequency, MHz * 10000 (wire text "%.4f");
                          // valid range 20-500 MHz; 0 gates the beacon off
    uint16_t height_m; // antenna height; 0 = HEIGHT element omitted
    char rig[FMO_BEACON_RIG_ANT_MAX + 1u];    // UTF-8, <=16 chars (UTF-8 on air)
    char ant[FMO_BEACON_RIG_ANT_MAX + 1u];    // UTF-8, <=16 chars (UTF-8 on air)
    char aprs_msg[FMO_BEACON_MSG_MAX + 1u];   // UTF-8 APFMO2 text, <=64 chars
    char notice[FMO_BEACON_NOTICE_MAX + 1u];  // UTF-8 APFMO1 notice, <=128 chars
    char qso_msg[FMO_BEACON_NOTICE_MAX + 1u]; // UTF-8, stored only, never sent
} FmoBeaconConfig;

typedef struct {
    bool gated;             // a gate currently blocks transmission
    uint32_t tx_count;      // beacons sent since boot
    uint32_t last_sent_epoch;  // time(NULL) of last beacon, 0 = none
    int last_reject;        // FMO_BEACON_REJECT_* of last skip
} FmoBeaconStatus;

// last_reject values (0 = sent / never tried).
enum {
    FMO_BEACON_REJECT_NONE = 0,
    FMO_BEACON_REJECT_NOT_DUE = 1,
    FMO_BEACON_REJECT_RATE_LIMIT = 2,
    FMO_BEACON_REJECT_NOT_VERIFIED = 3, // APRS-IS logresp not verified
    FMO_BEACON_REJECT_NO_TIME = 4,      // clock not synced
    FMO_BEACON_REJECT_CERT = 5,
    FMO_BEACON_REJECT_CONFIG = 6,       // e.g. freq == 0
    FMO_BEACON_REJECT_TOO_LONG = 7,     // wire frame would exceed 512 chars
    FMO_BEACON_REJECT_SEND = 8,
};

void FMO_BEACON_GetConfig(FmoBeaconConfig *config);
// Persisted update. Text fields are validated even while disabled (character
// limits, no ASCII comma); enabling additionally requires the
// frequency to be inside 20-500 MHz.
bool FMO_BEACON_SetConfig(const FmoBeaconConfig *config, bool persist);
void FMO_BEACON_GetStatus(FmoBeaconStatus *status);

void FMO_STATION_BCAST_GetConfig(FmoStationBroadcastConfig *config);
// Persisted update. Enabling validates the super-on-own-server gates (1-3)
// up front and refuses when they do not hold right now; an incomplete
// country/host is rejected as well. Empty host/port defaults to the FMO
// server host/port, an empty name defaults to the server/certificate name.
bool FMO_STATION_BCAST_SetConfig(const FmoStationBroadcastConfig *config,
                                 bool persist);

void FMO_STATION_BCAST_GetStatus(FmoStationBroadcastStatus *status);

// Online-count auto-statistics. Every online device heartbeats to
// FMO/LATE/UID_V1/<uid> about once a minute; fmo_service feeds the uid from
// each received heartbeat topic here. The automatic online count is the
// number of distinct uids seen inside a 120 s sliding window; the automatic
// peak is the running maximum, persisted to NVS (throttled) so it survives
// reboots. A configured online/peak of 0 selects these automatic values.
void FMO_STATION_BCAST_FeedHeartbeat(uint32_t uid);
// Clear the online roster on MQTT disconnect (the peak is kept).
void FMO_STATION_BCAST_RosterReset(void);
void FMO_STATION_BCAST_GetAutoCounts(uint32_t *online, uint32_t *peak);

// True while gates 1-3 hold (MQTT connected, actual role "super", server
// callsign == certificate callsign). Used by web/AT for up-front validation.
bool FMO_STATION_BCAST_GatesOk(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_STATION_BROADCAST_H
