#ifndef SRC_SERVICES_MUSIC_PLAYLIST_H
#define SRC_SERVICES_MUSIC_PLAYLIST_H

// Track list for the music player (docs/architecture.md 3.6): browses the
// music directories of every online mount (/sdcard/music, /usb/music, /smb),
// keeps the current directory's subdirectories and supported tracks in PSRAM
// and drives MUSIC_PlayFile with next/prev/auto-advance.
//
// Browsing is per client: the S31 screen (PLAYLIST_CLIENT_DISPLAY) and the
// web portal (PLAYLIST_CLIENT_WEB) each own an independent async browse
// session (own position, listing and scan revision) served by one persistent
// scan worker task; headless boards keep the synchronous legacy API, which
// operates on PLAYLIST_CLIENT_LEGACY. Playback is global and unique:
// PlayIndex snapshots the client's track listing into a playback queue that
// next/prev/auto-advance walk, decoupled from any later browsing.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers the track-end hook for auto-advance. Call once at startup
// (after MUSIC_Init).
void PLAYLIST_Init(void);

// (Re)scan the current music directory. Returns the number of tracks found.
// Called at startup, when storage mounts change, and when the UI refreshes.
size_t PLAYLIST_Scan(void);

// Browse clients with independent async sessions. The legacy sync API below
// operates on PLAYLIST_CLIENT_LEGACY.
typedef enum {
    PLAYLIST_CLIENT_LEGACY = 0,
    PLAYLIST_CLIENT_DISPLAY,
    PLAYLIST_CLIENT_WEB,
} PlaylistClient;

// Web and display directory browsing use asynchronous scans so a slow or
// dead SMB directory can neither hold the HTTP request open nor freeze the
// LVGL task inside a touch event (a stale NAS costs the 5 s open timeout).
bool PLAYLIST_ClientScanAsync(PlaylistClient client);
bool PLAYLIST_ClientEnterDirAsync(PlaylistClient client, size_t index);
bool PLAYLIST_ClientUpAsync(PlaylistClient client);
bool PLAYLIST_ClientIsScanning(PlaylistClient client);
bool PLAYLIST_ClientLastScanOk(PlaylistClient client);
// Monotonic counter bumped by every completed scan of this client's session.
// UIs rendering rows as index-based events compare it to spot directory-table
// rebuilds triggered by another scan of the same session.
unsigned PLAYLIST_ClientScanRevision(PlaylistClient client);

// Per-client directory browser state. Same semantics as the legacy getters
// below, but on the client's own session.
const char *PLAYLIST_ClientCurrentDir(PlaylistClient client);
bool PLAYLIST_ClientAtRoot(PlaylistClient client);
bool PLAYLIST_ClientInFavorites(PlaylistClient client);
size_t PLAYLIST_ClientDirCount(PlaylistClient client);
const char *PLAYLIST_ClientGetDirName(PlaylistClient client, size_t index);
const char *PLAYLIST_ClientGetDirPath(PlaylistClient client, size_t index);
size_t PLAYLIST_ClientCount(PlaylistClient client);
const char *PLAYLIST_ClientGetPath(PlaylistClient client, size_t index);

// Start playing entry `index` of the client's listing; snapshots that listing
// into the global playback queue for next/prev/auto-advance.
bool PLAYLIST_ClientPlayIndex(PlaylistClient client, size_t index);

// Legacy session (PLAYLIST_CLIENT_LEGACY) browser state, scanned
// synchronously on the caller's task. At the virtual root, DirCount lists the
// available sources (SD music, USB music, SMB share); inside a source it lists
// direct child directories. Track APIs below always refer only to the current
// directory's direct music files.
const char *PLAYLIST_CurrentDir(void);
bool PLAYLIST_AtRoot(void);
size_t PLAYLIST_DirCount(void);
const char *PLAYLIST_GetDirName(size_t index);
const char *PLAYLIST_GetDirPath(size_t index);
bool PLAYLIST_EnterDir(size_t index);
bool PLAYLIST_Up(void);
bool PLAYLIST_InFavorites(void);

size_t PLAYLIST_Count(void);

// Path of entry `index` (NULL when out of range).
const char *PLAYLIST_GetPath(size_t index);

// Index of the playback-queue entry currently playing, or -1 when idle.
int PLAYLIST_CurrentIndex(void);

// Start playing entry `index` of the legacy session's listing; snapshots that
// listing into the playback queue for next/prev.
bool PLAYLIST_PlayIndex(size_t index);

// Relative navigation on the playback queue (wraps around). No-ops on an
// empty queue.
bool PLAYLIST_Next(void);
bool PLAYLIST_Prev(void);

typedef enum {
    PLAYLIST_REPEAT_LIST = 0,
    PLAYLIST_REPEAT_ONE = 1,
} PlaylistRepeatMode;

PlaylistRepeatMode PLAYLIST_GetRepeatMode(void);
void PLAYLIST_SetRepeatMode(PlaylistRepeatMode mode);
PlaylistRepeatMode PLAYLIST_ToggleRepeatMode(void);

enum {
    PLAYLIST_FAV_MAX = 128,
};

size_t PLAYLIST_FavoriteCount(void);
bool PLAYLIST_IsFavorite(const char *path);
bool PLAYLIST_ToggleFavorite(const char *path);

// Auto-advance to the next track when one finishes naturally (default on).
void PLAYLIST_SetAutoAdvance(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_MUSIC_PLAYLIST_H
