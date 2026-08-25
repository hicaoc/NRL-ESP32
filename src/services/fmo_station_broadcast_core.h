#ifndef SRC_SERVICES_FMO_STATION_BROADCAST_CORE_H
#define SRC_SERVICES_FMO_STATION_BROADCAST_CORE_H

// Pure, host-compilable encoders for the FMO-V4 STATION server broadcast
// (docs/firmware-analysis.md §8.3 in the fmo-sim project). No ESP-IDF or
// libsodium dependency: hashing/signing inputs and outputs are passed as raw
// byte buffers so the unit test in tests/ can exercise the exact wire layout.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Re-encoding input for the user certificate CBOR blob. The blob is the
// deterministic 10-element array
//   ["FMO",4,"userCert",issuerSn,callsign,uid,pubKey(32B),iat,exp,sig(64B)]
// and is what the on-air "CERT:" field carries (b64url) and what
// certBlobHash = SHA-256(blob) is computed over.
typedef struct {
    uint32_t issuer_sn;
    char callsign[16];
    uint32_t uid;
    uint8_t public_key[32];
    uint64_t issued_at;
    uint64_t expires_at;
    uint8_t signature[64];
} FmoStationCertFields;

// Inputs for the 16-element STATION TBS (§8.3, 30/30 real-network verified):
//   ["FMO",4,"STATION",callUpper,ssid,latStr,lonStr,certBlobHash(32B),
//    ccUpper,name(UTF-8),host,port,coverKm,online,peak,time(NULL)/600]
// `lat`/`lon` are the exact ddmm.mmH / dddmm.mmH strings that also appear in
// the APRS position prefix -- they must be byte-identical in both places.
typedef struct {
    char callsign[16];          // uppercased by the builder
    uint32_t ssid;              // SSID of the packet header callsign; 0 = none
    char lat[10];               // e.g. "3952.80N" (see FormatLat sizing note)
    char lon[11];               // e.g. "11931.57E"
    uint8_t cert_blob_hash[32]; // SHA-256 of the raw CERT blob bytes
    char country[3];            // 2-letter code, uppercased by the builder
    const char *name_utf8;      // station name in UTF-8 (the line carries the
                                // same UTF-8 bytes)
    const char *host;
    uint32_t port;
    uint32_t cover_km;
    uint32_t online;
    uint32_t peak;
    uint64_t time_slot;         // time(NULL)/600
} FmoStationTbsParams;

bool FMO_STATION_CORE_BuildCertBlob(const FmoStationCertFields *cert,
                                    uint8_t *out, size_t capacity,
                                    size_t *out_size);
bool FMO_STATION_CORE_BuildTbs(const FmoStationTbsParams *params, uint8_t *out,
                               size_t capacity, size_t *out_size);

// Inputs for the BEACON (personal beacon) TBS: 10-13 elements
//   ["FMO",4,"BEACON",callUpper,ssid,latStr,lonStr,certBlobHash(32B),
//    freqStr("%.4f" text), (heightM, only when >0), (rig UTF-8, only when
//    non-empty), (ant UTF-8, only when non-empty), time(NULL)/600]
// Optional elements are omitted from the array entirely (never sent as empty
// placeholders); the omission rules are independent per element. Layout
// verified against 9/9 real-network BEACON captures (fmo-sim
// .tmp/verify_beacon.py). The on-air RIG:/ANT: fields carry the same UTF-8
// bytes as the TBS.
typedef struct {
    char callsign[16];          // uppercased by the builder
    uint32_t ssid;              // SSID of the packet header callsign; 0 = none
    char lat[10];               // e.g. "3952.80N" (see FormatLat sizing note)
    char lon[11];               // e.g. "11931.57E"
    uint8_t cert_blob_hash[32]; // SHA-256 of the raw CERT blob bytes
    char freq[16];              // "%.4f" text, e.g. "439.1625"
    uint32_t height_m;          // antenna height; 0 = element omitted
    const char *rig_utf8;       // NULL/empty = element omitted
    const char *ant_utf8;       // NULL/empty = element omitted
    uint64_t time_slot;         // time(NULL)/600
} FmoBeaconTbsParams;

bool FMO_STATION_CORE_BuildBeaconTbs(const FmoBeaconTbsParams *params,
                                     uint8_t *out, size_t capacity,
                                     size_t *out_size);

// APRS coordinate strings, byte-compatible with ParseAPRS::deg2lat/deg2lon
// (including their truncation behavior), so TBS and position prefix match.
void FMO_STATION_CORE_FormatLat(double deg, char out[10]);  // "ddmm.mmH" (9 chars when hundredths round up to 100, like ParseAPRS::deg2lat)
void FMO_STATION_CORE_FormatLon(double deg, char out[11]);  // "dddmm.mmH" (10 chars on the same edge)

// Base64url without padding. Returns the encoded length, 0 on small buffer.
size_t FMO_STATION_CORE_Base64UrlEncode(const uint8_t *data, size_t size,
                                        char *out, size_t capacity);

// UTF-8 -> GBK via a linear reverse lookup of the generated GBK->Unicode
// table (broadcasts are rare, so the O(n) scan per character is acceptable).
// Unmapped code points become '?'. Returns the GBK byte count written.
// Retained for reference/legacy tooling: the on-air text fields are UTF-8
// now (the protocol requires UTF-8 and the map server rejects GBK), so the
// broadcast path no longer calls this.
size_t FMO_STATION_CORE_Utf8ToGbk(const char *utf8, uint8_t *out,
                                  size_t capacity);

// GBK-encodable size of a UTF-8 string (ASCII = 1 byte, GBK-mapped = 2).
// Returns false when a code point has no GBK mapping. Retained alongside
// Utf8ToGbk; config validation no longer requires GBK mappability.
bool FMO_STATION_CORE_GbkEncodedSize(const char *utf8, size_t *out_size);

// ---------------------------------------------------------------------------
// Online-count roster (real-network verified against the broker's own
// FMO/SERVER_INFO [1:5] u32 counter): every online device publishes an
// 8-byte heartbeat to FMO/LATE/UID_V1/<uid> about once a minute, so the
// number of distinct uids seen inside a 120 s sliding window equals the
// server-side online count. The roster is capped; when full, the oldest
// entry is evicted. uid 0 is a valid, countable id (slots are tracked by
// count, not by a sentinel).
#define FMO_STATION_ONLINE_WINDOW_S 120u
#define FMO_STATION_ROSTER_CAPACITY 512u

typedef struct {
    uint32_t uid;         // topic-suffix id; 0 counts like any other
    uint32_t last_seen_s; // monotonic seconds of the last heartbeat
} FmoOnlineRosterEntry;

typedef struct {
    FmoOnlineRosterEntry entries[FMO_STATION_ROSTER_CAPACITY];
    uint32_t count;        // valid prefix of entries[]
    uint32_t session_peak; // max count since the last RosterReset
} FmoOnlineRoster;

// Clear the roster and the session peak (call on MQTT disconnect).
void FMO_STATION_CORE_RosterReset(FmoOnlineRoster *roster);
// Record one heartbeat. Returns the online count at now_s.
uint32_t FMO_STATION_CORE_RosterFeed(FmoOnlineRoster *roster, uint32_t uid,
                                     uint32_t now_s);
// Distinct uids seen inside the window ending at now_s (evicts expired).
uint32_t FMO_STATION_CORE_RosterOnline(FmoOnlineRoster *roster,
                                       uint32_t now_s);

// Config online/peak semantics: 0 = automatic value, >0 = manual override.
uint32_t FMO_STATION_CORE_EffectiveCount(uint32_t configured,
                                         uint32_t automatic);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_STATION_BROADCAST_CORE_H
