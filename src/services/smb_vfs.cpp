#include "services/smb_vfs.h"
#include "lib/nrl_psram.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>

#include <smb2/smb2.h>
#include <smb2/smb2-errors.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

#include <atomic>
// libsmb2 exposes its raw command API publicly but keeps these two helpers in
// its internal header. Declare only the required symbols here; including the
// complete private header collides with the S31 toolchain's MIN macro.
extern "C" int smb2_decode_fileidfulldirectoryinformation(
    struct smb2_context *, struct smb2_fileidfulldirectoryinformation *,
    struct smb2_iovec *);
extern "C" void smb2_timeout_pdus(struct smb2_context *);

static const char *TAG = "SMBVFS";

namespace {

constexpr int kMaxOpenFiles = 8;
// Directory enumeration on a NAS can legitimately take longer than file
// reads, especially for large music folders. Ten seconds caused libsmb2 to
// abort QUERY_DIRECTORY and made the browser drop the selected folder.
constexpr int kSmbTimeoutSeconds = 30;
// Proactive reconnect after this much idle time; see smb_ensure_connected_locked.
constexpr int kIdleReconnectSeconds = 60;
constexpr size_t kMaxCachedDirEntries = 640;
constexpr size_t kDirectoryCacheSlots = 2;
// A 4 KB reply is accepted reliably by slower NAS implementations and still
// avoids libsmb2's very chatty 512-byte ESP default.
constexpr uint32_t kDirectoryReplyBytes = 4096;
constexpr size_t kMaxDirectoryQueries = 64;
// Large directories can take more than five seconds before the first result.
// Memory remains bounded by kMaxCachedDirEntries, so waiting longer no longer
// risks the TLS/Web heap exhaustion caused by high-level smb2_opendir().
constexpr int kDirectoryCommandTimeoutMs = 15000;
// CREATE (directory open) gets a shorter fuse than QUERY_DIRECTORY: it is a
// tiny metadata exchange, so 5 s of silence already means the connection is
// dead, and the VFS lock is held for the whole wait.
constexpr int kDirectoryOpenTimeoutMs = 5000;
constexpr int kDirectoryScanBudgetMs = 30000;
constexpr size_t kMaxSmbText = 128;

// libsmb2's context is not thread safe; the player task, playlist scan and
// metadata parser all issue calls, so every operation takes this lock.
static SemaphoreHandle_t s_lock = nullptr;
static struct smb2_context *s_smb = nullptr;
static struct smb2fh *s_files[kMaxOpenFiles] = {};
static int s_open_dirs = 0;
static bool s_smb_bad = false;
// Last successful SMB round trip on the VFS context (esp_timer_get_time us).
static int64_t s_last_activity_us = 0;
static std::atomic_bool s_cancel_directory_scan{false};
static std::atomic_bool s_cancel_file_read{false};
static char s_server[kMaxSmbText] = {};
static char s_share[kMaxSmbText] = {};
static char s_user[kMaxSmbText] = {};
static char s_password[kMaxSmbText] = {};

// DIR wrapper handed back through the VFS: newlib requires the first member
// to be the plain DIR the VFS layer fills in.
struct CachedDirEntry {
    char name[sizeof(((struct dirent *)nullptr)->d_name)];
    uint8_t type;
};

struct SmbVfsDir {
    DIR dir;
    CachedDirEntry *entries;
    int cache_slot;
    size_t count;
    size_t index;
    bool truncated;
    bool context_stale;
    // Set when the scan was canceled locally (caller switched directories):
    // control flow, not a connection failure, so the SMB session stays usable.
    bool canceled;
    bool remote_open;
    bool first_query;
    bool finished;
    size_t query_count;
    size_t empty_replies;
    int64_t scan_started_us;
    smb2_file_id file_id;
    char path[256];
    struct dirent entry;
};

NRL_PSRAM_BSS static CachedDirEntry
    s_directory_cache[kDirectoryCacheSlots][kMaxCachedDirEntries];
static bool s_directory_cache_used[kDirectoryCacheSlots] = {};

static int acquire_directory_cache()
{
    for (size_t i = 0; i < kDirectoryCacheSlots; ++i) {
        if (!s_directory_cache_used[i]) {
            s_directory_cache_used[i] = true;
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void release_directory_cache(const int slot)
{
    if (slot >= 0 && static_cast<size_t>(slot) < kDirectoryCacheSlots) {
        s_directory_cache_used[slot] = false;
    }
}

static inline const char *smb_path(const char *vfs_path)
{
    // VFS hands paths relative to the mount point ("" or "/sub/file");
    // libsmb2 wants share-relative paths without the leading slash.
    return (vfs_path != nullptr && vfs_path[0] == '/') ? vfs_path + 1 : vfs_path;
}

class LockGuard {
public:
    LockGuard() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~LockGuard() { xSemaphoreGive(s_lock); }
};

static bool smb_error_is_connection_loss(const char *error)
{
    return error != nullptr &&
           (strstr(error, "smb2_service failed") != nullptr ||
            strstr(error, "SMB2_STATUS_IO_TIMEOUT") != nullptr ||
            strstr(error, "Not Connected") != nullptr ||
            strstr(error, "POLLHUP") != nullptr ||
            strstr(error, "socket error") != nullptr);
}

static bool smb_has_open_handles_locked(void)
{
    if (s_open_dirs > 0) {
        return true;
    }
    for (int i = 0; i < kMaxOpenFiles; ++i) {
        if (s_files[i] != nullptr) {
            return true;
        }
    }
    return false;
}

static void smb_destroy_context_locked(void)
{
    if (s_smb == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxOpenFiles; ++i) {
        s_files[i] = nullptr;
    }
    smb2_destroy_context(s_smb);
    s_smb = nullptr;
}

static void smb_touch_locked(void)
{
    s_last_activity_us = esp_timer_get_time();
}

static bool smb_connect_locked(void)
{
    if (smb_has_open_handles_locked()) {
        ESP_LOGW(TAG, "defer reconnect while SMB handles are open");
        return false;
    }
    smb_destroy_context_locked();
    s_smb_bad = false;

    s_smb = smb2_init_context();
    if (s_smb == nullptr) {
        return false;
    }
    smb2_set_timeout(s_smb, kSmbTimeoutSeconds);
    smb2_set_security_mode(s_smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (s_password[0] != '\0') {
        smb2_set_password(s_smb, s_password);
    }

    const char *login = (s_user[0] != '\0') ? s_user : "guest";
    if (smb2_connect_share(s_smb, s_server, s_share, login) < 0) {
        ESP_LOGE(TAG, "connect //%s/%s failed: %s", s_server, s_share, smb2_get_error(s_smb));
        smb_destroy_context_locked();
        return false;
    }
    smb_touch_locked();
    return true;
}

static bool smb_ensure_connected_locked(void)
{
    if (s_smb != nullptr && !s_smb_bad) {
        // The media stream uses its own SMB connection (smb_stream
        // lane), so this VFS context is completely idle during
        // playback -- and an idle TCP session is reaped by the
        // AP/NAT or the server after a minute or two. The socket
        // still looks healthy locally; the next command would
        // discover the loss only after the full command timeout and
        // the operation would fail. Rebuild the session proactively.
        if (!smb_has_open_handles_locked() &&
            esp_timer_get_time() - s_last_activity_us >=
                static_cast<int64_t>(kIdleReconnectSeconds) * 1000000LL) {
            ESP_LOGI(TAG, "SMB connection idle, reconnecting //%s/%s",
                     s_server, s_share);
            if (!smb_connect_locked()) {
                errno = ENODEV;
                return false;
            }
        }
        return true;
    }
    if (smb_has_open_handles_locked()) {
        errno = EIO;
        return false;
    }
    if (s_server[0] == '\0' || s_share[0] == '\0') {
        errno = ENODEV;
        return false;
    }
    ESP_LOGW(TAG, "SMB connection stale, reconnecting //%s/%s", s_server, s_share);
    if (!smb_connect_locked()) {
        errno = ENODEV;
        return false;
    }
    return true;
}

// Service one async libsmb2 command while holding s_lock. On every abnormal
// exit the context is destroyed before the caller's stack-owned callback
// state goes out of scope. smb2_destroy_context() synchronously cancels the
// queued PDU and invokes its callback, preventing a later use-after-return.
static bool smb_wait_async_locked(volatile size_t *completed,
                                  const size_t expected,
                                  const std::atomic_bool *cancel,
                                  const int timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() +
                                static_cast<int64_t>(timeout_ms) * 1000LL;
    bool aborted = false;
    while (*completed < expected) {
        if (cancel != nullptr && cancel->load()) {
            aborted = true;
            break;
        }
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            aborted = true;
            break;
        }
        struct pollfd pfd = {};
        pfd.fd = smb2_get_fd(s_smb);
        pfd.events = smb2_which_events(s_smb);
        const int remaining_ms = static_cast<int>(remaining_us / 1000LL);
        const int poll_ms = remaining_ms < 100 ? remaining_ms : 100;
        const int polled = poll(&pfd, 1, poll_ms > 0 ? poll_ms : 1);
        if (polled < 0) {
            if (errno == EINTR) {
                continue;
            }
            aborted = true;
            break;
        }
        smb2_timeout_pdus(s_smb);
        if (pfd.revents != 0 && smb2_service(s_smb, pfd.revents) < 0) {
            aborted = true;
            break;
        }
    }
    if (!aborted) {
        smb_touch_locked();
        return true;
    }
    s_smb_bad = true;
    smb_destroy_context_locked();
    return false;
}

struct AsyncReadBatch {
    volatile size_t completed;
};

struct AsyncReadResult {
    AsyncReadBatch *batch;
    int status;
    size_t requested;
};

static void file_read_cb(struct smb2_context *, const int status,
                         void *, void *private_data)
{
    AsyncReadResult *result = static_cast<AsyncReadResult *>(private_data);
    result->status = status;
    const size_t completed = result->batch->completed;
    result->batch->completed = completed + 1u;
}

static int vfs_open(void *, const char *path, const int flags, int)
{
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0) {
        errno = EROFS; // read-only backend
        return -1;
    }
    LockGuard lock;
    if (!smb_ensure_connected_locked()) {
        return -1;
    }
    int fd = -1;
    for (int i = 0; i < kMaxOpenFiles; ++i) {
        if (s_files[i] == nullptr) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        errno = ENFILE;
        return -1;
    }
    struct smb2fh *fh = smb2_open(s_smb, smb_path(path), O_RDONLY);
    if (fh == nullptr) {
        const char *error = smb2_get_error(s_smb);
        if (smb_error_is_connection_loss(error)) {
            ESP_LOGW(TAG, "open %s failed, reconnecting: %s", path, error);
            if (!smb_has_open_handles_locked() && smb_connect_locked()) {
                fh = smb2_open(s_smb, smb_path(path), O_RDONLY);
            } else {
                s_smb_bad = true;
            }
        }
    }
    if (fh == nullptr) {
        ESP_LOGW(TAG, "open %s: %s", path, (s_smb != nullptr) ? smb2_get_error(s_smb) : "not connected");
        errno = ENOENT;
        return -1;
    }
    s_files[fd] = fh;
    s_cancel_file_read.store(false);
    return fd;
}

static ssize_t vfs_read(void *, const int fd, void *dst, const size_t size)
{
    LockGuard lock;
    if (s_smb == nullptr || s_smb_bad || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    // Keep each response inside one TCP segment, but pipeline several explicit
    // preads so a PSRAM refill costs roughly one network round trip rather
    // than one round trip per 1200 bytes.
    constexpr size_t kChunk = 1200;
    constexpr size_t kPipelineDepth = 8;
    uint8_t *out = static_cast<uint8_t *>(dst);
    size_t done = 0;
    uint64_t file_offset = 0;
    if (smb2_lseek(s_smb, s_files[fd], 0, SEEK_CUR, &file_offset) < 0) {
        errno = EIO;
        return -1;
    }
    while (done < size) {
        if (s_cancel_file_read.load()) {
            errno = ECANCELED;
            break;
        }
        AsyncReadBatch batch = {};
        AsyncReadResult results[kPipelineDepth] = {};
        size_t queued = 0;
        size_t batch_requested = 0;
        bool queue_failed = false;
        while (queued < kPipelineDepth && done + batch_requested < size) {
            const size_t remaining = size - done - batch_requested;
            const size_t want = remaining < kChunk ? remaining : kChunk;
            results[queued].batch = &batch;
            results[queued].requested = want;
            const int rc = smb2_pread_async(
                s_smb, s_files[fd], out + done + batch_requested,
                static_cast<uint32_t>(want), file_offset + done + batch_requested,
                file_read_cb, &results[queued]);
            if (rc < 0) {
                queue_failed = true;
                break;
            }
            batch_requested += want;
            ++queued;
        }

        const bool wait_ok = queued > 0u &&
            smb_wait_async_locked(&batch.completed, queued,
                                  &s_cancel_file_read,
                                  kSmbTimeoutSeconds * 1000);
        if (!wait_ok) {
            queue_failed = true;
        }

        size_t batch_done = 0;
        bool short_read = false;
        for (size_t i = 0; i < queued; ++i) {
            if (results[i].status < 0) {
                queue_failed = true;
                break;
            }
            const size_t got = static_cast<size_t>(results[i].status);
            batch_done += got;
            if (got < results[i].requested) {
                short_read = true;
                break;
            }
        }
        done += batch_done;

        // pread callbacks may complete out of order and each updates fh->offset;
        // restore the one authoritative sequential position after the batch.
        if (s_smb != nullptr && !s_smb_bad) {
            uint64_t current = 0;
            (void)smb2_lseek(s_smb, s_files[fd],
                             static_cast<int64_t>(file_offset + done),
                             SEEK_SET, &current);
        }

        if (queue_failed) {
            const char *error = s_smb != nullptr ? smb2_get_error(s_smb)
                                                 : "connection aborted";
            int first_status = 0;
            for (size_t i = 0; i < queued; ++i) {
                if (results[i].status < 0) {
                    first_status = results[i].status;
                    break;
                }
            }
            ESP_LOGW(TAG,
                     "read batch failed at %llu: queued=%u completed=%u status=%d error=%s",
                     static_cast<unsigned long long>(file_offset + done),
                     static_cast<unsigned>(queued),
                     static_cast<unsigned>(batch.completed), first_status,
                     error);
            if (s_cancel_file_read.load() || smb_error_is_connection_loss(error)) {
                s_smb_bad = true;
            }
            if (done > 0) {
                break; // hand back what we already have
            }
            errno = EIO;
            return -1;
        }
        if (short_read || batch_done < batch_requested) {
            break; // EOF or short read
        }
    }
    return static_cast<ssize_t>(done);
}

static off_t vfs_lseek(void *, const int fd, const off_t offset, const int whence)
{
    LockGuard lock;
    if (s_smb == nullptr || s_smb_bad || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    uint64_t current = 0;
    const int64_t pos = smb2_lseek(s_smb, s_files[fd], offset, whence, &current);
    if (pos < 0) {
        errno = EINVAL;
        return -1;
    }
    return static_cast<off_t>(pos);
}

static int vfs_close(void *, const int fd)
{
    LockGuard lock;
    if (fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (s_smb != nullptr && !s_smb_bad) {
        (void)smb2_close(s_smb, s_files[fd]);
    }
    s_files[fd] = nullptr;
    return 0;
}

static int vfs_fstat(void *, const int fd, struct stat *st)
{
    LockGuard lock;
    if (s_smb == nullptr || s_smb_bad || fd < 0 || fd >= kMaxOpenFiles || s_files[fd] == nullptr) {
        errno = EBADF;
        return -1;
    }
    struct smb2_stat_64 smb_st = {};
    if (smb2_fstat(s_smb, s_files[fd], &smb_st) < 0) {
        if (smb_error_is_connection_loss(smb2_get_error(s_smb))) {
            s_smb_bad = true;
        }
        errno = EIO;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = static_cast<off_t>(smb_st.smb2_size);
    st->st_mode = S_IFREG | 0444;
    return 0;
}

static int vfs_stat(void *, const char *path, struct stat *st)
{
    LockGuard lock;
    if (!smb_ensure_connected_locked()) {
        return -1;
    }
    struct smb2_stat_64 smb_st = {};
    if (smb2_stat(s_smb, smb_path(path), &smb_st) < 0) {
        if (smb_error_is_connection_loss(smb2_get_error(s_smb))) {
            s_smb_bad = true;
        }
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = static_cast<off_t>(smb_st.smb2_size);
    st->st_mode = (smb_st.smb2_type == SMB2_TYPE_DIRECTORY) ? (S_IFDIR | 0555)
                                                            : (S_IFREG | 0444);
    return 0;
}

struct RawCommandResult {
    volatile size_t completed;
    int status;
    smb2_file_id file_id;
    SmbVfsDir *dir;
};

static bool raw_wait_for_reply(RawCommandResult *result, const int timeout_ms)
{
    return smb_wait_async_locked(&result->completed, 1u,
                                 &s_cancel_directory_scan,
                                 timeout_ms);
}

static void raw_create_cb(struct smb2_context *, const int status,
                          void *command_data, void *private_data)
{
    RawCommandResult *result = static_cast<RawCommandResult *>(private_data);
    result->status = status;
    if (status == SMB2_STATUS_SUCCESS && command_data != nullptr) {
        const auto *reply = static_cast<const smb2_create_reply *>(command_data);
        memcpy(result->file_id, reply->file_id, SMB2_FD_SIZE);
    }
    result->completed = 1u;
}

static void raw_close_cb(struct smb2_context *, const int status,
                         void *, void *private_data)
{
    RawCommandResult *result = static_cast<RawCommandResult *>(private_data);
    result->status = status;
    result->completed = 1u;
}

static void raw_query_dir_cb(struct smb2_context *smb2, const int status,
                             void *command_data, void *private_data)
{
    RawCommandResult *result = static_cast<RawCommandResult *>(private_data);
    result->status = status;
    if (status == SMB2_STATUS_SUCCESS && command_data != nullptr && result->dir != nullptr) {
        const auto *reply = static_cast<const smb2_query_directory_reply *>(command_data);
        uint32_t offset = 0;
        while (offset < reply->output_buffer_length &&
               result->dir->count < kMaxCachedDirEntries) {
            smb2_iovec vec = {};
            vec.buf = reply->output_buffer + offset;
            vec.len = reply->output_buffer_length - offset;
            smb2_fileidfulldirectoryinformation info = {};
            if (smb2_decode_fileidfulldirectoryinformation(smb2, &info, &vec) < 0 ||
                info.name == nullptr) {
                result->status = -ENOMEM;
                break;
            }

            if (strcmp(info.name, ".") != 0 && strcmp(info.name, "..") != 0) {
                CachedDirEntry &entry = result->dir->entries[result->dir->count];
                const int written = snprintf(entry.name, sizeof(entry.name), "%s", info.name);
                if (written >= 0 && static_cast<size_t>(written) < sizeof(entry.name)) {
                    entry.type = (info.file_attributes & SMB2_FILE_ATTRIBUTE_DIRECTORY)
                                     ? DT_DIR : DT_REG;
                    ++result->dir->count;
                }
            }
            free(const_cast<char *>(info.name));

            if (info.next_entry_offset == 0u) {
                break;
            }
            if (info.next_entry_offset >= vec.len) {
                result->status = -EIO;
                break;
            }
            offset += info.next_entry_offset;
        }
        if (result->dir->count >= kMaxCachedDirEntries) {
            result->dir->truncated = true;
        }
    }
    result->completed = 1u;
}

static bool raw_open_directory(const char *path, smb2_file_id file_id)
{
    smb2_create_request request = {};
    request.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    request.desired_access = SMB2_FILE_LIST_DIRECTORY | SMB2_FILE_READ_ATTRIBUTES;
    request.file_attributes = SMB2_FILE_ATTRIBUTE_DIRECTORY;
    request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE;
    request.create_disposition = SMB2_FILE_OPEN;
    request.create_options = SMB2_FILE_DIRECTORY_FILE;
    request.name = smb_path(path);

    RawCommandResult result = {};
    smb2_pdu *pdu = smb2_cmd_create_async(s_smb, &request, raw_create_cb, &result);
    if (pdu == nullptr) {
        return false;
    }
    smb2_queue_pdu(s_smb, pdu);
    // CREATE is pure metadata (fast on a healthy LAN); a shorter timeout than
    // QUERY_DIRECTORY keeps a dead connection from holding the VFS lock for
    // the full command timeout while every other VFS user (UI included)
    // blocks behind it. Background remount is the mount supervisor's job.
    if (!raw_wait_for_reply(&result, kDirectoryOpenTimeoutMs) ||
        result.status != SMB2_STATUS_SUCCESS) {
        return false;
    }
    memcpy(file_id, result.file_id, SMB2_FD_SIZE);
    return true;
}

static bool raw_close_directory(const smb2_file_id file_id)
{
    smb2_close_request request = {};
    memcpy(request.file_id, file_id, SMB2_FD_SIZE);
    RawCommandResult result = {};
    smb2_pdu *pdu = smb2_cmd_close_async(s_smb, &request, raw_close_cb, &result);
    if (pdu == nullptr) {
        return false;
    }
    smb2_queue_pdu(s_smb, pdu);
    return raw_wait_for_reply(&result, kDirectoryCommandTimeoutMs) &&
           result.status == SMB2_STATUS_SUCCESS;
}

static bool raw_read_directory_batch(SmbVfsDir *dir)
{
    if (dir == nullptr || dir->finished || !dir->remote_open) {
        return true;
    }
    if (s_cancel_directory_scan.load()) {
        ESP_LOGI(TAG, "directory scan canceled: %s", dir->path);
        dir->canceled = true;
        dir->truncated = true;
        dir->finished = true;
        return dir->count > 0u;
    }
    const int64_t elapsed_us = esp_timer_get_time() - dir->scan_started_us;
    const int64_t remaining_us =
        static_cast<int64_t>(kDirectoryScanBudgetMs) * 1000LL - elapsed_us;
    if (remaining_us <= 0 || dir->query_count >= kMaxDirectoryQueries ||
        dir->count >= kMaxCachedDirEntries) {
        dir->truncated = true;
        dir->finished = true;
        return true;
    }

    smb2_query_directory_request request = {};
    request.file_information_class = SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION;
    request.flags = dir->first_query ? SMB2_RESTART_SCANS : 0u;
    memcpy(request.file_id, dir->file_id, SMB2_FD_SIZE);
    request.name = "*";
    request.output_buffer_length = kDirectoryReplyBytes;

    RawCommandResult result = {};
    result.dir = dir;
    smb2_pdu *pdu = smb2_cmd_query_directory_async(
        s_smb, &request, raw_query_dir_cb, &result);
    if (pdu == nullptr) {
        return false;
    }
    const size_t count_before = dir->count;
    smb2_queue_pdu(s_smb, pdu);
    const int remaining_ms = static_cast<int>(remaining_us / 1000LL);
    const int command_timeout = remaining_ms < kDirectoryCommandTimeoutMs
                                    ? remaining_ms : kDirectoryCommandTimeoutMs;
    if (!raw_wait_for_reply(&result, command_timeout > 0 ? command_timeout : 1)) {
        if (s_cancel_directory_scan.load()) {
            ESP_LOGI(TAG, "directory query canceled with %u entries cached",
                     static_cast<unsigned>(dir->count));
            dir->canceled = true;
        } else {
            ESP_LOGW(TAG, "directory query %u timed out with %u entries cached",
                     static_cast<unsigned>(dir->query_count + 1u),
                     static_cast<unsigned>(dir->count));
            dir->context_stale = true;
        }
        dir->truncated = true;
        dir->finished = true;
        return dir->count > 0u;
    }
    ++dir->query_count;
    dir->first_query = false;
    const uint32_t status = static_cast<uint32_t>(result.status);
    if (status == SMB2_STATUS_NO_MORE_FILES) {
        dir->finished = true;
        return true;
    }
    if (status != SMB2_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "directory query %u failed: status=0x%08lx, %u entries cached",
                 static_cast<unsigned>(dir->query_count),
                 static_cast<unsigned long>(status),
                 static_cast<unsigned>(dir->count));
        dir->context_stale = true;
        dir->truncated = dir->count > 0u;
        dir->finished = true;
        return dir->count > 0u;
    }
    if (dir->count == count_before) {
        if (++dir->empty_replies >= 2u) {
            ESP_LOGW(TAG, "directory returned repeated empty replies");
            dir->truncated = true;
            dir->finished = true;
        }
    } else {
        dir->empty_replies = 0;
    }
    if (dir->count >= kMaxCachedDirEntries) {
        dir->truncated = true;
        dir->finished = true;
    }
    return true;
}

static DIR *vfs_opendir(void *, const char *path)
{
    LockGuard lock;
    if (!smb_ensure_connected_locked()) {
        return nullptr;
    }
    s_cancel_directory_scan.store(false);
    SmbVfsDir *dir = static_cast<SmbVfsDir *>(calloc(1, sizeof(SmbVfsDir)));
    if (dir == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    dir->cache_slot = acquire_directory_cache();
    if (dir->cache_slot < 0) {
        free(dir);
        ESP_LOGW(TAG, "all directory cache slots are busy");
        errno = EBUSY;
        return nullptr;
    }
    dir->entries = s_directory_cache[dir->cache_slot];

    ESP_LOGI(TAG, "directory scan started: %s", path);

    // Fail fast on a dead connection: the inline reconnect+retry that used to
    // sit here held the VFS lock through a synchronous connect (15-30 s),
    // freezing every other VFS user (the UI scan status included). Recovery is
    // the mount supervisor's job; it remounts and rescans in the background.
    if (!raw_open_directory(path, dir->file_id)) {
        const char *error = s_smb != nullptr ? smb2_get_error(s_smb)
                                             : "connection aborted";
        // Heap telemetry on the failure path: an internal-RAM squeeze stalls
        // lwIP/AES allocs and looks exactly like a dead NAS (5 s CREATE
        // timeout), so record whether memory was actually tight.
        ESP_LOGW(TAG, "open directory %s failed: %s (dram free=%u largest=%u)",
                 path, error,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        s_smb_bad = true;
        release_directory_cache(dir->cache_slot);
        free(dir);
        errno = EIO;
        return nullptr;
    }
    dir->remote_open = true;
    dir->first_query = true;
    dir->scan_started_us = esp_timer_get_time();
    snprintf(dir->path, sizeof(dir->path), "%s", path);
    ++s_open_dirs;
    return &dir->dir;
}

static void finish_remote_dir_locked(SmbVfsDir *dir)
{
    if (dir == nullptr || !dir->remote_open) {
        return;
    }
    if (dir->context_stale || s_smb == nullptr || s_smb_bad) {
        s_smb_bad = true;
    } else if (!raw_close_directory(dir->file_id)) {
        // A canceled scan takes this branch too: the session is healthy, so
        // the handle is closed normally and s_smb_bad is left alone.
        const char *error = s_smb != nullptr ? smb2_get_error(s_smb)
                                             : "connection aborted";
        ESP_LOGW(TAG, "close directory %s failed: %s", dir->path, error);
        s_smb_bad = true;
    }
    dir->remote_open = false;
    if (s_open_dirs > 0) {
        --s_open_dirs;
    }
    if (dir->truncated) {
        ESP_LOGW(TAG, "directory %s limited to %u entries", dir->path,
                 static_cast<unsigned>(dir->count));
    }
    ESP_LOGI(TAG, "directory scan finished: %s, %u entries, %lld ms%s (dram free=%u min=%u)",
             dir->path, static_cast<unsigned>(dir->count),
             static_cast<long long>((esp_timer_get_time() - dir->scan_started_us) / 1000LL),
             dir->truncated ? " (partial)" : "",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
}

static struct dirent *vfs_readdir(void *, DIR *pdir)
{
    SmbVfsDir *dir = reinterpret_cast<SmbVfsDir *>(pdir);
    if (dir == nullptr || dir->entries == nullptr) {
        return nullptr;
    }
    if (dir->index >= dir->count && !dir->finished) {
        LockGuard lock;
        if (s_smb == nullptr || s_smb_bad || !raw_read_directory_batch(dir)) {
            dir->finished = true;
            // A canceled empty scan returns false too; that is local control
            // flow, not a stale connection.
            if (!dir->canceled) {
                dir->context_stale = true;
            }
        }
        if (dir->finished) {
            finish_remote_dir_locked(dir);
        }
    }
    if (dir->index >= dir->count) {
        return nullptr;
    }
    const CachedDirEntry &cached = dir->entries[dir->index++];
    memset(&dir->entry, 0, sizeof(dir->entry));
    snprintf(dir->entry.d_name, sizeof(dir->entry.d_name), "%s", cached.name);
    dir->entry.d_type = cached.type;
    return &dir->entry;
}

static int vfs_closedir(void *, DIR *pdir)
{
    SmbVfsDir *dir = reinterpret_cast<SmbVfsDir *>(pdir);
    if (dir == nullptr) {
        errno = EBADF;
        return -1;
    }
    {
        LockGuard lock;
        finish_remote_dir_locked(dir);
        release_directory_cache(dir->cache_slot);
    }
    free(dir);
    return 0;
}

static bool s_registered = false;

static bool register_vfs(void)
{
    if (s_registered) {
        return true;
    }
    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_CONTEXT_PTR;
    vfs.open_p = vfs_open;
    vfs.read_p = vfs_read;
    vfs.lseek_p = vfs_lseek;
    vfs.close_p = vfs_close;
    vfs.fstat_p = vfs_fstat;
    vfs.stat_p = vfs_stat;
    vfs.opendir_p = vfs_opendir;
    vfs.readdir_p = vfs_readdir;
    vfs.closedir_p = vfs_closedir;
    if (esp_vfs_register(SMB_VFS_MOUNT_POINT, &vfs, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "vfs register failed");
        return false;
    }
    s_registered = true;
    return true;
}

} // namespace

extern "C" bool SMB_VFS_Mount(const char *server, const char *share,
                              const char *user, const char *password)
{
    if (server == nullptr || server[0] == '\0' || share == nullptr || share[0] == '\0') {
        return false;
    }
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            return false;
        }
    }
    SMB_VFS_Unmount();
    if (!register_vfs()) {
        return false;
    }

    snprintf(s_server, sizeof(s_server), "%s", server);
    snprintf(s_share, sizeof(s_share), "%s", share);
    snprintf(s_user, sizeof(s_user), "%s", (user != nullptr) ? user : "");
    snprintf(s_password, sizeof(s_password), "%s", (password != nullptr) ? password : "");

    LockGuard lock;
    if (!smb_connect_locked()) {
        return false;
    }
    ESP_LOGI(TAG, "//%s/%s mounted at %s (user=%s)", s_server, s_share, SMB_VFS_MOUNT_POINT,
             (s_user[0] != '\0') ? s_user : "guest");
    return true;
}

extern "C" void SMB_VFS_Unmount(void)
{
    if (s_lock == nullptr) {
        return;
    }
    LockGuard lock;
    if (smb_has_open_handles_locked()) {
        ESP_LOGW(TAG, "unmount deferred: SMB handles still open");
        s_smb_bad = true;
        return;
    }
    if (s_smb != nullptr && !s_smb_bad) {
        for (int i = 0; i < kMaxOpenFiles; ++i) {
            if (s_files[i] != nullptr) {
                (void)smb2_close(s_smb, s_files[i]);
                s_files[i] = nullptr;
            }
        }
        (void)smb2_disconnect_share(s_smb);
    }
    smb_destroy_context_locked();
    s_smb_bad = false;
    ESP_LOGI(TAG, "unmounted");
}

extern "C" bool SMB_VFS_Mounted(void)
{
    return s_smb != nullptr && !s_smb_bad;
}

extern "C" void SMB_VFS_CancelDirectoryScan(void)
{
    s_cancel_directory_scan.store(true);
}

extern "C" void SMB_VFS_CancelFileRead(void)
{
    s_cancel_file_read.store(true);
}
