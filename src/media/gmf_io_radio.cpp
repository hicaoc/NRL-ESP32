#include "media/gmf_io_radio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include "esp_gmf_oal_mem.h"
#include "esp_gmf_payload.h"

// GMF IO "io_radio": see gmf_io_radio.h. Modelled on esp-gmf's
// esp_gmf_io_http.c, with the media decoder's former http_open_stream()
// connect logic (verified-first TLS with one insecure retry) moved into
// _radio_open. Reads run in the IO's own thread (configured via io_cfg), so
// network pacing is absorbed by the IO ringbuffer, not by the decode
// pipeline.

namespace {

const char *TAG = "io_radio";

typedef struct {
    esp_gmf_io_t             base;           // must be the first member
    bool                     is_open;
    esp_http_client_handle_t client;
    const volatile bool     *stop_requested; // armed per playback, may be NULL
    volatile bool            reading;        // IO thread inside esp_http_client_read
} radio_stream_t;

// One-entry cache of the last host that needed the TLS insecure fallback.
// HLS playlist polls and segment fetches come through separate IO instances
// created from this template, so the hint cannot live on an instance; only
// one stream plays at a time, making a single entry sufficient.
char s_insecure_host[128];
bool s_insecure_host_valid = false;

esp_gmf_err_t radio_io_init(gmf_io_radio_cfg_t *config, esp_gmf_io_handle_t *io);

esp_gmf_err_t _radio_new(void *cfg, esp_gmf_obj_handle_t *io)
{
    return radio_io_init(static_cast<gmf_io_radio_cfg_t *>(cfg),
                         reinterpret_cast<esp_gmf_io_handle_t *>(io));
}

esp_gmf_err_t _radio_get_score(esp_gmf_io_handle_t handle, const char *url, int *score)
{
    *score = ESP_GMF_IO_SCORE_NONE;
    if (url == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    if (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0) {
        *score = ESP_GMF_IO_SCORE_STANDARD;
    }
    return ESP_GMF_ERR_OK;
}

// Extract "host[:port]" from an http(s) URL for the insecure-hint cache.
void radio_host_of(const char *url, char *out, const size_t cap)
{
    out[0] = '\0';
    const char *scheme_end = strstr(url, "://");
    if (scheme_end == nullptr) {
        return;
    }
    const char *host = scheme_end + 3;
    const char *end = strchr(host, '/');
    const size_t len = (end != nullptr) ? static_cast<size_t>(end - host) : strlen(host);
    if (len > 0 && len < cap) {
        memcpy(out, host, len);
        out[len] = '\0';
    }
}

// Open the stream, verifying TLS against the cert bundle first; on handshake
// failure retry once without verification (see gmf_io_radio.h for why).
// Returns an OPENED client or nullptr.
esp_http_client_handle_t radio_open_stream(radio_stream_t *io, const char *url,
                                           const int timeout_ms, const int buffer_size)
{
    char host[128];
    radio_host_of(url, host, sizeof(host));
    const bool hint = s_insecure_host_valid && strcmp(host, s_insecure_host) == 0;
    const int first = hint ? 1 : 0;
    for (int insecure = first; insecure < 2; ++insecure) {
        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.timeout_ms = timeout_ms;
        cfg.buffer_size = buffer_size;
        if (insecure == 0) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        if (http == nullptr) {
            return nullptr;
        }
        if (esp_http_client_open(http, 0) == ESP_OK) {
            if (insecure != 0) {
                if (!hint) {
                    ESP_LOGW(TAG, "TLS verification skipped for %s", url);
                    snprintf(s_insecure_host, sizeof(s_insecure_host), "%s", host);
                    s_insecure_host_valid = true;
                }
            }
            return http;
        }
        esp_http_client_cleanup(http);
        if (strncmp(url, "https://", 8) != 0) {
            return nullptr; // plain http can't be a TLS failure; don't retry
        }
    }
    return nullptr;
}

esp_gmf_err_t _radio_open(esp_gmf_io_handle_t handle)
{
    radio_stream_t *io = reinterpret_cast<radio_stream_t *>(handle);
    if (io->is_open) {
        return ESP_GMF_ERR_OK;
    }
    char *uri = nullptr;
    esp_gmf_io_get_uri(handle, &uri);
    if (uri == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "open %s", uri);
    io->client = radio_open_stream(io, uri, 10000, 4096);
    if (io->client == nullptr) {
        ESP_LOGE(TAG, "stream open failed: %s", uri);
        return ESP_GMF_ERR_FAIL;
    }
    const int64_t size = esp_http_client_fetch_headers(io->client);
    const int status = esp_http_client_get_status_code(io->client);
    if (status != 200) {
        ESP_LOGE(TAG, "stream HTTP %d: %s", status, uri);
        (void)esp_http_client_close(io->client);
        esp_http_client_cleanup(io->client);
        io->client = nullptr;
        return ESP_GMF_ERR_FAIL;
    }
    if (size > 0) {
        esp_gmf_io_set_size(handle, static_cast<uint64_t>(size));
    }
    io->is_open = true;
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_io_t _radio_acquire_read(esp_gmf_io_handle_t handle, void *payload,
                                     uint32_t wanted_size, int block_ticks)
{
    radio_stream_t *io = reinterpret_cast<radio_stream_t *>(handle);
    esp_gmf_payload_t *pload = static_cast<esp_gmf_payload_t *>(payload);
    if (io->client == nullptr || pload == nullptr || pload->buf == nullptr) {
        return ESP_GMF_IO_FAIL;
    }
    if (io->stop_requested != nullptr && *io->stop_requested) {
        return ESP_GMF_IO_ABORT;
    }
    io->reading = true;
    const int rlen = esp_http_client_read(io->client, reinterpret_cast<char *>(pload->buf),
                                          static_cast<int>(wanted_size));
    io->reading = false;
    if (rlen < 0) {
        if (io->stop_requested != nullptr && *io->stop_requested) {
            // Teardown shut the socket down under this read: an abort, not an
            // error (keeps GMF off the "Job failed" path on playback stop).
            return ESP_GMF_IO_ABORT;
        }
        ESP_LOGW(TAG, "read error %d", rlen);
        return ESP_GMF_IO_FAIL;
    }
    pload->valid_size = static_cast<size_t>(rlen);
    if (rlen == 0) {
        pload->is_done = true; // server closed the stream: EOF downstream
    }
    return ESP_GMF_IO_OK;
}

esp_gmf_err_io_t _radio_release_read(esp_gmf_io_handle_t handle, void *payload, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

// Break a read blocked inside the TLS/TCP stack so the IO thread (and with it
// pipeline stop) does not wait out the 10 s read timeout.
void radio_shutdown_socket(radio_stream_t *io)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 3)
    if (io->client != nullptr) {
        const int fd = esp_http_client_get_socket(io->client);
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
        }
    }
#endif
}

esp_gmf_err_t _radio_prev_close(esp_gmf_io_handle_t handle)
{
    radio_shutdown_socket(reinterpret_cast<radio_stream_t *>(handle));
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t _radio_close(esp_gmf_io_handle_t handle)
{
    radio_stream_t *io = reinterpret_cast<radio_stream_t *>(handle);
    if (io->client != nullptr) {
        // The IO thread may still be blocked in esp_http_client_read. Closing
        // the socket under it makes the read fail with ENOTCONN (noisy
        // transport_base/HTTP_CLIENT errors, and esp_http_client state freed
        // mid-read). Shut the socket down so the read exits, then wait for it
        // (bounded) before closing.
        radio_shutdown_socket(io);
        for (int i = 0; i < 200 && io->reading; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        (void)esp_http_client_close(io->client);
        esp_http_client_cleanup(io->client);
        io->client = nullptr;
    }
    io->is_open = false;
    esp_gmf_io_set_pos(handle, 0);
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t _radio_reload(esp_gmf_io_handle_t handle, const char *new_uri)
{
    // Used by the HLS IO to hop between playlist/segment URLs without
    // destroying the IO instance.
    (void)_radio_close(handle);
    esp_gmf_io_set_uri(handle, new_uri);
    return _radio_open(handle);
}

esp_gmf_err_t _radio_delete(esp_gmf_obj_handle_t handle)
{
    void *cfg = OBJ_GET_CFG(handle);
    if (cfg != nullptr) {
        esp_gmf_oal_free(cfg);
    }
    esp_gmf_io_deinit(reinterpret_cast<esp_gmf_io_handle_t>(handle));
    esp_gmf_oal_free(handle);
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t radio_io_init(gmf_io_radio_cfg_t *config, esp_gmf_io_handle_t *io)
{
    if (config == nullptr || io == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    radio_stream_t *s = static_cast<radio_stream_t *>(
        esp_gmf_oal_calloc(1, sizeof(radio_stream_t)));
    if (s == nullptr) {
        return ESP_GMF_ERR_MEMORY_LACK;
    }
    s->base.dir = static_cast<esp_gmf_io_dir_t>(config->dir);
    s->base.type = ESP_GMF_IO_TYPE_BYTE;
    esp_gmf_obj_t *obj = reinterpret_cast<esp_gmf_obj_t *>(s);
    obj->new_obj = _radio_new;
    obj->del_obj = _radio_delete;
    gmf_io_radio_cfg_t *cfg = static_cast<gmf_io_radio_cfg_t *>(
        esp_gmf_oal_calloc(1, sizeof(gmf_io_radio_cfg_t)));
    if (cfg == nullptr) {
        esp_gmf_oal_free(s);
        return ESP_GMF_ERR_MEMORY_LACK;
    }
    memcpy(cfg, config, sizeof(gmf_io_radio_cfg_t));
    esp_gmf_obj_set_config(obj, cfg, sizeof(gmf_io_radio_cfg_t));
    esp_gmf_obj_set_tag(obj, config->name != nullptr ? config->name : "io_radio");
    s->base.get_score = _radio_get_score;
    s->base.open = _radio_open;
    s->base.prev_close = _radio_prev_close;
    s->base.close = _radio_close;
    s->base.reload = _radio_reload;
    s->base.acquire_read = _radio_acquire_read;
    s->base.release_read = _radio_release_read;
    const esp_gmf_err_t ret = esp_gmf_io_init(obj, &config->io_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(obj);
        return ret;
    }
    *io = obj;
    return ESP_GMF_ERR_OK;
}

} // namespace

extern "C" esp_gmf_err_t gmf_io_radio_init(gmf_io_radio_cfg_t *config, esp_gmf_io_handle_t *io)
{
    return radio_io_init(config, io);
}

extern "C" void gmf_io_radio_set_stop_request(esp_gmf_io_handle_t io,
                                              const volatile bool *stop_requested)
{
    if (io == nullptr) {
        return;
    }
    reinterpret_cast<radio_stream_t *>(io)->stop_requested = stop_requested;
}
