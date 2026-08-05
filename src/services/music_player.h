#ifndef SRC_SERVICES_MUSIC_PLAYER_H
#define SRC_SERVICES_MUSIC_PLAYER_H

// Music player service (docs/architecture.md Phase 1). Plays audio files
// from mounted storage (/sdcard, /usb) through the hi-fi speaker path at
// their native sample rate. Voice interrupts music: when NRL downlink
// playback starts, the current track is stopped and the voice path restored
// (interrupt policy; ducking/mixing come later).
//
// Currently WAV/PCM 16-bit only; MP3/FLAC/APE decoders arrive as plugins.
// Local playback uses the board codec's native-rate Hi-Fi backend (ES8389 or
// ES8311); network uplink does not acquire a local codec.

#include <stdbool.h>
#include <stddef.h>

#include "media/media_metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

// Playback target (nanny feature, docs/architecture.md 功能2): local
// speaker, the NRL network uplink (resampled to 8 kHz G.711), or both
// simultaneously (one decode, two consumers). Applies from the next track.
typedef enum {
    MUSIC_TARGET_LOCAL = 0,
    MUSIC_TARGET_NET = 1,
    MUSIC_TARGET_BOTH = 2,
} MusicTarget_t;

// Setting the target persists it in NVS; MUSIC_Init restores it on boot.
void MUSIC_SetTarget(int target);
int MUSIC_GetTarget(void);

// Local output device: onboard speaker (hi-fi path) or the Bluetooth
// headset (A2DP source, 44.1 kHz SBC). Applies from the next track; falls
// back to the speaker when the headset/A2DP is unavailable. Persisted.
typedef enum {
    MUSIC_OUTPUT_SPEAKER = 0,
    MUSIC_OUTPUT_BT = 1,
} MusicOutput_t;

void MUSIC_SetOutput(int output);
int MUSIC_GetOutput(void);

// Net-radio station URL (http:// or https://), persisted in NVS so the
// web portal / LCD UI can re-tune the saved station. Empty when unset.
bool MUSIC_SetRadioUrl(const char *url);
void MUSIC_GetRadioUrl(char *out, size_t out_size);

// Register voice-focus hooks. NRL/ESP-NOW/APRS audio pauses media immediately;
// after all focus owners end, playback resumes following 30 seconds of idle.
// File playback retains its decoder position; live radio reconnects.
// Safe to call on every board.
void MUSIC_Init(void);

// Start playing `path` (e.g. "/sdcard/music/song.wav"). Stops any current
// track first. Returns false if the file is missing/unsupported or the
// hi-fi path is unavailable.
bool MUSIC_PlayFile(const char *path);

// Request stop; returns immediately, the player task winds down and
// restores the voice path within one buffer duration.
void MUSIC_Stop(void);

bool MUSIC_IsPlaying(void);

// Path of the current (or last) track; empty string when none.
const char *MUSIC_CurrentPath(void);

// Tags/cover of the current track (parsed when playback starts). The
// returned pointer stays valid until the next MUSIC_PlayFile -- except
// cover_data, which MUSIC_ReleaseTrackCover() may free early.
const MediaTrackInfo *MUSIC_GetTrackInfo(void);

// Free the embedded cover bytes (up to 512 KB of PSRAM) once the UI has
// copied them out for async decode; no-op afterwards or when there is none.
void MUSIC_ReleaseTrackCover(void);

// Sample format of the stream currently decoding (rate / bit depth /
// channel count for the UI's format line). Valid from the first decoded
// frame until the track stops; returns false when idle. Out pointers may
// be NULL.
bool MUSIC_GetStreamInfo(uint32_t *sample_rate_hz, uint8_t *bits_per_sample, uint8_t *channels);

// Called when a track finishes decoding to the end (NOT on MUSIC_Stop,
// voice interrupt, or decode error). Fired from the player task after it
// has released the codec, so the callback may start the next track.
typedef void (*MusicTrackEndCb_t)(void);
void MUSIC_SetTrackEndCallback(MusicTrackEndCb_t callback);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_MUSIC_PLAYER_H
