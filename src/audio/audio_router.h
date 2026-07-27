#ifndef SRC_AUDIO_AUDIO_ROUTER_H
#define SRC_AUDIO_AUDIO_ROUTER_H

// N-source x M-sink PCM16 routing matrix for the voice-domain audio paths
// (docs/architecture.md "AudioRouter"). Sources push mono PCM16 frames tagged
// with their sample rate; the router fans each frame out to every enabled
// (source, sink) route, converting 16k<->8k at the edge when the sink's
// declared rate differs and applying the per-route gain.
//
// Phase 0 scope: replaces the old single frame hook + hardwired
// mic->NRL / NRL->speaker / BT plumbing with routes. Mixing multiple
// simultaneous sources into one sink is NOT done here yet -- each sink
// currently sees interleaved writes from whichever routes are enabled, and
// the NRL voice wiring keeps exactly one source per sink active at a time
// (as the pre-router code did). The hi-fi (44.1k-192k) music path bypasses
// this router entirely; see the architecture doc.
//
// Threading: sink registration happens once during init, before the audio
// tasks start. Route enable flags and gains are single-word volatiles, safe
// to flip at runtime from any task while pushes are in flight.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Well-known endpoint IDs. Future features (media player, beacon, TTS,
// ESP-NOW, ASR, net radio...) append here without touching router internals.
typedef enum {
    AUDIO_SRC_MIC = 0,      // onboard mic: raw 16 kHz, or AFE-processed 8 kHz
    AUDIO_SRC_NRL_DOWNLINK, // NRL network voice, G.711-decoded 8 kHz
    AUDIO_SRC_BT_HFP_MIC,   // Bluetooth headset mic, 8 kHz SCO
    AUDIO_SRC_ESPNOW,       // ESP-NOW peer voice, G.711-decoded 8 kHz
    AUDIO_SRC_AI,           // xiaozhi AI TTS voice, Opus-decoded 16 kHz
    AUDIO_SRC_APRS,         // AFSK-modulated APRS beacon PCM, 8 kHz
    AUDIO_SRC_MDC_NRL,      // MDC1200 tail burst appended to NRL uplink
    AUDIO_SRC_MDC_SPEAKER,  // MDC1200 tail burst appended to speaker playback
    AUDIO_SRC_DTMF_NRL,     // DTMF tail burst appended to NRL uplink
    AUDIO_SRC_DTMF_SPEAKER, // DTMF tail burst appended to speaker playback
    AUDIO_SRC_CW_NRL,       // CW practice/automatic keying to NRL uplink
    AUDIO_SRC_CW_SPEAKER,   // CW sidetone to the local speaker
    AUDIO_SRC_COUNT
} AudioRouterSource_t;

typedef enum {
    AUDIO_SINK_SPEAKER = 0, // onboard DAC playback queue, 8 kHz
    AUDIO_SINK_NRL_UPLINK,  // NRL network uplink (G.711/UDP), 8 kHz
    AUDIO_SINK_BT_HFP,      // Bluetooth headset speaker, 8 kHz SCO
    AUDIO_SINK_ESPNOW,      // ESP-NOW broadcast uplink (G.711), 8 kHz
    AUDIO_SINK_AI,          // xiaozhi AI mic uplink (Opus-encoded), 16 kHz
    AUDIO_SINK_APRS,        // AFSK demodulator mic tap, 16 kHz
    AUDIO_SINK_SIGNALING,   // MDC1200/DTMF decoder tap, 16 kHz
    AUDIO_SINK_COUNT
} AudioRouterSink_t;

// Q8.8 fixed-point route gain; 256 = unity (the default for every route).
#define AUDIO_ROUTER_GAIN_UNITY 256u

// Sink delivery callback. source_id identifies who produced the frame so a
// sink can arbitrate between producers (e.g. the NRL uplink ignores the
// onboard mic while a BT headset supplies the voice).
typedef void (*AudioRouterSinkWrite_t)(uint8_t source_id,
                                       const int16_t *samples,
                                       size_t sample_count,
                                       void *user_data);

// Register a sink and the sample rate it consumes (8000 or 16000 in the
// voice domain). Re-registering an id overwrites it; returns false on a bad
// id or null callback.
bool AudioRouter_RegisterSink(uint8_t sink_id,
                              uint32_t sample_rate_hz,
                              AudioRouterSinkWrite_t write,
                              void *user_data);

// Enable/disable one (source, sink) route. All routes start disabled.
bool AudioRouter_SetRoute(uint8_t source_id, uint8_t sink_id, bool enabled);

// Per-route gain in Q8.8 (256 = unity). Applied after rate conversion.
bool AudioRouter_SetRouteGain(uint8_t source_id, uint8_t sink_id, uint16_t gain_q8);

// Push one mono PCM16 frame from a source. sample_rate_hz declares the rate
// of THIS frame (the mic source legitimately alternates 16k raw / 8k AFE).
// Fans out to every enabled route; frames to unregistered sinks are dropped.
void AudioRouter_PushFrame(uint8_t source_id,
                           uint32_t sample_rate_hz,
                           const int16_t *samples,
                           size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif // SRC_AUDIO_AUDIO_ROUTER_H
