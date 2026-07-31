#ifndef SRC_MEDIA_GMF_IO_SMB_H
#define SRC_MEDIA_GMF_IO_SMB_H

// GMF IO "io_smb": plays audio straight off the SMB2/3 network share through
// the dedicated media connection (services/smb_stream.cpp), which bypasses
// the /smb VFS so directory browsing never shares a socket or mutex with
// playback, and keeps the 64 KB pread + reconnect logic. Wrapping it as a
// GMF IO lets a pipeline pull compressed bytes from /smb/... URIs exactly
// like io_file pulls from /sdcard.

#include "esp_gmf_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int               dir;    // ESP_GMF_IO_DIR_READER
    const char       *name;   // instance tag, NULL -> "io_smb"
    esp_gmf_io_cfg_t  io_cfg; // thread + buffer configuration
} gmf_io_smb_cfg_t;

esp_gmf_err_t gmf_io_smb_init(gmf_io_smb_cfg_t *config, esp_gmf_io_handle_t *io);

// Arm the cooperative-stop pointer honoured by blocking reads (the media
// decoder's stop_requested). Set on the pipeline's IN io instance before
// each run; only one SMB playback is active at a time.
void gmf_io_smb_set_stop_request(esp_gmf_io_handle_t io, const volatile bool *stop_requested);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_GMF_IO_SMB_H
