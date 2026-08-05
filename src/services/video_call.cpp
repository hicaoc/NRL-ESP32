#include "services/video_call.h"

#include "driver/board_pins.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO

#include "lib/nrl_audio_bridge.h"
#include "lib/nrl_psram.h"

#include "bsp/camera.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string.h>

static const char *TAG = "VIDEO";

namespace {

constexpr size_t kSubHeaderBytes = 4;
constexpr size_t kFragDataBytes = 1020;      // NRL max payload (1024) - sub header
constexpr size_t kMaxJpegBytes = VIDEO_MAX_JPEG_BYTES; // VGA JPEG headroom
constexpr uint32_t kTxIntervalMs = 200;      // ~5 fps
constexpr int kJpegQuality = 40;
constexpr int64_t kRxActiveWindowUs = 2000000; // "receiving" indicator window

// ---- TX ---------------------------------------------------------------------
// The camera device stays open for the lifetime of the firmware: esp-video's
// DVP pipeline teardown (esp_video_deinit) takes several seconds and races
// with the TX task's V4L2 calls, so "Cam OFF" only stops the stream
// (STREAMOFF) and the TX task; "Cam ON" restarts them on the same handle.
static bsp_camera_t *s_camera = nullptr;
static TaskHandle_t s_tx_task = nullptr;
static volatile bool s_tx_run = false;
static uint16_t s_tx_seq = 0;

// Local self-view: the TX task publishes each captured JPEG here so the LCD
// can render the local camera without a second sensor stream. Copy-out under
// the lock keeps the decode task tear-free at the cost of one ~10-30 KB
// PSRAM memcpy per frame (negligible against the 200 ms frame interval).
NRL_PSRAM_BSS static uint8_t s_local_storage[kMaxJpegBytes];
static uint8_t *s_local = s_local_storage;
static SemaphoreHandle_t s_local_lock = nullptr;
static size_t s_local_bytes = 0;
static volatile uint32_t s_local_seq = 0;

// ---- RX ---------------------------------------------------------------------
// Double buffer: fragments assemble into s_assm; a completed frame is copied
// under the mutex into s_ready for the UI to decode at its own pace.
NRL_PSRAM_BSS static uint8_t s_assm_storage[kMaxJpegBytes];
NRL_PSRAM_BSS static uint8_t s_ready_storage[kMaxJpegBytes];
static uint8_t *s_assm = s_assm_storage;
static uint8_t *s_ready = s_ready_storage;
static SemaphoreHandle_t s_ready_lock = nullptr;
static uint16_t s_assm_seq = 0;
static uint8_t s_assm_cnt = 0;
static uint32_t s_assm_mask[8] = {};         // fragment-received bitmap (<=255)
static size_t s_assm_bytes = 0;
static size_t s_assm_last_frag_len = 0;
static volatile uint32_t s_ready_seq = 0;    // monotonically grows per shown frame
static size_t s_ready_bytes = 0;
static volatile int64_t s_last_rx_us = 0;
static uint32_t s_frames_completed = 0;

static bool assm_all_received(void)
{
    for (uint8_t i = 0; i < s_assm_cnt; ++i) {
        if ((s_assm_mask[i / 32u] & (1u << (i % 32u))) == 0u) {
            return false;
        }
    }
    return s_assm_cnt > 0u;
}

static void video_rx_handler(const uint8_t *payload, const size_t payload_size)
{
    if (payload_size <= kSubHeaderBytes || s_assm == nullptr) {
        return;
    }
    const uint16_t seq = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
    const uint8_t idx = payload[2];
    const uint8_t cnt = payload[3];
    const size_t data_len = payload_size - kSubHeaderBytes;
    if (cnt == 0u || idx >= cnt || data_len > kFragDataBytes ||
        (static_cast<size_t>(idx) * kFragDataBytes + data_len) > kMaxJpegBytes) {
        return;
    }

    s_last_rx_us = esp_timer_get_time();

    if (seq != s_assm_seq || cnt != s_assm_cnt) {
        // New frame (or sender restarted): drop any partial assembly.
        s_assm_seq = seq;
        s_assm_cnt = cnt;
        memset(s_assm_mask, 0, sizeof(s_assm_mask));
        s_assm_bytes = 0;
        s_assm_last_frag_len = 0;
    }

    memcpy(s_assm + static_cast<size_t>(idx) * kFragDataBytes, payload + kSubHeaderBytes, data_len);
    s_assm_mask[idx / 32u] |= (1u << (idx % 32u));
    if (idx == cnt - 1u) {
        s_assm_last_frag_len = data_len;
    }

    if (s_assm_last_frag_len == 0u || !assm_all_received()) {
        return;
    }

    const size_t total = static_cast<size_t>(s_assm_cnt - 1u) * kFragDataBytes + s_assm_last_frag_len;
    if (s_ready_lock != nullptr && xSemaphoreTake(s_ready_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(s_ready, s_assm, total);
        s_ready_bytes = total;
        s_ready_seq = ++s_frames_completed;
        xSemaphoreGive(s_ready_lock);
    }
    memset(s_assm_mask, 0, sizeof(s_assm_mask));
    s_assm_last_frag_len = 0;
}

static void video_tx_task(void *)
{
    while (s_tx_run) {
        const int64_t cycle_start = esp_timer_get_time();

        bsp_camera_frame_t frame = {};
        if (bsp_camera_get_frame(s_camera, &frame) == ESP_OK && frame.data != nullptr) {
            const uint8_t *jpeg = static_cast<const uint8_t *>(frame.data);
            const size_t jpeg_size = frame.size;
            if (jpeg_size > 0u && jpeg_size <= kMaxJpegBytes) {
                // Publish for the local self-view before the network send so
                // the preview isn't delayed by the fragment burst.
                if (s_local != nullptr && s_local_lock != nullptr &&
                    xSemaphoreTake(s_local_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                    memcpy(s_local, jpeg, jpeg_size);
                    s_local_bytes = jpeg_size;
                    s_local_seq = s_local_seq + 1u; // volatile: no ++ in C++20
                    xSemaphoreGive(s_local_lock);
                }
                const uint8_t cnt = static_cast<uint8_t>((jpeg_size + kFragDataBytes - 1u) / kFragDataBytes);
                ++s_tx_seq;
                static uint8_t payload[kSubHeaderBytes + kFragDataBytes];
                for (uint8_t idx = 0; idx < cnt && s_tx_run; ++idx) {
                    const size_t offset = static_cast<size_t>(idx) * kFragDataBytes;
                    const size_t chunk = ((jpeg_size - offset) < kFragDataBytes)
                                             ? (jpeg_size - offset)
                                             : kFragDataBytes;
                    payload[0] = static_cast<uint8_t>(s_tx_seq >> 8);
                    payload[1] = static_cast<uint8_t>(s_tx_seq & 0xFFu);
                    payload[2] = idx;
                    payload[3] = cnt;
                    memcpy(payload + kSubHeaderBytes, jpeg + offset, chunk);
                    (void)NRLAudioBridge_SendTyped(13u, payload, kSubHeaderBytes + chunk);
                    // Space the burst out slightly so a 20-30 packet frame
                    // doesn't monopolise the WiFi queue against voice.
                    if ((idx & 0x03u) == 0x03u) {
                        vTaskDelay(1);
                    }
                }
            }
            (void)bsp_camera_return_frame(s_camera, &frame);
        }

        const int64_t elapsed_ms = (esp_timer_get_time() - cycle_start) / 1000;
        if (elapsed_ms < kTxIntervalMs) {
            vTaskDelay(pdMS_TO_TICKS(kTxIntervalMs - static_cast<uint32_t>(elapsed_ms)));
        }
    }
    s_tx_task = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void VIDEO_Init(void)
{
    if (s_ready_lock == nullptr) {
        s_ready_lock = xSemaphoreCreateMutex();
    }
    if (s_local_lock == nullptr) {
        s_local_lock = xSemaphoreCreateMutex();
    }
    if (s_ready_lock == nullptr || s_local_lock == nullptr) {
        ESP_LOGE(TAG, "rx buffer alloc failed");
        return;
    }
    NRLAudioBridge_SetVideoRxHandler(video_rx_handler);
}

extern "C" bool VIDEO_SetTxEnabled(const bool enabled)
{
    if (!enabled) {
        s_tx_run = false;
        // The TX task may be blocked inside a DQBUF ioctl; it wakes with the
        // next captured frame (~83 ms at 12 fps). Wait generously, and never
        // touch the device from here while the task could still issue V4L2
        // calls on it.
        for (int i = 0; i < 200 && s_tx_task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_tx_task != nullptr) {
            ESP_LOGE(TAG, "tx task did not exit; leaving stream running");
            return false;
        }
        if (s_camera != nullptr) {
            (void)bsp_camera_stop(s_camera); // STREAMOFF only; device stays open
        }
        s_local_seq = 0; // self-view frames belong to the stopped session
        return true;
    }

    if (s_tx_task != nullptr) {
        return true;
    }
    if (s_camera == nullptr) {
        // The OV3660's only DVP JPEG mode: 1280x720 @ 12 fps, 10 MHz XCLK
        // (sensor-side encode, no CPU cost). We pace it down to ~5 fps.
        bsp_camera_config_t cfg = BSP_CAMERA_DEFAULT_CONFIG();
        cfg.width = 1280;
        cfg.height = 720;
        cfg.pixel_format = BSP_CAMERA_PIXEL_FORMAT_JPEG;
        cfg.xclk_freq_hz = 10 * 1000 * 1000;
        if (bsp_camera_open(&cfg, &s_camera) != ESP_OK || s_camera == nullptr) {
            ESP_LOGE(TAG, "camera open failed");
            s_camera = nullptr;
            return false;
        }
        (void)bsp_camera_set_jpeg_quality(s_camera, kJpegQuality);
    }
    if (bsp_camera_start(s_camera) != ESP_OK) { // no-op when already streaming
        ESP_LOGE(TAG, "camera start failed");
        return false;
    }
    s_tx_run = true;
    if (xTaskCreatePinnedToCore(video_tx_task, "video_tx", 4096, nullptr, 4, &s_tx_task, 0) != pdPASS) {
        s_tx_run = false;
        s_tx_task = nullptr;
        (void)bsp_camera_stop(s_camera);
        ESP_LOGE(TAG, "tx task create failed");
        return false;
    }
    ESP_LOGI(TAG, "camera streaming (720p JPEG q%d, ~%lu fps)",
             kJpegQuality, static_cast<unsigned long>(1000u / kTxIntervalMs));
    return true;
}

extern "C" bool VIDEO_TxEnabled(void)
{
    return s_tx_task != nullptr;
}

extern "C" bool VIDEO_AcquireFrame(uint8_t *dst, const size_t dst_cap,
                                    size_t *jpeg_size, uint32_t *seq)
{
    if (dst == nullptr || jpeg_size == nullptr || seq == nullptr ||
        s_ready == nullptr || s_ready_lock == nullptr) {
        return false;
    }
    const uint32_t current = s_ready_seq;
    if (current == 0u || current == *seq) {
        return false;
    }
    // Copy-out under the lock: the RX reassembly overwrites s_ready whenever
    // a frame completes, so decoding directly from it would read a torn
    // JPEG mid-decode. One ~40-90 KB PSRAM memcpy per frame at ~5 fps is
    // negligible next to the decode itself.
    if (xSemaphoreTake(s_ready_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    const size_t bytes = s_ready_bytes;
    const bool ok = bytes > 0u && bytes <= dst_cap;
    if (ok) {
        memcpy(dst, s_ready, bytes);
        *jpeg_size = bytes;
        *seq = s_ready_seq; // re-read: a frame may have completed meanwhile
    }
    xSemaphoreGive(s_ready_lock);
    return ok;
}

extern "C" bool VIDEO_CopyLocalFrame(uint8_t *dst, const size_t dst_cap,
                                     size_t *size, uint32_t *seq)
{
    if (dst == nullptr || size == nullptr || seq == nullptr ||
        s_local == nullptr || s_local_lock == nullptr) {
        return false;
    }
    const uint32_t current = s_local_seq;
    if (current == 0u || current == *seq) {
        return false;
    }
    if (xSemaphoreTake(s_local_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const size_t bytes = s_local_bytes;
    const bool ok = bytes > 0u && bytes <= dst_cap;
    if (ok) {
        memcpy(dst, s_local, bytes);
        *size = bytes;
        *seq = s_local_seq; // re-read: the TX task may have republished
    }
    xSemaphoreGive(s_local_lock);
    return ok;
}

extern "C" bool VIDEO_Receiving(void)
{
    return (esp_timer_get_time() - s_last_rx_us) < kRxActiveWindowUs;
}

#else // !S31

extern "C" void VIDEO_Init(void) {}
extern "C" bool VIDEO_SetTxEnabled(bool) { return false; }
extern "C" bool VIDEO_TxEnabled(void) { return false; }
extern "C" bool VIDEO_AcquireFrame(uint8_t *, size_t, size_t *, uint32_t *) { return false; }
extern "C" bool VIDEO_CopyLocalFrame(uint8_t *, size_t, size_t *, uint32_t *) { return false; }
extern "C" bool VIDEO_Receiving(void) { return false; }

#endif
