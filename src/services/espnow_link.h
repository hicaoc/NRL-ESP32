#ifndef SRC_SERVICES_ESPNOW_LINK_H
#define SRC_SERVICES_ESPNOW_LINK_H

// ESP-NOW voice link (docs/architecture.md 功能7): off-grid short-range
// intercom between NRL-ESP32 devices, no AP or server needed. Voice frames
// are G.711 A-law over broadcast ESP-NOW (20 ms / 160 bytes per packet,
// same codec as the NRL uplink), tagged with the local callsign.
//
// Routing (audio router): mic -> AUDIO_SINK_ESPNOW when enabled (subject to
// the same squelch gating as the NRL uplink; NRL simulcast keeps working),
// received frames -> AUDIO_SRC_ESPNOW -> speaker.
//
// ESP-NOW shares the WiFi radio; it works alongside the STA connection on
// the same channel. Enable state persists in NVS.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring the link up (RX is always on once WiFi is started) and restore the
// persisted TX enable state. Call after WiFi init; retries in the background
// while WiFi is still coming up.
void ESPNOW_LINK_Init(void);

// Arm/disarm ESP-NOW transmit at runtime; persists. RX stays on regardless --
// incoming intercom voice is always heard. Arming requires WiFi to be started
// (returns false otherwise). On boards without the dedicated touch PTT this
// also re-targets the single PTT/SQL keying source to ESP-NOW (and back).
bool ESPNOW_LINK_SetEnabled(bool enabled);

bool ESPNOW_LINK_IsEnabled(void);

// Independent RX switch (persisted, defaults ON): incoming intercom voice is
// heard whenever this is on, regardless of the TX enable above.
void ESPNOW_LINK_SetRxEnabled(bool enabled);
bool ESPNOW_LINK_IsRxEnabled(void);

// ESP-NOW TX voice codec (persisted): 0 = G.711 (type 1), 1 = Opus 16k (type
// 8). Independent of the NRL uplink codec; RX always auto-detects both.
// Switching to Opus pre-allocates the codecs; on RAM shortfall the switch
// rolls back to G.711 and false is returned (nothing persisted).
bool ESPNOW_LINK_SetTxCodec(uint8_t codec);
uint8_t ESPNOW_LINK_GetTxCodec(void);

// Physical/user PTT target. 0 = normal NRL uplink, 1 = ESP-NOW uplink,
// 2 = FMO uplink. Kept here for compatibility with the existing configuration
// surfaces; all three voice links consult this shared persisted selector.
// Persisted in NVS. RX still works for both links regardless of this mode.
void ESPNOW_LINK_SetPttMode(uint8_t mode);
uint8_t ESPNOW_LINK_GetPttMode(void);

// Dedicated hold-to-talk for the ESP-NOW link (S31 touch UI): while held,
// mic audio broadcasts over ESP-NOW, independent of the NRL PTT. On boards
// without a touch UI the link keys off the radio squelch instead and these
// are unused.
void ESPNOW_LINK_SetPtt(bool held);
bool ESPNOW_LINK_PttActive(void);

// True while ESP-NOW voice frames are actively arriving (UI RX indicator).
bool ESPNOW_LINK_IsReceiving(void);

// Callsign-SSID of the most recent peer heard (empty when none yet).
void ESPNOW_LINK_GetLastPeer(char *out, size_t out_size);

// Codec of the most recently received ESP-NOW voice frame: 0 = G.711 (type 1),
// 1 = Opus (type 8). Only meaningful while ESPNOW_LINK_IsReceiving() is true.
uint8_t ESPNOW_LINK_GetRxCodec(void);

// Pre-allocate the ESP-NOW Opus encoder+decoder (idempotent). Called when the
// voice codec switches to Opus so allocation failures surface at the switch,
// not mid-conversation. Returns false if either instance is unavailable.
bool ESPNOW_LINK_PrewarmOpus(void);

// Rollback helper for a failed Opus switch: frees the TX encoder. Only call
// after the voice codec has been reverted to G.711 -- the encoder is touched
// exclusively by the audio task while the codec is Opus, so closing it here
// cannot race the send path. The decoder is kept (RX is remote-driven).
void ESPNOW_LINK_ReleaseOpusEncoder(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_ESPNOW_LINK_H
