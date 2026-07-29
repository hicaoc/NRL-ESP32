// SSTV transmit service, see sstv_service.h for the module contract.

#include "sstv_service.h"

#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_jpeg_enc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../audio/audio_router.h"
#include "../lib/nrl_psram.h"
#include "../media/cover_decoder.h"
#include "storage_service.h"

static const char *TAG = "SSTV";

namespace {

constexpr size_t kTaskStackBytes = 8192u;
constexpr size_t kJpegCap = 2u * 1024u * 1024u; // input JPEG read cap
constexpr size_t kCamJpegCap = 192u * 1024u;    // camera JPEG request buffer
constexpr uint32_t kDecodeMaxDim = 640u;        // PD120 needs the full 640 px
constexpr uint16_t kFrameWidth = SSTV_IMAGE_WIDTH;

// TX frame geometry per mode (RX is always 320 wide).
uint16_t frameWidth(SSTV_Mode mode)
{
    return mode == SSTV_MODE_PD120 ? 640u : 320u;
}

uint16_t frameHeight(SSTV_Mode mode)
{
    return mode == SSTV_MODE_ROBOT36 ? 240u : (mode == SSTV_MODE_MARTIN_M1) ? 256u : 496u;
}

SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
SstvSnapshot s_snap = {};

// Latched request for the worker.
bool s_stop_requested = false;
bool s_req_from_buffer = false;
SSTV_Mode s_req_mode = SSTV_MODE_ROBOT36;
char s_req_path[sizeof(s_snap.path)] = {};
uint8_t *s_req_jpeg = nullptr; // kCamJpegCap, allocated once (camera frames)
size_t s_req_jpeg_size = 0u;

// Frame buffer, allocated once (160 KB): PSRAM where available.
uint16_t *s_image = nullptr;

// RX side: the decoder callbacks fire from the audio task that pushes into
// our sink, so they only touch plain single-writer fields (no lock, no heap).
uint16_t *s_rx_image = nullptr;      // 320x256 RGB565, PSRAM
volatile uint32_t s_rx_revision = 0u; // bumps per received line

void rxOnVis(SSTV_Mode, void *)
{
    if (s_rx_image != nullptr) {
        memset(s_rx_image, 0, kFrameWidth * 256u * sizeof(uint16_t));
    }
    s_rx_revision = 0u;
}

void rxOnLine(uint16_t y, const uint16_t *row, void *)
{
    if (s_rx_image != nullptr && row != nullptr && y < 256u) {
        memcpy(s_rx_image + static_cast<uint32_t>(y) * kFrameWidth, row,
               kFrameWidth * sizeof(uint16_t));
    }
    // Plain assignment: ++ on a volatile object is deprecated in C++20 and
    // this build treats warnings as errors (CI caught it on all boards).
    s_rx_revision = s_rx_revision + 1u;
}

void rxOnDone(void *)
{
    s_rx_revision = s_rx_revision + 1u;
}

// 16 kHz tap for the demodulator (see audio_router sink docs).
void sstvSink(uint8_t, const int16_t *samples, size_t sample_count, void *)
{
    SSTV_RX_Feed(samples, sample_count);
}

// Stack in PSRAM where the target allows external stacks (the worker blocks
// on FatFS reads and the 10 ms pacing); internal SRAM otherwise.
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && \
    CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
NRL_PSRAM_BSS StackType_t s_task_stack[kTaskStackBytes / sizeof(StackType_t)];
#else
StackType_t s_task_stack[kTaskStackBytes / sizeof(StackType_t)];
#endif
StaticTask_t s_task_tcb;

void setState(SstvState state)
{
    s_snap.state = state;
    ++s_snap.revision;
}

// Decode a JPEG from memory and cover-crop-scale it into s_image at the
// mode's frame geometry. Runs in the worker task WITHOUT s_lock held.
bool prepareImageBuffer(const uint8_t *jpeg, size_t jpeg_size, SSTV_Mode mode,
                        const char *label, const char **error)
{
    const uint16_t target_w = frameWidth(mode);
    const uint16_t target_h = frameHeight(mode);
    CoverBitmap bmp = {};
    const bool decoded = COVER_DecodeJpeg(jpeg, jpeg_size, kDecodeMaxDim, &bmp);
    if (!decoded || bmp.rgb565 == nullptr || bmp.width == 0u || bmp.height == 0u) {
        COVER_Free(&bmp);
        *error = "DECODE";
        return false;
    }
    if (s_image == nullptr) {
        COVER_Free(&bmp);
        *error = "NOMEM";
        return false;
    }

    // Cover-crop: scale so the source covers the frame, then center-sample.
    const float scale = (static_cast<float>(target_w) / bmp.width >
                         static_cast<float>(target_h) / bmp.height)
                            ? static_cast<float>(target_w) / bmp.width
                            : static_cast<float>(target_h) / bmp.height;
    const float view_w = static_cast<float>(target_w) / scale;
    const float view_h = static_cast<float>(target_h) / scale;
    const float x0 = (static_cast<float>(bmp.width) - view_w) * 0.5f;
    const float y0 = (static_cast<float>(bmp.height) - view_h) * 0.5f;
    for (uint16_t y = 0u; y < target_h; ++y) {
        int sy = static_cast<int>(y0 + (static_cast<float>(y) + 0.5f) * view_h / target_h);
        if (sy < 0) sy = 0;
        if (sy >= bmp.height) sy = bmp.height - 1u;
        for (uint16_t x = 0u; x < target_w; ++x) {
            int sx = static_cast<int>(x0 + (static_cast<float>(x) + 0.5f) * view_w / target_w);
            if (sx < 0) sx = 0;
            if (sx >= bmp.width) sx = bmp.width - 1u;
            s_image[static_cast<uint32_t>(y) * target_w + x] =
                reinterpret_cast<const uint16_t *>(bmp.rgb565)[static_cast<uint32_t>(sy) * bmp.width + sx];
        }
    }
    ESP_LOGI(TAG, "prepared %ux%u frame from %ux%u (%s)", target_w, target_h,
             bmp.width, bmp.height, label);
    COVER_Free(&bmp);
    return true;
}

// Decode `path` and cover-crop-scale it into s_image at the mode's frame
// geometry. Runs in the worker task WITHOUT s_lock held; on failure it
// returns false with a short reason in `error` (the caller files it into the
// snapshot under the lock).
bool prepareImage(const char *path, SSTV_Mode mode, const char **error)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        *error = "OPEN";
        return false;
    }
    uint8_t *jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(kJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg == nullptr) {
        jpeg = static_cast<uint8_t *>(heap_caps_malloc(kJpegCap, MALLOC_CAP_8BIT));
    }
    size_t jpeg_size = 0u;
    if (jpeg != nullptr && fseek(file, 0, SEEK_END) == 0) {
        const long size = ftell(file);
        if (size > 0 && static_cast<size_t>(size) <= kJpegCap && fseek(file, 0, SEEK_SET) == 0 &&
            fread(jpeg, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size)) {
            jpeg_size = static_cast<size_t>(size);
        }
    }
    fclose(file);
    if (jpeg_size == 0u) {
        if (jpeg != nullptr) {
            heap_caps_free(jpeg);
        }
        *error = "READ";
        return false;
    }
    const bool ok = prepareImageBuffer(jpeg, jpeg_size, mode, path, error);
    heap_caps_free(jpeg);
    return ok;
}

void worker(void *)
{
    int16_t chunk[SSTV_CHUNK_SAMPLES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const SSTV_Mode mode = s_req_mode;
        const bool from_buffer = s_req_from_buffer;
        char path[sizeof(s_req_path)] = {};
        snprintf(path, sizeof(path), "%s", s_req_path);
        s_stop_requested = false;
        s_snap.mode = mode;
        s_snap.progress_percent = 0u;
        s_snap.error[0] = '\0';
        snprintf(s_snap.path, sizeof(s_snap.path), "%s",
                 from_buffer ? "(camera)" : path);
        setState(SSTV_STATE_PREPARING);
        xSemaphoreGive(s_lock);

        const char *error = nullptr;
        const bool ready = from_buffer
                               ? prepareImageBuffer(s_req_jpeg, s_req_jpeg_size, mode,
                                                    "camera", &error)
                               : prepareImage(path, mode, &error);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool stopped = s_stop_requested;
        if (!ready) {
            snprintf(s_snap.error, sizeof(s_snap.error), "%s",
                     error != nullptr ? error : "");
            setState(SSTV_STATE_ERROR);
        } else if (!stopped) {
            setState(SSTV_STATE_SENDING);
        }
        xSemaphoreGive(s_lock);

        bool completed = false;
        if (ready && !stopped) {
            SSTV_TxInit(mode);
            (void)SSTV_TxSetImage(s_image, frameWidth(mode), frameHeight(mode));
            for (;;) {
                const bool more = SSTV_TxFillChunk(chunk);
                AudioRouter_PushFrame(AUDIO_SRC_SSTV_NRL, SSTV_SAMPLE_RATE_HZ, chunk,
                                      SSTV_CHUNK_SAMPLES);
                AudioRouter_PushFrame(AUDIO_SRC_SSTV_SPEAKER, SSTV_SAMPLE_RATE_HZ, chunk,
                                      SSTV_CHUNK_SAMPLES);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                const uint32_t percent = static_cast<uint64_t>(SSTV_TxElapsedSamples()) * 100u /
                                         SSTV_TxTotalSamples();
                if (percent != s_snap.progress_percent) {
                    s_snap.progress_percent = static_cast<uint8_t>(percent);
                    ++s_snap.revision;
                }
                const bool stop = s_stop_requested;
                xSemaphoreGive(s_lock);
                if (stop) {
                    break;
                }
                if (!more) {
                    completed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (completed) {
            s_snap.progress_percent = 100u;
            setState(SSTV_STATE_DONE);
        } else if (stopped || (ready && !completed)) {
            setState(SSTV_STATE_DONE); // aborted mid-frame
        }
        // !ready: the ERROR state was already filed above.
        s_stop_requested = false;
        xSemaphoreGive(s_lock);
    }
}

} // namespace

void SSTV_SERVICE_Init(void)
{
    if (s_lock != nullptr) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) {
        ESP_LOGE(TAG, "no mutex");
        return;
    }
    s_image = static_cast<uint16_t *>(heap_caps_malloc(
        static_cast<size_t>(640u) * 496u * sizeof(uint16_t), // largest frame: PD120
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_image == nullptr) {
        s_image = static_cast<uint16_t *>(heap_caps_malloc(
            static_cast<size_t>(640u) * 496u * sizeof(uint16_t), MALLOC_CAP_8BIT));
    }
    if (s_image == nullptr) {
        ESP_LOGE(TAG, "no frame buffer");
    }
    s_req_jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(kCamJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_req_jpeg == nullptr) {
        ESP_LOGW(TAG, "no camera request buffer, SendJpegBuffer disabled");
    }
    s_rx_image = static_cast<uint16_t *>(heap_caps_malloc(
        static_cast<size_t>(kFrameWidth) * 256u * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_rx_image == nullptr) {
        s_rx_image = static_cast<uint16_t *>(heap_caps_malloc(
            static_cast<size_t>(kFrameWidth) * 256u * sizeof(uint16_t), MALLOC_CAP_8BIT));
    }
    if (s_rx_image == nullptr) {
        ESP_LOGE(TAG, "no RX frame buffer");
    }
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = SSTV_STATE_IDLE;
    SSTV_RX_Init(rxOnVis, rxOnLine, rxOnDone, nullptr);
    AudioRouter_RegisterSink(AUDIO_SINK_SSTV, SSTV_SAMPLE_RATE_HZ, sstvSink, nullptr);
    AudioRouter_SetRoute(AUDIO_SRC_SSTV_NRL, AUDIO_SINK_NRL_UPLINK, true);
    AudioRouter_SetRoute(AUDIO_SRC_SSTV_SPEAKER, AUDIO_SINK_SPEAKER, true);
    // Core 1 with the audio pipeline, same priority as the CW tx worker.
    if (xTaskCreateStaticPinnedToCore(worker, "sstv_tx", kTaskStackBytes, nullptr, 5,
                                      s_task_stack, &s_task_tcb, 1) == nullptr) {
        ESP_LOGE(TAG, "worker task create failed");
        s_task = nullptr;
    }
}

bool SSTV_SERVICE_SendJpeg(const char *path, SSTV_Mode mode)
{
    if (path == nullptr || path[0] == '\0' || s_lock == nullptr || s_task == nullptr) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_snap.state == SSTV_STATE_PREPARING || s_snap.state == SSTV_STATE_SENDING ||
        s_snap.rx_active) {
        xSemaphoreGive(s_lock);
        return false;
    }
    snprintf(s_req_path, sizeof(s_req_path), "%s", path);
    s_req_mode = mode;
    s_req_from_buffer = false;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

bool SSTV_SERVICE_SendJpegBuffer(const uint8_t *jpeg, size_t jpeg_size, SSTV_Mode mode)
{
    if (jpeg == nullptr || jpeg_size == 0u || jpeg_size > kCamJpegCap ||
        s_lock == nullptr || s_task == nullptr || s_req_jpeg == nullptr) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_snap.state == SSTV_STATE_PREPARING || s_snap.state == SSTV_STATE_SENDING ||
        s_snap.rx_active) {
        xSemaphoreGive(s_lock);
        return false;
    }
    memcpy(s_req_jpeg, jpeg, jpeg_size);
    s_req_jpeg_size = jpeg_size;
    s_req_from_buffer = true;
    s_req_mode = mode;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

bool SSTV_SERVICE_Stop(void)
{
    if (s_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = s_snap.state == SSTV_STATE_PREPARING ||
                        s_snap.state == SSTV_STATE_SENDING;
    if (active) {
        s_stop_requested = true;
    }
    xSemaphoreGive(s_lock);
    return active;
}

bool SSTV_SERVICE_StartRx(SstvRxSource source)
{
    if (s_lock == nullptr || s_rx_image == nullptr) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_snap.state == SSTV_STATE_PREPARING || s_snap.state == SSTV_STATE_SENDING) {
        xSemaphoreGive(s_lock);
        return false; // TX owns the airtime
    }
    SSTV_RX_Reset();
    s_rx_revision = 0u;
    s_snap.rx_source = source;
    s_snap.rx_active = true;
    s_snap.rx_lines = 0u;
    ++s_snap.revision;
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_SSTV, source == SSTV_SOURCE_MIC);
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_SSTV, source == SSTV_SOURCE_NRL);
    xSemaphoreGive(s_lock);
    return true;
}

bool SSTV_SERVICE_StopRx(void)
{
    if (s_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was = s_snap.rx_active;
    s_snap.rx_active = false;
    ++s_snap.revision;
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_SSTV, false);
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_SSTV, false);
    SSTV_RX_Reset();
    xSemaphoreGive(s_lock);
    return was;
}

const uint16_t *SSTV_SERVICE_RxImage(void)
{
    return s_rx_image;
}

uint32_t SSTV_SERVICE_RxImageRevision(void)
{
    return s_rx_revision;
}

bool SSTV_SERVICE_SaveRxJpeg(char *out_path, size_t out_path_size)
{
    if (s_rx_image == nullptr || !STORAGE_SdMounted()) {
        return false;
    }
    const uint16_t lines = SSTV_RX_LinesReceived();
    const uint16_t height = static_cast<uint16_t>(lines & ~7u); // MCU-friendly
    if (height < 8u) {
        return false;
    }
    jpeg_enc_config_t enc_cfg = DEFAULT_JPEG_ENC_CONFIG();
    enc_cfg.width = static_cast<int>(kFrameWidth);
    enc_cfg.height = height;
    enc_cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    enc_cfg.subsampling = JPEG_SUBSAMPLE_420;
    enc_cfg.quality = 85u;
    jpeg_enc_handle_t enc = nullptr;
    if (jpeg_enc_open(&enc_cfg, &enc) != JPEG_ERR_OK || enc == nullptr) {
        return false;
    }
    constexpr size_t kJpegOutCap = 96u * 1024u;
    uint8_t *jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(kJpegOutCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int jpeg_size = 0;
    bool ok = jpeg != nullptr &&
              jpeg_enc_process(enc, reinterpret_cast<const uint8_t *>(s_rx_image),
                               static_cast<int>(kFrameWidth) * height * 2, jpeg,
                               static_cast<int>(kJpegOutCap), &jpeg_size) == JPEG_ERR_OK &&
              jpeg_size > 0;
    (void)jpeg_enc_close(enc);
    if (ok) {
        char path[96];
        snprintf(path, sizeof(path), "%s/sstv_rx_%lu.jpg", STORAGE_SdMountPoint(),
                 static_cast<unsigned long>(xTaskGetTickCount() * portTICK_PERIOD_MS));
        FILE *file = fopen(path, "wb");
        if (file != nullptr) {
            ok = fwrite(jpeg, 1, static_cast<size_t>(jpeg_size), file) ==
                 static_cast<size_t>(jpeg_size);
            fclose(file);
            if (ok) {
                ESP_LOGI(TAG, "saved %s (%ux%u)", path, kFrameWidth, height);
                if (out_path != nullptr && out_path_size > 0u) {
                    snprintf(out_path, out_path_size, "%s", path);
                }
            }
        } else {
            ok = false;
        }
    }
    if (jpeg != nullptr) {
        heap_caps_free(jpeg);
    }
    return ok;
}

void SSTV_SERVICE_GetSnapshot(SstvSnapshot *out)
{
    if (out == nullptr) {
        return;
    }
    if (s_lock == nullptr) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    // Decoder-side live counters are single-writer (audio task); read unlocked.
    out->rx_state = s_snap.rx_active ? SSTV_RX_GetState() : SSTV_RX_HUNT;
    out->rx_mode = SSTV_RX_GetMode();
    out->rx_lines = SSTV_RX_LinesReceived();
    out->rx_lines_total = SSTV_RX_LinesTotal();
    out->rx_quality = s_snap.rx_active ? SSTV_RX_SignalQuality() : 0u;
    out->rx_revision = s_rx_revision;
    xSemaphoreGive(s_lock);
}
