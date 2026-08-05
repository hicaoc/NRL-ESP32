#ifndef SRC_MEDIA_GMF_IO_RADIO_H
#define SRC_MEDIA_GMF_IO_RADIO_H

// GMF IO "io_radio": direct HTTP(S) net-radio streams. This is a custom IO
// instead of GMF's io_http because the media player relies on a TLS
// availability fallback io_http does not have: open verifies against the cert
// bundle first and, on handshake failure, retries once without verification
// (net-radio CDNs commonly serve incomplete chains; these are public audio
// streams, so availability wins over authenticity on the retry). Hosts that
// needed the fallback are remembered so follow-up connections (HLS playlist
// polls / segments via the pool) skip the doomed verified attempt.

#include "esp_gmf_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int               dir;    // ESP_GMF_IO_DIR_READER
    const char       *name;   // instance tag, NULL -> "io_radio"
    esp_gmf_io_cfg_t  io_cfg; // thread + buffer configuration
} gmf_io_radio_cfg_t;

esp_gmf_err_t gmf_io_radio_init(gmf_io_radio_cfg_t *config, esp_gmf_io_handle_t *io);

// Arm the cooperative-stop pointer honoured by blocking reads (the media
// decoder's stop_requested). Set on the pipeline's IN io instance before
// each run; only one radio playback is active at a time.
void gmf_io_radio_set_stop_request(esp_gmf_io_handle_t io, const volatile bool *stop_requested);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_GMF_IO_RADIO_H
