#include "services/music_playlist.h"

#include "services/music_player.h"
#include "services/smb_vfs.h"
#include "services/storage_service.h"
#include "lib/nrl_psram.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <atomic>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "PLAYLIST";

namespace {

constexpr size_t kMaxTracks = 512;
constexpr size_t kMaxDirs = 128;
constexpr size_t kMaxScannedEntries = kMaxTracks + kMaxDirs;
// Matches music_player's limit; UTF-8 Chinese paths (3 bytes per CJK char
// plus artist/album directories) overflow the old 128 quickly.
constexpr size_t kMaxPathLen = 256;
constexpr size_t kMaxNameLen = 96;
constexpr const char *kMusicSubdir = "music";
constexpr size_t kClientCount = 3; // PLAYLIST_CLIENT_LEGACY/DISPLAY/WEB

struct PlaylistDir {
    char path[kMaxPathLen];
    char name[kMaxNameLen];
};

// One independent browse session per client: own position, listing, scan
// revision and async scan state, backed by its own PSRAM tables.
struct PlaylistSession {
    char current_dir[kMaxPathLen]; // empty string means virtual source root
    char (*paths)[kMaxPathLen];
    PlaylistDir *dirs;
    std::atomic_size_t count{0};
    std::atomic_size_t dir_count{0};
    // Bumped at the end of every scan of this session. UIs that turn list
    // rows into index-based events compare against it to detect that the
    // directory tables were rebuilt behind their back.
    std::atomic_uint revision{0};
    volatile bool scanning = false;
    volatile bool queued = false; // queued_dir holds a pending dir switch
    volatile bool entries_visible = true;
    volatile bool last_ok = true;
    std::atomic_bool cancel{false};
    char queued_dir[kMaxPathLen];
};

NRL_PSRAM_BSS static char s_path_storage[kClientCount][kMaxTracks][kMaxPathLen];
NRL_PSRAM_BSS static PlaylistDir s_dir_storage[kClientCount][kMaxDirs];
// Playback queue: snapshot of the track listing a PlayIndex call was made
// from, so next/prev/auto-advance survive any client rescanning afterwards.
NRL_PSRAM_BSS static char s_queue_paths[kMaxTracks][kMaxPathLen];
NRL_PSRAM_BSS static char s_fav_storage[PLAYLIST_FAV_MAX][kMaxPathLen];
static PlaylistSession s_sessions[kClientCount];
static size_t s_queue_count = 0;
static volatile int s_current = -1; // index into s_queue_paths, -1 when idle
static volatile bool s_auto_advance = true;
static char (*s_favs)[kMaxPathLen] = s_fav_storage;
static size_t s_fav_count = 0;
static PlaylistRepeatMode s_repeat_mode = PLAYLIST_REPEAT_LIST;
static portMUX_TYPE s_scan_state_lock = portMUX_INITIALIZER_UNLOCKED;

// Persistent scan worker on a static PSRAM stack, same pattern as
// music_player's player task: it only walks SD/FatFS directories and never
// writes internal flash (NVS/LittleFS). The runtime heap shatters to ~1.5 KB
// largest blocks in steady state (see the BT reserve in nrl_bt_hfp.cpp), so
// the old transient xTaskCreate of a 6 KB stack failed once the device had
// been up a while.
constexpr size_t kScanStackBytes = 6144;
NRL_PSRAM_BSS static StackType_t s_scan_stack[kScanStackBytes / sizeof(StackType_t)];
static StaticTask_t s_scan_tcb;
static TaskHandle_t s_scan_task = nullptr;

constexpr const char *kNvsNamespace = "playlist";
constexpr const char *kFavoritesDir = "@favorites";
constexpr const char *kFavoritesFile = "/sdcard/.nrl_music_favorites.txt";

static bool has_supported_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    return strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0 ||
           strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".m4a") == 0 ||
           strcasecmp(dot, ".aac") == 0;
}

static const char *basename_of(const char *path)
{
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return (slash != nullptr) ? slash + 1 : path;
}

static PlaylistSession *session_of(const PlaylistClient client)
{
    const size_t c = static_cast<size_t>(client);
    return (c < kClientCount) ? &s_sessions[c] : nullptr;
}

static bool add_dir(PlaylistSession *s, const char *path, const char *name)
{
    if (path == nullptr || path[0] == '\0' || s->dir_count >= kMaxDirs) {
        return false;
    }
    const int p = snprintf(s->dirs[s->dir_count].path, kMaxPathLen, "%s", path);
    const int n = snprintf(s->dirs[s->dir_count].name, kMaxNameLen, "%s",
                           (name != nullptr && name[0] != '\0') ? name : basename_of(path));
    if (p <= 0 || n <= 0 || static_cast<size_t>(p) >= kMaxPathLen ||
        static_cast<size_t>(n) >= kMaxNameLen) {
        return false;
    }
    ++s->dir_count;
    return true;
}

static bool add_track(PlaylistSession *s, const char *dir_path, const char *name)
{
    if (dir_path == nullptr || name == nullptr || s->count >= kMaxTracks) {
        return false;
    }
    const int written = snprintf(s->paths[s->count], kMaxPathLen, "%s/%s", dir_path, name);
    if (written <= 0 || static_cast<size_t>(written) >= kMaxPathLen) {
        return false;
    }
    ++s->count;
    return true;
}

static int compare_paths(const void *a, const void *b)
{
    return strcasecmp(static_cast<const char *>(a), static_cast<const char *>(b));
}

static int compare_dirs(const void *a, const void *b)
{
    const PlaylistDir *da = static_cast<const PlaylistDir *>(a);
    const PlaylistDir *db = static_cast<const PlaylistDir *>(b);
    return strcasecmp(da->name, db->name);
}

static bool source_root_path(const char *mount_point, const char *subdir,
                             char *out, const size_t out_size)
{
    if (mount_point == nullptr || mount_point[0] == '\0' || out == nullptr || out_size == 0u) {
        return false;
    }
    int written = 0;
    if (subdir != nullptr && subdir[0] != '\0') {
        written = snprintf(out, out_size, "%s/%s", mount_point, subdir);
    } else {
        written = snprintf(out, out_size, "%s", mount_point);
    }
    return written > 0 && static_cast<size_t>(written) < out_size;
}

static bool is_source_root(const char *path)
{
    char root[kMaxPathLen];
    if (source_root_path(STORAGE_SdMountPoint(), kMusicSubdir, root, sizeof(root)) &&
        strcmp(path, root) == 0) {
        return true;
    }
    if (source_root_path(STORAGE_UsbMountPoint(), kMusicSubdir, root, sizeof(root)) &&
        strcmp(path, root) == 0) {
        return true;
    }
    if (source_root_path(STORAGE_SmbMountPoint(), nullptr, root, sizeof(root)) &&
        strcmp(path, root) == 0) {
        return true;
    }
    return false;
}

static void scan_virtual_root(PlaylistSession *s)
{
    char path[kMaxPathLen];
    if (STORAGE_SdMounted()) {
        add_dir(s, kFavoritesDir, "Favorites");
    }
    if (source_root_path(STORAGE_SmbMountPoint(), nullptr, path, sizeof(path))) {
        add_dir(s, path, "SMB");
    }
    if (source_root_path(STORAGE_SdMountPoint(), kMusicSubdir, path, sizeof(path))) {
        add_dir(s, path, "SD");
    }
    if (source_root_path(STORAGE_UsbMountPoint(), kMusicSubdir, path, sizeof(path))) {
        add_dir(s, path, "USB");
    }
}

static void scan_favorites(PlaylistSession *s, const bool sort)
{
    if (s_favs == nullptr) {
        return;
    }
    for (size_t i = 0; i < s_fav_count && s->count < kMaxTracks; ++i) {
        memcpy(s->paths[s->count], s_favs[i], kMaxPathLen);
        s->paths[s->count][kMaxPathLen - 1u] = '\0';
        ++s->count;
    }
    if (sort && s->count > 1u) {
        qsort(s->paths, s->count, kMaxPathLen, compare_paths);
    }
}

static bool scan_current_dir(PlaylistSession *s, const bool sort)
{
    DIR *dir = opendir(s->current_dir);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "open dir failed: %s", s->current_dir);
        return false;
    }

    struct dirent *entry;
    size_t inspected = 0;
    while (inspected < kMaxScannedEntries && !s->cancel.load() &&
           (entry = readdir(dir)) != nullptr) {
        ++inspected;
        if (entry->d_name[0] == '.') {
            continue; // hidden entries + "."/".."
        }
        if (entry->d_type == DT_DIR) {
            if (s->dir_count < kMaxDirs) {
                char sub_path[kMaxPathLen];
                const int written = snprintf(sub_path, sizeof(sub_path), "%s/%s",
                                             s->current_dir, entry->d_name);
                if (written > 0 && static_cast<size_t>(written) < sizeof(sub_path)) {
                    add_dir(s, sub_path, entry->d_name);
                }
            }
            continue;
        }
        if (s->count < kMaxTracks && has_supported_extension(entry->d_name)) {
            add_track(s, s->current_dir, entry->d_name);
        }
    }
    closedir(dir);

    if (sort && s->dir_count > 1u) {
        qsort(s->dirs, s->dir_count, sizeof(PlaylistDir), compare_dirs);
    }
    if (sort && s->count > 1u) {
        qsort(s->paths, s->count, kMaxPathLen, compare_paths);
    }
    return true;
}

static bool set_current_dir(PlaylistSession *s, const char *path)
{
    if (path == nullptr) {
        s->current_dir[0] = '\0';
        return true;
    }
    const int written = snprintf(s->current_dir, sizeof(s->current_dir), "%s", path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(s->current_dir)) {
        return false;
    }
    return true;
}

static void save_favorites_to_sd()
{
    if (!STORAGE_SdMounted() || s_favs == nullptr) {
        return;
    }
    FILE *file = fopen(kFavoritesFile, "w");
    if (file == nullptr) {
        ESP_LOGW(TAG, "favorite file save failed: %s", kFavoritesFile);
        return;
    }
    for (size_t i = 0; i < s_fav_count; ++i) {
        fprintf(file, "%s\n", s_favs[i]);
    }
    fclose(file);
}

static void load_favorites_from_sd()
{
    s_fav_count = 0;
    if (!STORAGE_SdMounted()) {
        return;
    }
    FILE *file = fopen(kFavoritesFile, "r");
    if (file == nullptr) {
        return;
    }
    char line[kMaxPathLen + 8];
    while (fgets(line, sizeof(line), file) != nullptr && s_fav_count < PLAYLIST_FAV_MAX) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        *end = '\0';
        if (line[0] == '\0' || strlen(line) >= kMaxPathLen) {
            continue;
        }
        // Precision bound keeps GCC's -Werror=format-truncation happy: line is
        // sized kMaxPathLen+8, but the guard above already ensures it fits.
        snprintf(s_favs[s_fav_count], kMaxPathLen, "%.*s", static_cast<int>(kMaxPathLen - 1), line);
        ++s_fav_count;
    }
    fclose(file);
}

// Scan body shared by the synchronous legacy API and the scan worker: fills
// the session's tables and bumps its revision. `sort` orders the listing
// (legacy sync scans only; async scans keep scan order).
static size_t scan_session(PlaylistSession *s, const bool sort)
{
    load_favorites_from_sd();

    s->count = 0;
    s->dir_count = 0;
    bool scan_ok = true;
    if (strcmp(s->current_dir, kFavoritesDir) == 0) {
        scan_favorites(s, sort);
    } else if (s->current_dir[0] == '\0') {
        scan_virtual_root(s);
    } else if (!scan_current_dir(s, sort)) {
        // A network directory can fail transiently while SMB reconnects. Keep
        // the requested directory selected so the web/display UI can retry it
        // in place instead of unexpectedly jumping back to the source root.
        s->count = 0;
        s->dir_count = 0;
        scan_ok = false;
    }

    ESP_LOGI(TAG, "%u dirs, %u tracks indexed in %s",
             static_cast<unsigned>(s->dir_count.load()),
             static_cast<unsigned>(s->count.load()),
             s->current_dir[0] ? s->current_dir : "<sources>");
    s->last_ok = scan_ok;
    s->revision.fetch_add(1u);
    return s->count;
}

// One worker drains all sessions with a pending scan request, serially (SMB
// is a single connection anyway).
static void scan_worker_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (size_t c = 0; c < kClientCount; ++c) {
            PlaylistSession *s = &s_sessions[c];
            if (!s->scanning) {
                continue;
            }
            const bool sort = (c == static_cast<size_t>(PLAYLIST_CLIENT_LEGACY));
            while (true) {
                // Entries appear as the scan fills the tables (mirrors the old
                // async scan's entries-visible handling).
                s->entries_visible = true;
                (void)scan_session(s, sort);

                char next[kMaxPathLen] = {};
                bool scan_again = false;
                portENTER_CRITICAL(&s_scan_state_lock);
                if (s->queued) {
                    memcpy(next, s->queued_dir, sizeof(next));
                    s->queued = false;
                    s->entries_visible = false;
                    s->last_ok = true;
                    scan_again = true;
                } else {
                    s->entries_visible = true;
                    s->scanning = false;
                }
                portEXIT_CRITICAL(&s_scan_state_lock);

                if (!scan_again) {
                    break;
                }
                (void)set_current_dir(s, next);
                s->cancel.store(false);
                ESP_LOGI(TAG, "client %u switching directory scan to %s",
                         static_cast<unsigned>(c), next[0] ? next : "<sources>");
            }
        }
    }
}

static bool schedule_scan(const size_t client, const char *path)
{
    if (client >= kClientCount || path == nullptr) {
        return false;
    }
    // Any browse/play intent is the trigger for the lazy SMB mount
    // (STORAGE_SmbAutoStart is a no-op when no share is configured or the
    // supervisor already runs).
    STORAGE_SmbAutoStart();
    PlaylistSession *s = &s_sessions[client];
    char target[kMaxPathLen] = {};
    char previous[kMaxPathLen] = {};
    const int written = snprintf(target, sizeof(target), "%s", path);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(target)) {
        return false;
    }
    portENTER_CRITICAL(&s_scan_state_lock);
    if (s->scanning) {
        memcpy(s->queued_dir, target, sizeof(s->queued_dir));
        s->queued = true;
        s->last_ok = true;
        portEXIT_CRITICAL(&s_scan_state_lock);

        // readdir() may currently be waiting for an SMB QUERY_DIRECTORY reply.
        // Wake that wait promptly; the worker opens the most recently queued
        // directory after discarding the partial result. Cancellation is local
        // control flow: the SMB session survives it (see smb_vfs.cpp).
        s->cancel.store(true);
        SMB_VFS_CancelDirectoryScan();
        ESP_LOGI(TAG, "client %u directory switch queued: %s",
                 static_cast<unsigned>(client), target[0] ? target : "<sources>");
        return true;
    }
    memcpy(previous, s->current_dir, sizeof(previous));
    memcpy(s->current_dir, target, sizeof(s->current_dir));
    s->scanning = true;
    s->entries_visible = false;
    s->last_ok = true;
    s->queued = false;
    s->cancel.store(false);
    portEXIT_CRITICAL(&s_scan_state_lock);

    if (s_scan_task == nullptr) {
        portENTER_CRITICAL(&s_scan_state_lock);
        s->scanning = false;
        s->entries_visible = true;
        s->last_ok = false;
        memcpy(s->current_dir, previous, sizeof(s->current_dir));
        portEXIT_CRITICAL(&s_scan_state_lock);
        ESP_LOGE(TAG, "scan worker unavailable (init failed)");
        return false;
    }
    xTaskNotifyGive(s_scan_task);
    return true;
}

static void save_settings()
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "settings persist failed");
        return;
    }
    (void)nvs_set_u8(nvs, "repeat", static_cast<uint8_t>(s_repeat_mode));
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_settings()
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t repeat = 0;
    if (nvs_get_u8(nvs, "repeat", &repeat) == ESP_OK && repeat <= PLAYLIST_REPEAT_ONE) {
        s_repeat_mode = static_cast<PlaylistRepeatMode>(repeat);
    }
    nvs_close(nvs);
    load_favorites_from_sd();
    ESP_LOGI(TAG, "%u favorite tracks loaded from SD", static_cast<unsigned>(s_fav_count));
}

static bool queue_play_index(const size_t index)
{
    if (index >= s_queue_count) {
        return false;
    }
    if (!MUSIC_PlayFile(s_queue_paths[index])) {
        return false;
    }
    s_current = static_cast<int>(index);
    return true;
}

static bool snapshot_queue(const PlaylistSession *session)
{
    if (session == nullptr || session->scanning || !session->entries_visible ||
        session->paths == nullptr) {
        return false;
    }
    size_t count = session->count.load();
    if (count > kMaxTracks) {
        count = kMaxTracks;
    }
    if (count == 0u) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        memcpy(s_queue_paths[i], session->paths[i], kMaxPathLen);
    }
    s_queue_count = count;
    s_current = -1;
    return true;
}

static void on_track_end(void)
{
    if (!s_auto_advance) {
        return;
    }
    if (s_repeat_mode == PLAYLIST_REPEAT_ONE && s_current >= 0) {
        (void)queue_play_index(static_cast<size_t>(s_current));
    } else {
        (void)PLAYLIST_Next();
    }
}

} // namespace

extern "C" void PLAYLIST_Init(void)
{
    for (size_t i = 0; i < kClientCount; ++i) {
        s_sessions[i].paths = s_path_storage[i];
        s_sessions[i].dirs = s_dir_storage[i];
    }
    load_settings();
    MUSIC_SetTrackEndCallback(on_track_end);
    s_scan_task = xTaskCreateStaticPinnedToCore(scan_worker_task, "playlist_scan",
                                                kScanStackBytes, nullptr, 3,
                                                s_scan_stack, &s_scan_tcb, 0);
    if (s_scan_task == nullptr) {
        ESP_LOGE(TAG, "scan worker create failed at init");
    }
}

extern "C" size_t PLAYLIST_Scan(void)
{
    // Legacy session scans run synchronously on the caller's task; the SMB
    // VFS tolerates a second open directory alongside the scan worker.
    return scan_session(&s_sessions[PLAYLIST_CLIENT_LEGACY], true);
}

extern "C" bool PLAYLIST_ClientScanAsync(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    if (s == nullptr) {
        return false;
    }
    return schedule_scan(static_cast<size_t>(client), s->current_dir);
}

extern "C" bool PLAYLIST_ClientEnterDirAsync(const PlaylistClient client, const size_t index)
{
    const char *path = PLAYLIST_ClientGetDirPath(client, index);
    if (path == nullptr ||
        (strcmp(path, kFavoritesDir) == 0 && !STORAGE_SdMounted())) {
        return false;
    }
    char target[kMaxPathLen];
    const int written = snprintf(target, sizeof(target), "%s", path);
    return written >= 0 && static_cast<size_t>(written) < sizeof(target) &&
           schedule_scan(static_cast<size_t>(client), target);
}

extern "C" bool PLAYLIST_ClientUpAsync(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    if (s == nullptr || s->current_dir[0] == '\0') {
        return false;
    }
    char target[kMaxPathLen];
    snprintf(target, sizeof(target), "%s", s->current_dir);
    if (strcmp(target, kFavoritesDir) == 0 || is_source_root(target)) {
        target[0] = '\0';
    } else {
        char *slash = strrchr(target, '/');
        if (slash == nullptr || slash == target) {
            target[0] = '\0';
        } else {
            *slash = '\0';
        }
    }
    return schedule_scan(static_cast<size_t>(client), target);
}

extern "C" bool PLAYLIST_ClientIsScanning(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return s != nullptr && s->scanning;
}

extern "C" bool PLAYLIST_ClientLastScanOk(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return s != nullptr && s->last_ok;
}

extern "C" unsigned PLAYLIST_ClientScanRevision(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return (s != nullptr) ? s->revision.load() : 0u;
}

extern "C" const char *PLAYLIST_ClientCurrentDir(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return (s != nullptr) ? s->current_dir : "";
}

extern "C" bool PLAYLIST_ClientAtRoot(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return s == nullptr || s->current_dir[0] == '\0';
}

extern "C" bool PLAYLIST_ClientInFavorites(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return s != nullptr && strcmp(s->current_dir, kFavoritesDir) == 0;
}

extern "C" size_t PLAYLIST_ClientDirCount(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return (s != nullptr && s->entries_visible) ? s->dir_count.load() : 0u;
}

extern "C" const char *PLAYLIST_ClientGetDirName(const PlaylistClient client, const size_t index)
{
    PlaylistSession *s = session_of(client);
    if (s == nullptr || !s->entries_visible || s->dirs == nullptr || index >= s->dir_count) {
        return nullptr;
    }
    return s->dirs[index].name;
}

extern "C" const char *PLAYLIST_ClientGetDirPath(const PlaylistClient client, const size_t index)
{
    PlaylistSession *s = session_of(client);
    if (s == nullptr || !s->entries_visible || s->dirs == nullptr || index >= s->dir_count) {
        return nullptr;
    }
    return s->dirs[index].path;
}

extern "C" size_t PLAYLIST_ClientCount(const PlaylistClient client)
{
    PlaylistSession *s = session_of(client);
    return (s != nullptr && s->entries_visible) ? s->count.load() : 0u;
}

extern "C" const char *PLAYLIST_ClientGetPath(const PlaylistClient client, const size_t index)
{
    PlaylistSession *s = session_of(client);
    if (s == nullptr || !s->entries_visible || s->paths == nullptr || index >= s->count) {
        return nullptr;
    }
    return s->paths[index];
}

extern "C" bool PLAYLIST_ClientPlayIndex(const PlaylistClient client, const size_t index)
{
    PlaylistSession *s = session_of(client);
    if (s == nullptr || s->scanning || !s->entries_visible ||
        s->paths == nullptr || index >= s->count) {
        return false;
    }
    // Snapshot the client's whole track listing into the playback queue so
    // next/prev/auto-advance stay stable however any client browses later.
    if (!snapshot_queue(s) || index >= s_queue_count) {
        return false;
    }
    if (!MUSIC_PlayFile(s_queue_paths[index])) {
        return false;
    }
    s_current = static_cast<int>(index);
    return true;
}

extern "C" const char *PLAYLIST_CurrentDir(void)
{
    return s_sessions[PLAYLIST_CLIENT_LEGACY].current_dir;
}

extern "C" bool PLAYLIST_AtRoot(void)
{
    return s_sessions[PLAYLIST_CLIENT_LEGACY].current_dir[0] == '\0';
}

extern "C" bool PLAYLIST_InFavorites(void)
{
    return strcmp(s_sessions[PLAYLIST_CLIENT_LEGACY].current_dir, kFavoritesDir) == 0;
}

extern "C" size_t PLAYLIST_DirCount(void)
{
    return PLAYLIST_ClientDirCount(PLAYLIST_CLIENT_LEGACY);
}

extern "C" const char *PLAYLIST_GetDirName(const size_t index)
{
    return PLAYLIST_ClientGetDirName(PLAYLIST_CLIENT_LEGACY, index);
}

extern "C" const char *PLAYLIST_GetDirPath(const size_t index)
{
    return PLAYLIST_ClientGetDirPath(PLAYLIST_CLIENT_LEGACY, index);
}

extern "C" bool PLAYLIST_EnterDir(const size_t index)
{
    PlaylistSession *s = &s_sessions[PLAYLIST_CLIENT_LEGACY];
    if (s->scanning) {
        return false;
    }
    const char *path = PLAYLIST_GetDirPath(index);
    if (path != nullptr && strcmp(path, kFavoritesDir) == 0 && !STORAGE_SdMounted()) {
        return false;
    }
    if (path == nullptr || !set_current_dir(s, path)) {
        return false;
    }
    (void)PLAYLIST_Scan();
    return true;
}

extern "C" bool PLAYLIST_Up(void)
{
    PlaylistSession *s = &s_sessions[PLAYLIST_CLIENT_LEGACY];
    if (s->scanning || s->current_dir[0] == '\0') {
        return false;
    }
    if (strcmp(s->current_dir, kFavoritesDir) == 0) {
        s->current_dir[0] = '\0';
        (void)PLAYLIST_Scan();
        return true;
    }
    if (is_source_root(s->current_dir)) {
        s->current_dir[0] = '\0';
        (void)PLAYLIST_Scan();
        return true;
    }
    char *slash = strrchr(s->current_dir, '/');
    if (slash == nullptr || slash == s->current_dir) {
        s->current_dir[0] = '\0';
    } else {
        *slash = '\0';
    }
    (void)PLAYLIST_Scan();
    return true;
}

extern "C" size_t PLAYLIST_Count(void)
{
    return PLAYLIST_ClientCount(PLAYLIST_CLIENT_LEGACY);
}

extern "C" const char *PLAYLIST_GetPath(const size_t index)
{
    return PLAYLIST_ClientGetPath(PLAYLIST_CLIENT_LEGACY, index);
}

extern "C" int PLAYLIST_CurrentIndex(void)
{
    return s_current;
}

extern "C" bool PLAYLIST_PlayIndex(const size_t index)
{
    return PLAYLIST_ClientPlayIndex(PLAYLIST_CLIENT_LEGACY, index);
}

extern "C" bool PLAYLIST_Next(void)
{
    if (s_queue_count == 0u &&
        !snapshot_queue(&s_sessions[PLAYLIST_CLIENT_LEGACY])) {
        return false;
    }
    const size_t next = (s_current < 0) ? 0u : (static_cast<size_t>(s_current) + 1u) % s_queue_count;
    return queue_play_index(next);
}

extern "C" bool PLAYLIST_Prev(void)
{
    if (s_queue_count == 0u &&
        !snapshot_queue(&s_sessions[PLAYLIST_CLIENT_LEGACY])) {
        return false;
    }
    const size_t prev = (s_current <= 0) ? (s_queue_count - 1u) : (static_cast<size_t>(s_current) - 1u);
    return queue_play_index(prev);
}

extern "C" PlaylistRepeatMode PLAYLIST_GetRepeatMode(void)
{
    return s_repeat_mode;
}

extern "C" void PLAYLIST_SetRepeatMode(const PlaylistRepeatMode mode)
{
    s_repeat_mode = (mode == PLAYLIST_REPEAT_ONE) ? PLAYLIST_REPEAT_ONE : PLAYLIST_REPEAT_LIST;
    save_settings();
}

extern "C" PlaylistRepeatMode PLAYLIST_ToggleRepeatMode(void)
{
    PLAYLIST_SetRepeatMode(s_repeat_mode == PLAYLIST_REPEAT_ONE
                               ? PLAYLIST_REPEAT_LIST
                               : PLAYLIST_REPEAT_ONE);
    return s_repeat_mode;
}

extern "C" size_t PLAYLIST_FavoriteCount(void)
{
    return s_fav_count;
}

extern "C" bool PLAYLIST_IsFavorite(const char *path)
{
    if (path == nullptr || path[0] == '\0' || s_favs == nullptr) {
        return false;
    }
    for (size_t i = 0; i < s_fav_count; ++i) {
        if (strcmp(s_favs[i], path) == 0) {
            return true;
        }
    }
    return false;
}

extern "C" bool PLAYLIST_ToggleFavorite(const char *path)
{
    if (!STORAGE_SdMounted() || path == nullptr || path[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < s_fav_count; ++i) {
        if (strcmp(s_favs[i], path) == 0) {
            memmove(&s_favs[i], &s_favs[i + 1], (s_fav_count - i - 1u) * kMaxPathLen);
            --s_fav_count;
            save_favorites_to_sd();
            // Rescan-on-favorites stays on the legacy session, as before.
            if (strcmp(s_sessions[PLAYLIST_CLIENT_LEGACY].current_dir, kFavoritesDir) == 0) {
                (void)PLAYLIST_Scan();
            }
            return true;
        }
    }
    if (s_fav_count >= PLAYLIST_FAV_MAX) {
        return false;
    }
    const int written = snprintf(s_favs[s_fav_count], kMaxPathLen, "%s", path);
    if (written <= 0 || static_cast<size_t>(written) >= kMaxPathLen) {
        return false;
    }
    ++s_fav_count;
    save_favorites_to_sd();
    return true;
}

extern "C" void PLAYLIST_SetAutoAdvance(const bool enabled)
{
    s_auto_advance = enabled;
}
