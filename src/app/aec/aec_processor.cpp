#include "aec/aec_processor.h"

#include "driver/board_pins.h"

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE

#include <esp_afe_config.h>
#include <esp_afe_sr_models.h>
#include <esp_ae_rate_cvt.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <model_path.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "AEC";

namespace {

// ---- Resampler ---------------------------------------------------------
// 16k->8k downsampler backed by esp_audio_effects' band-limited rate
// converter (replaces the old hand-written 31-tap windowed-sinc FIR).
// esp_ae_rate_cvt_handle_t s_rate_cvt is opened in afe_create_pipeline().

// ---- AFE state ---------------------------------------------------------
constexpr size_t kOutFrameSamples = 80; // 8 kHz NRL uplink frame

const esp_afe_sr_iface_t *s_iface = nullptr;
esp_afe_sr_data_t *s_afe = nullptr;
int s_feed_chunk = 0;  // per-channel samples per feed (16 kHz)
int s_fetch_chunk = 0; // samples per fetch (16 kHz)

int16_t *s_feed_buf = nullptr; // interleaved channels @16 kHz
size_t s_feed_cap = 0;         // per-channel capacity
size_t s_feed_fill = 0;        // per-channel samples buffered
size_t s_feed_channels = 1;    // 1=mic only, 2=mic+ref

// Downsampler state (see "Resampler" note above).
esp_ae_rate_cvt_handle_t s_rate_cvt = nullptr;
int16_t *s_down_buf = nullptr;   // 8 kHz scratch for the fetch task
uint32_t s_down_buf_cap = 0;     // samples

int16_t s_out_frame[kOutFrameSamples];
size_t s_out_fill = 0;

AEC_OutputCallback s_out_cb = nullptr;
void *s_out_user = nullptr;

TaskHandle_t s_fetch_task = nullptr;
volatile bool s_fetch_task_exited = false;
volatile bool s_running = false;
bool s_ready = false;
bool s_has_reference = false;
bool s_has_ai_noise = false;
volatile bool s_runtime_aec_enabled = false;
volatile bool s_runtime_ai_noise_enabled = false;
srmodel_list_t *s_models = nullptr;

const char *input_format_for(const bool use_ref_channel) {
    return use_ref_channel ? "MR" : "M";
}

void aec_fetch_task(void *) {
    while (s_running) {
        afe_fetch_result_t *res = s_iface->fetch(s_afe);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        const size_t n16 = static_cast<size_t>(res->data_size) / sizeof(int16_t);
        size_t off = 0;
        while (off < n16) {
            const size_t block = (n16 - off > 2048u) ? 2048u : (n16 - off);
            uint32_t n8 = s_down_buf_cap;
            if (esp_ae_rate_cvt_process(s_rate_cvt,
                                        const_cast<int16_t *>(res->data + off),
                                        static_cast<uint32_t>(block),
                                        s_down_buf, &n8) != ESP_AE_ERR_OK) {
                n8 = 0;
            }
            for (uint32_t i = 0; i < n8; ++i) {
                s_out_frame[s_out_fill++] = s_down_buf[i];
                if (s_out_fill == kOutFrameSamples) {
                    if (s_out_cb != nullptr) {
                        s_out_cb(s_out_frame, kOutFrameSamples, s_out_user);
                    }
                    s_out_fill = 0;
                }
            }
            off += block;
        }
        taskYIELD();
    }
    s_fetch_task_exited = true;
    // Do not self-delete a WithCaps task. The owner deletes this suspended
    // task from afe_destroy_pipeline(), avoiding the SMP cleanup-task race.
    vTaskSuspend(nullptr);
}

} // namespace

// Build the AFE pipeline (handle + buffers + fetch task) using already-loaded
// models. Shared between AEC_Init (first time) and AEC_Reconfigure (live
// reinit on ai_noise toggle).
static bool afe_create_pipeline(bool enable_aec, bool enable_ai_noise) {
    const bool use_ref_channel = enable_aec;

    afe_config_t *cfg = afe_config_init(input_format_for(use_ref_channel),
                                        s_models,
                                        AFE_TYPE_VC,
                                        AFE_MODE_LOW_COST);
    if (cfg == nullptr) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return false;
    }

    cfg->aec_init = use_ref_channel;
    cfg->se_init = false;
    cfg->vad_init = false;
    cfg->wakenet_init = false;
    cfg->agc_init = false;
    // ★ key: `ns_init` (the boolean) is what truly stops NSNET2 RNN
    // inference; `afe_ns_mode` only picks the model when ns_init is true.
    // Toggling ns_init off here gives Reconfigure() a real runtime
    // disable instead of "module is on but its output is hidden behind a
    // routing flag".
    cfg->ns_init = enable_ai_noise;
    cfg->afe_ns_mode = AFE_NS_MODE_NET;
    // Push the bulk of the AFE/NSNET2 working set into PSRAM. Internal SRAM is
    // scarce once WiFi/BT/ESP-NOW/LVGL are up; the default (more-internal)
    // allocation of the ~50 KB NSNET2 model can exhaust it and make its
    // FreeRTOS mutex create() return NULL -> boot assert. esp-sr still keeps the
    // DMA/cache-critical pieces in internal RAM.
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_iface = esp_afe_handle_from_config(cfg);
    if (s_iface == nullptr) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(cfg);
        return false;
    }

    s_afe = s_iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (s_afe == nullptr) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return false;
    }

    s_feed_chunk = s_iface->get_feed_chunksize(s_afe);
    s_fetch_chunk = s_iface->get_fetch_chunksize(s_afe);
    s_feed_channels = static_cast<size_t>(s_iface->get_feed_channel_num(s_afe));
    if (s_feed_channels == 0u) {
        s_feed_channels = use_ref_channel ? 2u : 1u;
    }
    if (s_feed_chunk <= 0) {
        ESP_LOGE(TAG, "invalid feed chunk size");
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        return false;
    }

    s_feed_cap = static_cast<size_t>(s_feed_chunk) + 256u;
    s_feed_buf = static_cast<int16_t *>(
        malloc(s_feed_cap * s_feed_channels * sizeof(int16_t)));
    if (s_feed_buf == nullptr) {
        ESP_LOGE(TAG, "feed buffer alloc failed");
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        return false;
    }

    s_feed_fill = 0;
    s_out_fill = 0;

    // 16k->8k band-limited downsampler for the AFE output (see "Resampler").
    // Speech path: complexity 2 is a middle ground; the low-IRAM variant
    // because internal SRAM is scarce (see the AFE PSRAM note above).
    esp_ae_rate_cvt_cfg_t rate_cfg = {};
    rate_cfg.src_rate = 16000u;
    rate_cfg.dest_rate = 8000u;
    rate_cfg.channel = 1u;
    rate_cfg.bits_per_sample = 16u;
    rate_cfg.complexity = 2u;
    rate_cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY;
    if (esp_ae_rate_cvt_open(&rate_cfg, &s_rate_cvt) != ESP_AE_ERR_OK ||
        esp_ae_rate_cvt_get_max_out_sample_num(s_rate_cvt, 2048u,
                                               &s_down_buf_cap) != ESP_AE_ERR_OK) {
        ESP_LOGE(TAG, "rate converter init failed");
        free(s_feed_buf);
        s_feed_buf = nullptr;
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        return false;
    }
    s_down_buf = static_cast<int16_t *>(
        heap_caps_malloc(s_down_buf_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (s_down_buf == nullptr) {
        ESP_LOGE(TAG, "downsample buffer alloc failed");
        esp_ae_rate_cvt_close(s_rate_cvt);
        s_rate_cvt = nullptr;
        free(s_feed_buf);
        s_feed_buf = nullptr;
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        return false;
    }

    s_running = true;
    // Stack in PSRAM (MALLOC_CAP_SPIRAM) like the passthrough task: with
    // WiFi/BT/ESP-NOW up, internal SRAM is too fragmented for an internal-stack
    // create ("fetch task create failed" -> AEC falls back to raw). The fetch
    // task runs AFE fetch/DSP as a normal RTOS task (never with flash cache
    // disabled), so a PSRAM stack is safe. 32 KB because this task delivers
    // processed mic frames through the router sinks inline, which includes the
    // Opus TX encode (libopus, deep stack) when the Opus codec is enabled.
    s_fetch_task_exited = false;
    if (xTaskCreatePinnedToCoreWithCaps(aec_fetch_task, "aec_fetch", 32768, nullptr, 9,
                                        &s_fetch_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "fetch task create failed");
        s_running = false;
        esp_ae_rate_cvt_close(s_rate_cvt);
        s_rate_cvt = nullptr;
        free(s_down_buf);
        s_down_buf = nullptr;
        free(s_feed_buf);
        s_feed_buf = nullptr;
        s_iface->destroy(s_afe);
        s_afe = nullptr;
        return false;
    }

    s_has_reference = enable_aec;
    s_has_ai_noise = enable_ai_noise;
    s_ready = true;
    AEC_SetRuntimeEnabled(enable_aec, enable_ai_noise);
    return true;
}

// Tear down the pipeline. Models stay loaded so the next afe_create_pipeline()
// avoids the multi-second NSNET2 model reload. Fetch task is unblocked by
// feeding silence directly (bypassing our gated AEC_SubmitCapture); it sees
// s_running=false on the next iteration and exits cleanly.
static void afe_destroy_pipeline(void) {
    if (!s_ready) {
        return;
    }
    // Stop accepting new submits and signal the fetch task to wind down.
    s_ready = false;
    s_running = false;

    // Feed a few silent frames straight into esp-sr so fetch() returns,
    // the loop sees s_running=false, and the task parks for owner deletion. Without
    // this the task could block forever waiting for AFE output.
    if (s_iface != nullptr && s_afe != nullptr && s_feed_buf != nullptr) {
        memset(s_feed_buf, 0, s_feed_cap * s_feed_channels * sizeof(int16_t));
        for (int i = 0; i < 4; ++i) {
            s_iface->feed(s_afe, s_feed_buf);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    // Up to ~500 ms for graceful exit, then last-resort force delete.
    for (int waited = 0; waited < 50 && !s_fetch_task_exited; ++waited) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_fetch_task != nullptr) {
        if (!s_fetch_task_exited) {
            ESP_LOGW(TAG, "fetch task didn't exit cleanly, forcing delete");
        }
        vTaskDeleteWithCaps(s_fetch_task);
        s_fetch_task = nullptr;
    }
    s_fetch_task_exited = false;

    if (s_iface != nullptr && s_afe != nullptr) {
        s_iface->destroy(s_afe);
    }
    s_afe = nullptr;
    s_iface = nullptr;
    if (s_rate_cvt != nullptr) {
        esp_ae_rate_cvt_close(s_rate_cvt);
        s_rate_cvt = nullptr;
    }
    free(s_down_buf);
    s_down_buf = nullptr;
    s_down_buf_cap = 0;
    if (s_feed_buf != nullptr) {
        free(s_feed_buf);
        s_feed_buf = nullptr;
    }
    s_feed_fill = 0;
    s_out_fill = 0;
    s_has_reference = false;
    s_has_ai_noise = false;
    s_runtime_aec_enabled = false;
    s_runtime_ai_noise_enabled = false;
}

bool AEC_Init(const bool enable_aec, const bool enable_ai_noise) {
    if (s_ready) {
        return true;
    }
    if (!enable_aec && !enable_ai_noise) {
        return false;
    }

    s_models = esp_srmodel_init("model");
    if (s_models == nullptr) {
        ESP_LOGE(TAG, "esp_srmodel_init('model') failed -- NSNET2 model partition missing?");
        return false;
    }

    if (!afe_create_pipeline(enable_aec, enable_ai_noise)) {
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "ready: feed_chunk=%d fetch_chunk=%d (16kHz), "
             "capture=16k output=8k, ch=%u aec=%u ai_noise=%u (ns_init=%u)",
             s_feed_chunk,
             s_fetch_chunk,
             static_cast<unsigned>(s_feed_channels),
             s_has_reference ? 1u : 0u,
             s_has_ai_noise ? 1u : 0u,
             enable_ai_noise ? 1u : 0u);
    return true;
}

bool AEC_Reconfigure(const bool enable_aec, const bool enable_ai_noise) {
    // Cold path: nothing is up yet, just do a normal init.
    if (!s_ready) {
        return AEC_Init(enable_aec, enable_ai_noise);
    }
    // No-op if nothing actually changed (the heavy reinit is wasted work).
    if (s_has_reference == enable_aec && s_has_ai_noise == enable_ai_noise) {
        AEC_SetRuntimeEnabled(enable_aec, enable_ai_noise);
        return true;
    }

    ESP_LOGI(TAG, "reconfigure: aec=%u->%u ai_noise=%u->%u (will cut audio briefly)",
             s_has_reference ? 1u : 0u, enable_aec ? 1u : 0u,
             s_has_ai_noise ? 1u : 0u, enable_ai_noise ? 1u : 0u);

    afe_destroy_pipeline();

    // Models stay loaded across reinit. If both are off after the toggle
    // there is no AFE to bring back -- the bypass path in
    // audio_passthrough_task takes over with raw mic.
    if (!enable_aec && !enable_ai_noise) {
        return true;
    }
    if (!afe_create_pipeline(enable_aec, enable_ai_noise)) {
        ESP_LOGE(TAG, "reconfigure failed");
        return false;
    }
    ESP_LOGI(TAG, "reconfigured: aec=%u ai_noise=%u",
             s_has_reference ? 1u : 0u, s_has_ai_noise ? 1u : 0u);
    return true;
}

bool AEC_IsReady(void) {
    return s_ready;
}

void AEC_SetRuntimeEnabled(const bool enable_aec, const bool enable_ai_noise) {
    s_runtime_aec_enabled = s_has_reference && enable_aec;
    s_runtime_ai_noise_enabled = s_has_ai_noise && enable_ai_noise;
}

bool AEC_UsesReference(void) {
    return s_ready && s_has_reference;
}

bool AEC_IsRuntimeAecEnabled(void) {
    return s_ready && s_runtime_aec_enabled;
}

bool AEC_IsRuntimeActive(void) {
    return s_ready && (s_runtime_aec_enabled || s_runtime_ai_noise_enabled);
}

void AEC_SetOutputCallback(AEC_OutputCallback callback, void *user_data) {
    s_out_cb = callback;
    s_out_user = user_data;
}

void AEC_SubmitCapture(const int16_t *mic_16k,
                       const int16_t *ref_16k,
                       size_t sample_count) {
    if (!s_ready || mic_16k == nullptr || sample_count == 0) {
        return;
    }

    if (s_feed_fill + sample_count > s_feed_cap) {
        // Overflow guard: drop to resync rather than corrupt memory.
        s_feed_fill = 0;
        return;
    }

    // Append channel-interleaved: [mic] or [mic, ref]. A null ref_16k while
    // the AFE expects 2 channels is treated as a silent reference (caller
    // intentionally "soft-disabling" AEC).
    const bool zero_ref = (s_feed_channels > 1u && ref_16k == nullptr);
    for (size_t i = 0; i < sample_count; ++i) {
        s_feed_buf[(s_feed_fill + i) * s_feed_channels] = mic_16k[i];
        if (s_feed_channels > 1u) {
            s_feed_buf[(s_feed_fill + i) * s_feed_channels + 1u] =
                zero_ref ? static_cast<int16_t>(0) : ref_16k[i];
        }
    }
    s_feed_fill += sample_count;

    // Feed full chunks to the AFE.
    while (s_feed_fill >= static_cast<size_t>(s_feed_chunk)) {
        s_iface->feed(s_afe, s_feed_buf);
        const size_t remain = s_feed_fill - static_cast<size_t>(s_feed_chunk);
        if (remain > 0) {
            memmove(s_feed_buf,
                    s_feed_buf + static_cast<size_t>(s_feed_chunk) * s_feed_channels,
                    remain * s_feed_channels * sizeof(int16_t));
        }
        s_feed_fill = remain;
    }
}

#else // !NRL_ENABLE_AUDIO_AFE

bool AEC_Init(bool, bool) { return false; }
bool AEC_Reconfigure(bool, bool) { return false; }
bool AEC_IsReady(void) { return false; }
void AEC_SetRuntimeEnabled(bool, bool) {}
bool AEC_UsesReference(void) { return false; }
bool AEC_IsRuntimeActive(void) { return false; }
bool AEC_IsRuntimeAecEnabled(void) { return false; }
void AEC_SetOutputCallback(AEC_OutputCallback, void *) {}
void AEC_SubmitCapture(const int16_t *, const int16_t *, size_t) {}

#endif // NRL_ENABLE_AUDIO_AFE
