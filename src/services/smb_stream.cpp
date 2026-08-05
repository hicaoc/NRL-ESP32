#include "services/smb_stream.h"

#include "services/smb_vfs.h"
#include "services/storage_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <smb2/libsmb2.h>
#include <smb2/smb2.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>

static const char *TAG = "SMBSTRM";

namespace {

// TCP retransmission on a busy Wi-Fi link can exceed two seconds. Rebuilding
// the SMB session that early repeatedly interrupts an otherwise recoverable
// read, so allow TCP five seconds to finish before reconnecting.
constexpr int kCommandTimeoutMs = 5000;
// One larger request sustains lossless audio without concurrent operations on
// a libsmb2 context. Each pread is a synchronous round-trip (request -> RTT +
// server + local parsing -> response), so the request size directly sets how
// much of the link's idle turnaround is amortized: 64 KB reads quarter the
// round-trips of the original 16 KB, and TCP streams the multi-segment
// response continuously through the enlarged lwIP receive window. The old
// 1200-byte workaround was stable but too latency-bound for FLAC.
constexpr uint32_t kWireReadBytes = 64u * 1024u;
// Two lanes (parallel SMB sessions to the same file): a single synchronous
// pread per round-trip window could not keep a FLAC (lossless, ~100+ KB/s
// file rate) ahead of the 176 KB/s PCM drain when the NAS answers in
// 300-600 ms bursts -- the PCM ring ran dry every few seconds. Two 64 KB
// preads in flight double the windowed throughput; the wait handles partial
// per-lane results and only the received prefix advances the offset.
constexpr size_t kLaneCount = 2;
constexpr size_t kConfigTextBytes = 128;
constexpr size_t kPathBytes = 512;
constexpr uint32_t kRetryReadBytes[] = {
    64u * 1024u,
    32u * 1024u,
    16u * 1024u,
    1200u,
};
constexpr unsigned kReconnectAttempts =
    (sizeof(kRetryReadBytes) / sizeof(kRetryReadBytes[0])) - 1u;

struct ReadBatch;

struct ReadResult {
    ReadBatch *batch;
    int status;
    size_t requested;
};

struct ReadBatch {
    volatile size_t completed;
};

struct WaitStats {
    unsigned polls;
    unsigned readable;
    unsigned writable;
    short last_wanted;
    short last_returned;
};

enum class WaitResult {
    Done,
    Canceled,
    Timeout,
    TransportError,
};

struct SmbLane {
    struct smb2_context *context;
    struct smb2fh *file;
};

static void read_cb(struct smb2_context *, const int status,
                    void *, void *private_data)
{
    ReadResult *result = static_cast<ReadResult *>(private_data);
    result->status = status;
    result->batch->completed = result->batch->completed + 1u;
}

static bool path_is_smb(const char *path)
{
    constexpr size_t kMountLen = sizeof(SMB_VFS_MOUNT_POINT) - 1u;
    return path != nullptr &&
           strncmp(path, SMB_VFS_MOUNT_POINT, kMountLen) == 0 &&
           (path[kMountLen] == '/' || path[kMountLen] == '\0');
}

static const char *share_path(const char *path)
{
    constexpr size_t kMountLen = sizeof(SMB_VFS_MOUNT_POINT) - 1u;
    const char *relative = path + kMountLen;
    return relative[0] == '/' ? relative + 1 : relative;
}

} // namespace

struct SmbStream {
    SmbLane lanes[kLaneCount];
    const volatile bool *stop_requested;
    uint32_t max_read_bytes;
    uint64_t offset;
    char vfs_path[kPathBytes];
    bool failed;
};

namespace {

static bool stop_requested(const SmbStream *stream)
{
    return stream->stop_requested != nullptr && *stream->stop_requested;
}

static void destroy_lane(SmbLane *lane)
{
    if (lane->context == nullptr) return;
    // The context owns the file handle. Destroying it synchronously cancels
    // queued callbacks before their stack-owned ReadResult leaves scope.
    lane->file = nullptr;
    smb2_destroy_context(lane->context);
    lane->context = nullptr;
}

static void destroy_lanes(SmbStream *stream)
{
    for (size_t i = 0; i < kLaneCount; ++i) destroy_lane(&stream->lanes[i]);
}

static bool open_lane(SmbStream *stream, const size_t index,
                      const bool reconnecting)
{
    if (stop_requested(stream)) return false;

    char server[kConfigTextBytes] = {};
    char share[kConfigTextBytes] = {};
    char user[kConfigTextBytes] = {};
    char password[kConfigTextBytes] = {};
    if (!STORAGE_SmbGetConfig(server, sizeof(server), share, sizeof(share),
                              user, sizeof(user), password, sizeof(password))) {
        return false;
    }

    SmbLane *lane = &stream->lanes[index];
    destroy_lane(lane);
    lane->context = smb2_init_context();
    if (lane->context == nullptr) return false;
    smb2_set_timeout(lane->context, kCommandTimeoutMs / 1000);
    smb2_set_security_mode(lane->context, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (password[0] != '\0') smb2_set_password(lane->context, password);
    const char *login = user[0] != '\0' ? user : "guest";
    if (smb2_connect_share(lane->context, server, share, login) < 0) {
        ESP_LOGW(TAG, "%s lane %u //%s/%s failed: %s",
                 reconnecting ? "reconnect" : "connect",
                 static_cast<unsigned>(index), server, share,
                 smb2_get_error(lane->context));
        destroy_lane(lane);
        return false;
    }
    lane->file = smb2_open(lane->context, share_path(stream->vfs_path), O_RDONLY);
    if (lane->file == nullptr) {
        ESP_LOGW(TAG, "open lane %u %s failed: %s",
                 static_cast<unsigned>(index), stream->vfs_path,
                 smb2_get_error(lane->context));
        destroy_lane(lane);
        return false;
    }
    const uint32_t negotiated = smb2_get_max_read_size(lane->context);
    const uint32_t lane_max = negotiated == 0u
                                  ? kWireReadBytes
                                  : (negotiated < kWireReadBytes
                                         ? negotiated : kWireReadBytes);
    if (stream->max_read_bytes == 0u || lane_max < stream->max_read_bytes) {
        stream->max_read_bytes = lane_max;
    }
    return true;
}

static bool open_lanes(SmbStream *stream, const bool reconnecting)
{
    stream->max_read_bytes = 0;
    for (size_t i = 0; i < kLaneCount; ++i) {
        if (!open_lane(stream, i, reconnecting)) {
            destroy_lanes(stream);
            return false;
        }
    }
    if (reconnecting) {
        ESP_LOGI(TAG, "resumed %s at %llu", stream->vfs_path,
                 static_cast<unsigned long long>(stream->offset));
    }
    return true;
}

static WaitResult wait_for_reads(SmbStream *stream, ReadBatch *batch,
                                 const size_t expected, WaitStats *stats)
{
    const int64_t deadline = esp_timer_get_time() +
                             static_cast<int64_t>(kCommandTimeoutMs) * 1000LL;
    while (batch->completed < expected) {
        if (stop_requested(stream)) return WaitResult::Canceled;
        if (esp_timer_get_time() >= deadline) return WaitResult::Timeout;

        struct pollfd pfds[kLaneCount] = {};
        for (size_t i = 0; i < expected; ++i) {
            pfds[i].fd = smb2_get_fd(stream->lanes[i].context);
            pfds[i].events = smb2_which_events(stream->lanes[i].context);
            stats->last_wanted |= pfds[i].events;
        }
        const int polled = poll(pfds, expected, 100);
        ++stats->polls;
        if (polled < 0) {
            if (errno == EINTR) continue;
            return WaitResult::TransportError;
        }
        for (size_t i = 0; i < expected; ++i) {
            stats->last_returned |= pfds[i].revents;
            if ((pfds[i].revents & POLLIN) != 0) ++stats->readable;
            if ((pfds[i].revents & POLLOUT) != 0) ++stats->writable;
            if (pfds[i].revents != 0 &&
                smb2_service(stream->lanes[i].context, pfds[i].revents) < 0) {
                return WaitResult::TransportError;
            }
        }
    }
    return WaitResult::Done;
}

} // namespace

extern "C" SmbStream *SMB_STREAM_Open(
    const char *vfs_path, const volatile bool *external_stop)
{
    if (!path_is_smb(vfs_path) ||
        (external_stop != nullptr && *external_stop)) {
        return nullptr;
    }
    if (strlen(vfs_path) >= kPathBytes) {
        ESP_LOGW(TAG, "SMB path is too long");
        return nullptr;
    }

    SmbStream *stream = static_cast<SmbStream *>(heap_caps_calloc(
        1, sizeof(SmbStream), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stream == nullptr) return nullptr;
    stream->stop_requested = external_stop;
    snprintf(stream->vfs_path, sizeof(stream->vfs_path), "%s", vfs_path);
    if (!open_lanes(stream, false)) {
        heap_caps_free(stream);
        return nullptr;
    }
    ESP_LOGI(TAG, "opened %s (async read=%ux%lu)", vfs_path,
             static_cast<unsigned>(kLaneCount),
             static_cast<unsigned long>(stream->max_read_bytes));
    return stream;
}

extern "C" int SMB_STREAM_Read(SmbStream *stream, void *dst, const size_t size)
{
    if (stream == nullptr || dst == nullptr || size == 0u ||
        stream->failed || stop_requested(stream)) {
        return stream != nullptr && stop_requested(stream) ? 0 : -1;
    }

    for (unsigned attempt = 0; attempt <= kReconnectAttempts; ++attempt) {
        const uint32_t attempt_limit = kRetryReadBytes[attempt];
        bool lanes_ready = true;
        for (size_t i = 0; i < kLaneCount; ++i) {
            if (stream->lanes[i].context == nullptr ||
                stream->lanes[i].file == nullptr) {
                lanes_ready = false;
                break;
            }
        }
        if (!lanes_ready && !open_lanes(stream, true)) {
            if (stop_requested(stream)) return 0;
            continue;
        }

        ReadBatch batch = {};
        ReadResult results[kLaneCount] = {};
        WaitStats stats = {};
        size_t queued = 0;
        size_t queued_bytes = 0;
        while (queued < kLaneCount && queued_bytes < size) {
            const size_t remaining = size - queued_bytes;
            uint32_t want = stream->max_read_bytes;
            if (want > attempt_limit) want = attempt_limit;
            if (remaining < want) want = static_cast<uint32_t>(remaining);
            results[queued].batch = &batch;
            results[queued].requested = want;
            SmbLane *lane = &stream->lanes[queued];
            if (smb2_pread_async(
                    lane->context, lane->file,
                    static_cast<uint8_t *>(dst) + queued_bytes, want,
                    stream->offset + queued_bytes, read_cb,
                    &results[queued]) < 0) {
                break;
            }
            queued_bytes += want;
            ++queued;
        }

        WaitResult waited = WaitResult::TransportError;
        if (queued == 0u) {
            ESP_LOGW(TAG, "queue read failed");
        } else {
            waited = wait_for_reads(stream, &batch, queued, &stats);
        }

        bool status_error = false;
        size_t received = 0;
        if (waited == WaitResult::Done) {
            for (size_t i = 0; i < queued; ++i) {
                if (results[i].status < 0) {
                    status_error = true;
                    break;
                }
                const size_t got = static_cast<size_t>(results[i].status);
                received += got;
                if (got < results[i].requested) break;
            }
        }
        if (waited == WaitResult::Done && !status_error) {
            stream->offset += received;
            return static_cast<int>(received);
        }
        if (waited == WaitResult::Canceled || stop_requested(stream)) {
            ESP_LOGI(TAG, "read canceled");
            destroy_lanes(stream);
            return 0;
        }
        if (waited == WaitResult::Timeout) {
            ESP_LOGW(TAG,
                     "read timeout %d ms block=%lu lanes=%u/%u offset=%llu polls=%u rd=%u wr=%u; reconnecting",
                     kCommandTimeoutMs,
                     static_cast<unsigned long>(attempt_limit),
                     static_cast<unsigned>(batch.completed),
                     static_cast<unsigned>(queued),
                     static_cast<unsigned long long>(stream->offset),
                     stats.polls, stats.readable, stats.writable);
        } else {
            ESP_LOGW(TAG, "read lanes failed at %llu; reconnecting",
                     static_cast<unsigned long long>(stream->offset));
        }
        destroy_lanes(stream);
    }

    ESP_LOGE(TAG, "read failed after %u reconnect attempts at %llu",
             kReconnectAttempts,
             static_cast<unsigned long long>(stream->offset));
    stream->failed = true;
    return -1;
}

extern "C" bool SMB_STREAM_Seek(SmbStream *stream,
                                const int64_t offset, const int whence)
{
    if (stream == nullptr || stream->failed) return false;
    uint64_t target = 0;
    if (whence == SEEK_SET && offset >= 0) {
        target = static_cast<uint64_t>(offset);
    } else if (whence == SEEK_CUR && offset >= 0 &&
               static_cast<uint64_t>(offset) <= UINT64_MAX - stream->offset) {
        target = stream->offset + static_cast<uint64_t>(offset);
    } else if (whence == SEEK_CUR && offset < 0 &&
               offset != INT64_MIN &&
               static_cast<uint64_t>(-offset) <= stream->offset) {
        target = stream->offset - static_cast<uint64_t>(-offset);
    } else {
        // The media decoder only requires SET/CUR. SEEK_END would need a
        // synchronous remote stat and is intentionally not used here.
        return false;
    }
    stream->offset = target;
    return true;
}

extern "C" void SMB_STREAM_Close(SmbStream *stream)
{
    if (stream == nullptr) return;
    for (size_t i = 0; i < kLaneCount; ++i) {
        SmbLane *lane = &stream->lanes[i];
        if (lane->context != nullptr && !stream->failed) {
            if (lane->file != nullptr) {
                (void)smb2_close(lane->context, lane->file);
                lane->file = nullptr;
            }
            (void)smb2_disconnect_share(lane->context);
        }
        destroy_lane(lane);
    }
    heap_caps_free(stream);
}
