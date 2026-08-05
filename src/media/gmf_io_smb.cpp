#include "media/gmf_io_smb.h"

#include <stdio.h>
#include <string.h>

#include "esp_gmf_oal_mem.h"
#include "esp_gmf_payload.h"
#include "services/smb_stream.h"
#include "services/smb_vfs.h"

// GMF IO "io_smb": see gmf_io_smb.h. Modelled on esp-gmf's esp_gmf_io_file.c.
// Reads are synchronous round-trips driven by the IO's own thread (configured
// via io_cfg), so SMB network jitter is absorbed by the IO ringbuffer, not by
// the decode pipeline.

namespace {

typedef struct {
    esp_gmf_io_t          base;           // must be the first member
    bool                  is_open;
    SmbStream            *fp;
    const volatile bool *stop_requested;  // armed per playback, may be NULL
} smb_io_stream_t;

esp_gmf_err_t smb_io_init(gmf_io_smb_cfg_t *config, esp_gmf_io_handle_t *io);

esp_gmf_err_t _smb_new(void *cfg, esp_gmf_obj_handle_t *io)
{
    return smb_io_init(static_cast<gmf_io_smb_cfg_t *>(cfg),
                       reinterpret_cast<esp_gmf_io_handle_t *>(io));
}

esp_gmf_err_t _smb_get_score(esp_gmf_io_handle_t handle, const char *url, int *score)
{
    *score = ESP_GMF_IO_SCORE_NONE;
    if (url == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    // The media player passes VFS-style /smb/... paths (no scheme). Claim
    // exactly those; http(s) and local mounts keep their own IOs.
    constexpr size_t kMountLen = sizeof(SMB_VFS_MOUNT_POINT) - 1u;
    if (strncmp(url, SMB_VFS_MOUNT_POINT, kMountLen) == 0 &&
        (url[kMountLen] == '/' || url[kMountLen] == '\0')) {
        *score = ESP_GMF_IO_SCORE_STANDARD;
    }
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t _smb_open(esp_gmf_io_handle_t handle)
{
    smb_io_stream_t *io = reinterpret_cast<smb_io_stream_t *>(handle);
    char *uri = nullptr;
    esp_gmf_io_get_uri(handle, &uri);
    if (uri == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    io->fp = SMB_STREAM_Open(uri, io->stop_requested);
    if (io->fp == nullptr) {
        return ESP_GMF_ERR_FAIL;
    }
    esp_gmf_info_file_t info = {};
    esp_gmf_io_get_info(handle, &info);
    if (info.pos > 0) {
        // Resume position requested (e.g. pipeline seek): SmbStream seeks are
        // local offset bookkeeping, the next pread uses the absolute offset.
        if (!SMB_STREAM_Seek(io->fp, static_cast<int64_t>(info.pos), SEEK_SET)) {
            SMB_STREAM_Close(io->fp);
            io->fp = nullptr;
            return ESP_GMF_ERR_FAIL;
        }
    }
    io->is_open = true;
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_io_t _smb_acquire_read(esp_gmf_io_handle_t handle, void *payload,
                                   uint32_t wanted_size, int block_ticks)
{
    smb_io_stream_t *io = reinterpret_cast<smb_io_stream_t *>(handle);
    esp_gmf_payload_t *pload = static_cast<esp_gmf_payload_t *>(payload);
    if (io->fp == nullptr || pload == nullptr || pload->buf == nullptr) {
        return ESP_GMF_IO_FAIL;
    }
    const int rlen = SMB_STREAM_Read(io->fp, pload->buf, wanted_size);
    if (rlen < 0) {
        return ESP_GMF_IO_FAIL;
    }
    pload->valid_size = static_cast<size_t>(rlen);
    if (rlen == 0) {
        pload->is_done = true; // EOF propagates downstream as FINISHED
    }
    return ESP_GMF_IO_OK;
}

esp_gmf_err_io_t _smb_release_read(esp_gmf_io_handle_t handle, void *payload, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

esp_gmf_err_t _smb_seek(esp_gmf_io_handle_t handle, uint64_t pos)
{
    smb_io_stream_t *io = reinterpret_cast<smb_io_stream_t *>(handle);
    if (io->fp == nullptr) {
        return ESP_GMF_ERR_FAIL;
    }
    return SMB_STREAM_Seek(io->fp, static_cast<int64_t>(pos), SEEK_SET)
               ? ESP_GMF_ERR_OK
               : ESP_GMF_ERR_FAIL;
}

esp_gmf_err_t _smb_close(esp_gmf_io_handle_t handle)
{
    smb_io_stream_t *io = reinterpret_cast<smb_io_stream_t *>(handle);
    if (io->is_open) {
        SMB_STREAM_Close(io->fp);
        io->fp = nullptr;
        io->is_open = false;
    }
    esp_gmf_io_set_pos(handle, 0);
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t _smb_delete(esp_gmf_obj_handle_t handle)
{
    void *cfg = OBJ_GET_CFG(handle);
    if (cfg != nullptr) {
        esp_gmf_oal_free(cfg);
    }
    esp_gmf_io_deinit(reinterpret_cast<esp_gmf_io_handle_t>(handle));
    esp_gmf_oal_free(handle);
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t smb_io_init(gmf_io_smb_cfg_t *config, esp_gmf_io_handle_t *io)
{
    if (config == nullptr || io == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    smb_io_stream_t *s = static_cast<smb_io_stream_t *>(
        esp_gmf_oal_calloc(1, sizeof(smb_io_stream_t)));
    if (s == nullptr) {
        return ESP_GMF_ERR_MEMORY_LACK;
    }
    s->base.dir = static_cast<esp_gmf_io_dir_t>(config->dir);
    s->base.type = ESP_GMF_IO_TYPE_BYTE;
    esp_gmf_obj_t *obj = reinterpret_cast<esp_gmf_obj_t *>(s);
    obj->new_obj = _smb_new;
    obj->del_obj = _smb_delete;
    gmf_io_smb_cfg_t *cfg = static_cast<gmf_io_smb_cfg_t *>(
        esp_gmf_oal_calloc(1, sizeof(gmf_io_smb_cfg_t)));
    if (cfg == nullptr) {
        esp_gmf_oal_free(s);
        return ESP_GMF_ERR_MEMORY_LACK;
    }
    memcpy(cfg, config, sizeof(gmf_io_smb_cfg_t));
    esp_gmf_obj_set_config(obj, cfg, sizeof(gmf_io_smb_cfg_t));
    esp_gmf_obj_set_tag(obj, config->name != nullptr ? config->name : "io_smb");
    s->base.get_score = _smb_get_score;
    s->base.open = _smb_open;
    s->base.close = _smb_close;
    s->base.seek = _smb_seek;
    s->base.acquire_read = _smb_acquire_read;
    s->base.release_read = _smb_release_read;
    const esp_gmf_err_t ret = esp_gmf_io_init(obj, &config->io_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(obj);
        return ret;
    }
    *io = obj;
    return ESP_GMF_ERR_OK;
}

} // namespace

extern "C" esp_gmf_err_t gmf_io_smb_init(gmf_io_smb_cfg_t *config, esp_gmf_io_handle_t *io)
{
    return smb_io_init(config, io);
}

extern "C" void gmf_io_smb_set_stop_request(esp_gmf_io_handle_t io,
                                            const volatile bool *stop_requested)
{
    if (io == nullptr) {
        return;
    }
    reinterpret_cast<smb_io_stream_t *>(io)->stop_requested = stop_requested;
}
