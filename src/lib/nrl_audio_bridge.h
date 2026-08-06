#ifndef SRC_LIB_NRL_AUDIO_BRIDGE_H
#define SRC_LIB_NRL_AUDIO_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool NRLAudioBridge_Init(void);
bool NRLAudioBridge_GetRemoteIdentity(char *buffer, size_t buffer_size);

// Fills `callsign` (buffer >= 7 bytes) and `*ssid` with the most recent NRL
// caller seen on the network, for the on-device LCD. Returns true while that
// caller is still active (a packet arrived within the recent voice window),
// false when idle. `callsign` is filled with the last known caller in both
// cases, or an empty string if no caller has ever been heard.
bool NRLAudioBridge_GetRemoteCaller(char *callsign, size_t callsign_size, unsigned *ssid);

// DMR ID carried in bytes 6..8 of the active NRL2 voice packet header,
// decoded as an unsigned 24-bit big-endian value. Returns 0 when the current
// voice stream has no DMR ID or no NRL voice stream is active.
uint32_t NRLAudioBridge_GetRemoteDmrId(void);

// Codec of the most recently decoded downlink voice packet: 0 = G.711,
// 1 = Opus. Only meaningful while NRLAudioBridge_GetRemoteCaller reports an
// active caller; the LCD reads it to show the incoming stream's format.
uint8_t NRLAudioBridge_GetRxCodec(void);

void NRLAudioBridge_ApplyConfig(bool restart_wifi, bool restart_udp);

// Feed externally-captured microphone audio (PCM16 mono 8 kHz) into the network
// uplink, exactly as the onboard mic path does. Used by the Bluetooth HFP link
// so a headset mic's audio is transmitted. Subject to the same PTT gating.
void NRLAudioBridge_FeedExternalMic(const short *pcm8k, size_t sample_count);

// Media uplink for the nanny feature (docs/architecture.md 功能2): streams
// decoded music/beacon audio (PCM16 mono 8 kHz) to the NRL server as a
// deliberate transmission -- no squelch gating, no tail suppression. While
// active (Set...true), captured mic/BT audio is dropped so the G.711
// accumulator carries exactly one producer.
void NRLAudioBridge_SetMediaUplinkActive(bool active);
void NRLAudioBridge_SendMediaUplink(const short *pcm8k, size_t sample_count);

// Send generated narrowband signaling as an immediate G.711 packet stream.
// The input is already PCM16 mono 8 kHz, so this deliberately bypasses the
// 16 kHz voice router/resampler and the captured-voice accumulator.
bool NRLAudioBridge_SendGeneratedPcm8k(const short *pcm8k, size_t sample_count);

// NRL uplink TX voice codec: 0 = G.711 A-law 8 kHz (NRL packet type 1,
// default), 1 = Opus 16 kHz wideband (packet type 8, 20 ms frames, VOIP/VBR).
// RX accepts both regardless. Persisted in NVS. Independent of the ESP-NOW
// intercom codec (ESPNOW_LINK_SetTxCodec). Switching to Opus pre-allocates the
// NRL encoder/decoder up front so failures surface at the switch instead of
// mid-conversation; on allocation failure the switch rolls back to G.711,
// nothing is persisted, and false is returned.
bool NRLAudioBridge_SetVoiceCodec(uint8_t codec);
uint8_t NRLAudioBridge_GetVoiceCodec(void);

// Generic typed NRL packet TX (video call uses packet type 13). The payload
// is wrapped in the standard 48-byte NRL header (callsign etc.) and sent to
// the configured server. Thread-safe (same UDP mutex as voice).
bool NRLAudioBridge_SendTyped(uint8_t packet_type, const uint8_t *payload, size_t payload_size);

// RX hook for NRL packet type 13 (video fragments). The callback runs in
// the bridge task; it must copy the payload and return quickly. The sender
// callsign of the current packet is available via GetRemoteIdentity.
typedef void (*NrlVideoRxHandler_t)(const uint8_t *payload, size_t payload_size);
void NRLAudioBridge_SetVideoRxHandler(NrlVideoRxHandler_t handler);

// Poll the USB debug serial for AT commands typed by the user. Lines are
// terminated by CR/LF; e.g. "AT" lists commands, "AT+WIFI_SSID=MyNet" sets it.
void NRLAudioBridge_PollSerialConsole(void);

#ifdef __cplusplus
}
#endif

#endif
