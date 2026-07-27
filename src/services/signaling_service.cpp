#include "services/signaling_service.h"

#include "audio/audio_router.h"
#include "lib/ctcss_decoder.h"
#include "lib/cw_codec.h"
#include "lib/dtmf_codec.h"
#include "lib/nrl_psram.h"
#include "services/display_notice.h"
#include "services/cw_service.h"

extern "C" {
#include "mdc_decode.h"
#include "mdc_encode.h"
}

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr const char *TAG = "SIGNAL";
constexpr const char *kNvsNamespace = "signaling";
constexpr uint32_t kSampleRate = 16000u;
constexpr size_t kPcmChunk = 160u; // 10 ms
constexpr size_t kOpusFrameSamples = 320u;
constexpr size_t kDtmfToneSamples = 1600u;
constexpr size_t kDtmfGapSamples = 800u;
constexpr size_t kMdcCacheSamples = 16384u;
constexpr size_t kDtmfCacheSamples = 16u * (kDtmfToneSamples + kDtmfGapSamples);
constexpr uint32_t kDecodeNoticeDurationMs = 8000u;
constexpr double kPi = 3.14159265358979323846;

enum class DecodeSource : uint8_t { Mic, Nrl };
enum class TailDestination : uint8_t { Nrl, Speaker };

struct DecoderContext {
    DecodeSource source;
    mdc_decoder_t *mdc;
    CwAudioDecoder cw;
    DtmfDecoder dtmf;
    CtcssDecoder ctcss;
    char dtmf_result[17];
    char cw_result[49];
    uint32_t last_dtmf_ms;
};

struct PcmCache {
    int16_t *samples;
    size_t count;
};

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
SignalingConfig s_config = {};
DecoderContext s_mic = {DecodeSource::Mic, nullptr, {}, {}, {}, {}, {}, 0u};
DecoderContext s_nrl = {DecodeSource::Nrl, nullptr, {}, {}, {}, {}, {}, 0u};
mdc_encoder_t *s_encoder = nullptr;
QueueHandle_t s_tail_queue = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_cache_mutex = nullptr;
SemaphoreHandle_t s_encoder_mutex = nullptr;
NRL_PSRAM_BSS int16_t s_mdc_pcm_storage[kMdcCacheSamples];
NRL_PSRAM_BSS int16_t s_dtmf_pcm_storage[kDtmfCacheSamples];
PcmCache s_mdc_cache = {s_mdc_pcm_storage, 0u};
PcmCache s_dtmf_cache = {s_dtmf_pcm_storage, 0u};
char s_last_result[96] = {};
uint32_t s_last_result_ms = 0u;
uint32_t s_revision = 0u;
uint32_t s_last_mdc_ms[2] = {};
uint32_t s_last_mdc_signature[2] = {};
volatile bool s_ctcss_mic_enabled = false;

void signalingTask(void *);

const char *sourceName(DecodeSource source) { return source == DecodeSource::Mic ? "MIC" : "NRL"; }

void copyConfig(SignalingConfig *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_config;
    portEXIT_CRITICAL(&s_lock);
}

bool saveConfig()
{
    SignalingConfig cfg{};
    copyConfig(&cfg);
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = ESP_OK;
    auto put8 = [&](const char *key, uint8_t value) {
        if (err == ESP_OK) err = nvs_set_u8(handle, key, value);
    };
    put8("ctcss_rx_mic", cfg.ctcss_rx_mic);
    put8("ctcss_rx_nrl", cfg.ctcss_rx_nrl);
    put8("mdc_rx_mic", cfg.mdc_rx_mic);
    put8("mdc_rx_nrl", cfg.mdc_rx_nrl);
    put8("cw_rx_mic", cfg.cw_rx_mic);
    put8("cw_rx_nrl", cfg.cw_rx_nrl);
    put8("mdc_tx_nrl", cfg.mdc_tx_nrl);
    put8("mdc_tx_spk", cfg.mdc_tx_speaker);
    put8("dtmf_rx_mic", cfg.dtmf_rx_mic);
    put8("dtmf_rx_nrl", cfg.dtmf_rx_nrl);
    put8("dtmf_tx_nrl", cfg.dtmf_tx_nrl);
    put8("dtmf_tx_spk", cfg.dtmf_tx_speaker);
    put8("mdc_op", cfg.mdc_opcode);
    put8("mdc_arg", cfg.mdc_argument);
    if (err == ESP_OK) err = nvs_set_u16(handle, "mdc_id", cfg.mdc_unit_id);
    if (err == ESP_OK) err = nvs_set_str(handle, "dtmf_digits", cfg.dtmf_digits);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void loadConfig()
{
    memset(&s_config, 0, sizeof(s_config));
    s_config.mdc_opcode = 0x01u; // PTT ID
    s_config.mdc_argument = 0x00u;
    s_config.mdc_unit_id = 0x0001u;
    snprintf(s_config.dtmf_digits, sizeof(s_config.dtmf_digits), "1");
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = 0u;
    auto getBool = [&](const char *key, bool *target) {
        if (nvs_get_u8(handle, key, &value) == ESP_OK) *target = value != 0u;
    };
    getBool("ctcss_rx_mic", &s_config.ctcss_rx_mic);
    getBool("ctcss_rx_nrl", &s_config.ctcss_rx_nrl);
    getBool("mdc_rx_mic", &s_config.mdc_rx_mic);
    getBool("mdc_rx_nrl", &s_config.mdc_rx_nrl);
    getBool("cw_rx_mic", &s_config.cw_rx_mic);
    getBool("cw_rx_nrl", &s_config.cw_rx_nrl);
    getBool("mdc_tx_nrl", &s_config.mdc_tx_nrl);
    getBool("mdc_tx_spk", &s_config.mdc_tx_speaker);
    getBool("dtmf_rx_mic", &s_config.dtmf_rx_mic);
    getBool("dtmf_rx_nrl", &s_config.dtmf_rx_nrl);
    getBool("dtmf_tx_nrl", &s_config.dtmf_tx_nrl);
    getBool("dtmf_tx_spk", &s_config.dtmf_tx_speaker);
    (void)nvs_get_u8(handle, "mdc_op", &s_config.mdc_opcode);
    (void)nvs_get_u8(handle, "mdc_arg", &s_config.mdc_argument);
    (void)nvs_get_u16(handle, "mdc_id", &s_config.mdc_unit_id);
    size_t size = sizeof(s_config.dtmf_digits);
    (void)nvs_get_str(handle, "dtmf_digits", s_config.dtmf_digits, &size);
    if (!DTMF_IsValid(s_config.dtmf_digits)) snprintf(s_config.dtmf_digits, sizeof(s_config.dtmf_digits), "1");
    nvs_close(handle);
}

void applyRoutes()
{
    SignalingConfig cfg{};
    copyConfig(&cfg);
    s_ctcss_mic_enabled = cfg.ctcss_rx_mic;
    AudioRouter_SetRoute(AUDIO_SRC_MIC, AUDIO_SINK_SIGNALING,
                         cfg.mdc_rx_mic || cfg.dtmf_rx_mic || cfg.cw_rx_mic);
    AudioRouter_SetRoute(AUDIO_SRC_NRL_DOWNLINK, AUDIO_SINK_SIGNALING,
                         cfg.mdc_rx_nrl || cfg.dtmf_rx_nrl || cfg.ctcss_rx_nrl || cfg.cw_rx_nrl);
    AudioRouter_SetRoute(AUDIO_SRC_MDC_NRL, AUDIO_SINK_NRL_UPLINK, cfg.mdc_tx_nrl);
    AudioRouter_SetRoute(AUDIO_SRC_MDC_SPEAKER, AUDIO_SINK_SPEAKER, cfg.mdc_tx_speaker);
    AudioRouter_SetRoute(AUDIO_SRC_DTMF_NRL, AUDIO_SINK_NRL_UPLINK, cfg.dtmf_tx_nrl);
    AudioRouter_SetRoute(AUDIO_SRC_DTMF_SPEAKER, AUDIO_SINK_SPEAKER, cfg.dtmf_tx_speaker);
}

void publishResult(const char *text)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(s_last_result, sizeof(s_last_result), "%s", text);
    s_last_result_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    ++s_revision;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s", text);
}

void mdcDecoded(int frame_count, unsigned char op, unsigned char arg,
                unsigned short unit_id, unsigned char, unsigned char,
                unsigned char, unsigned char, void *context)
{
    auto *decoder = static_cast<DecoderContext *>(context);
    const size_t index = decoder->source == DecodeSource::Mic ? 0u : 1u;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    const uint32_t signature = (static_cast<uint32_t>(op) << 24u) |
                               (static_cast<uint32_t>(arg) << 16u) | unit_id;
    if (s_last_mdc_signature[index] == signature && now - s_last_mdc_ms[index] < 500u) return;
    s_last_mdc_signature[index] = signature;
    s_last_mdc_ms[index] = now;
    char text[96];
    snprintf(text, sizeof(text), "MDC %s: ID %04X OP %02X ARG %02X%s",
             sourceName(decoder->source), unit_id, op, arg, frame_count == 2 ? " (2)" : "");
    publishResult(text);
}

void dtmfDecoded(char digit, void *context)
{
    auto *decoder = static_cast<DecoderContext *>(context);
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (now - decoder->last_dtmf_ms > 1200u) decoder->dtmf_result[0] = '\0';
    decoder->last_dtmf_ms = now;
    size_t length = strlen(decoder->dtmf_result);
    if (length >= sizeof(decoder->dtmf_result) - 1u) {
        memmove(decoder->dtmf_result, decoder->dtmf_result + 1u, length - 1u);
        length = sizeof(decoder->dtmf_result) - 2u;
    }
    decoder->dtmf_result[length] = digit;
    decoder->dtmf_result[length + 1u] = '\0';
    char text[96];
    snprintf(text, sizeof(text), "DTMF %s: %s", sourceName(decoder->source), decoder->dtmf_result);
    publishResult(text);
}

void cwDecoded(const char character, const char *pattern, const uint16_t wpm, void *context)
{
    auto *decoder = static_cast<DecoderContext *>(context);
    size_t length = strlen(decoder->cw_result);
    if (length >= sizeof(decoder->cw_result) - 1u) {
        memmove(decoder->cw_result, decoder->cw_result + 1u, length - 1u);
        length = sizeof(decoder->cw_result) - 2u;
    }
    decoder->cw_result[length] = character;
    decoder->cw_result[length + 1u] = '\0';
    CW_SERVICE_RecordReceived(decoder->source == DecodeSource::Mic ? CW_SOURCE_MIC : CW_SOURCE_NRL,
                              character, pattern, wpm);
    char text[96];
    snprintf(text, sizeof(text), "CW %s: %s  %s %uWPM", sourceName(decoder->source),
             decoder->cw_result, pattern != nullptr ? pattern : "?",
             static_cast<unsigned>(wpm));
    publishResult(text);
}

void ctcssDecoded(const float frequency_hz, void *context)
{
    auto *decoder = static_cast<DecoderContext *>(context);
    char text[96];
    snprintf(text, sizeof(text), "CTCSS %s: %.1f Hz",
             sourceName(decoder->source), static_cast<double>(frequency_hz));
    publishResult(text);
}

void signalingSink(uint8_t source_id, const int16_t *samples, size_t count, void *)
{
    DecoderContext *decoder = source_id == AUDIO_SRC_MIC ? &s_mic : &s_nrl;
    SignalingConfig cfg{};
    copyConfig(&cfg);
    const bool mdc_enabled = decoder->source == DecodeSource::Mic ? cfg.mdc_rx_mic : cfg.mdc_rx_nrl;
    const bool dtmf_enabled = decoder->source == DecodeSource::Mic ? cfg.dtmf_rx_mic : cfg.dtmf_rx_nrl;
    const bool cw_enabled = decoder->source == DecodeSource::Mic ? cfg.cw_rx_mic : cfg.cw_rx_nrl;
    const bool ctcss_enabled = decoder->source == DecodeSource::Mic ? false : cfg.ctcss_rx_nrl;
    if (mdc_enabled && decoder->mdc != nullptr) {
        mdc_decoder_process_samples(decoder->mdc, const_cast<mdc_sample_t *>(samples), static_cast<int>(count));
    }
    if (dtmf_enabled) decoder->dtmf.feed(samples, count, dtmfDecoded, decoder);
    if (cw_enabled) decoder->cw.feed(samples, count, cwDecoded, decoder);
    if (ctcss_enabled) decoder->ctcss.feed(samples, count, ctcssDecoded, decoder);
}

void pushPaced(uint8_t source, const int16_t *samples, size_t count)
{
    size_t offset = 0u;
    while (offset < count) {
        const size_t take = (count - offset < kPcmChunk) ? count - offset : kPcmChunk;
        AudioRouter_PushFrame(source, kSampleRate, samples + offset, take);
        offset += take;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

size_t paddedSampleCount(size_t count)
{
    return ((count + kOpusFrameSamples - 1u) / kOpusFrameSamples) * kOpusFrameSamples;
}

bool ensureCacheMutex()
{
    if (s_cache_mutex != nullptr) return true;
    s_cache_mutex = xSemaphoreCreateMutex();
    return s_cache_mutex != nullptr;
}

bool ensureMdcEncoder()
{
    if (s_encoder_mutex == nullptr) s_encoder_mutex = xSemaphoreCreateMutex();
    if (s_encoder_mutex == nullptr) return false;
    if (s_encoder == nullptr) s_encoder = mdc_encoder_new(kSampleRate);
    return s_encoder != nullptr;
}

bool ensureMdcDecoder(DecoderContext *decoder)
{
    if (decoder == nullptr) return false;
    if (decoder->mdc != nullptr) return true;
    decoder->mdc = mdc_decoder_new(kSampleRate);
    if (decoder->mdc == nullptr) return false;
    mdc_decoder_set_callback(decoder->mdc, mdcDecoded, decoder);
    return true;
}

bool buildMdcCache(const SignalingConfig &cfg, PcmCache *result)
{
    if (result == nullptr || !ensureMdcEncoder()) return false;
    result->samples = nullptr;
    result->count = 0u;
    xSemaphoreTake(s_encoder_mutex, portMAX_DELAY);
    if (mdc_encoder_set_packet(s_encoder, cfg.mdc_opcode, cfg.mdc_argument, cfg.mdc_unit_id) != 0) {
        xSemaphoreGive(s_encoder_mutex);
        return false;
    }
    int16_t pcm[kPcmChunk];
    size_t raw_count = 0u;
    for (;;) {
        const int count = mdc_encoder_get_samples(s_encoder, pcm, static_cast<int>(kPcmChunk));
        if (count <= 0) break;
        raw_count += static_cast<size_t>(count);
    }
    const size_t count = paddedSampleCount(raw_count);
    if (count > kMdcCacheSamples ||
        mdc_encoder_set_packet(s_encoder, cfg.mdc_opcode, cfg.mdc_argument, cfg.mdc_unit_id) != 0) {
        xSemaphoreGive(s_encoder_mutex);
        return false;
    }
    memset(s_mdc_pcm_storage, 0, count * sizeof(s_mdc_pcm_storage[0]));
    size_t offset = 0u;
    while (offset < raw_count) {
        const size_t request = (raw_count - offset < kPcmChunk) ? raw_count - offset : kPcmChunk;
        const int generated = mdc_encoder_get_samples(s_encoder, s_mdc_pcm_storage + offset,
                                                      static_cast<int>(request));
        if (generated <= 0) break;
        offset += static_cast<size_t>(generated);
    }
    xSemaphoreGive(s_encoder_mutex);
    if (offset != raw_count) return false;
    result->samples = s_mdc_pcm_storage;
    result->count = count;
    return true;
}

bool buildDtmfCache(const SignalingConfig &cfg, PcmCache *result)
{
    if (result == nullptr || !DTMF_IsValid(cfg.dtmf_digits)) return false;
    result->samples = nullptr;
    result->count = 0u;
    const size_t raw_count = strlen(cfg.dtmf_digits) * (kDtmfToneSamples + kDtmfGapSamples);
    const size_t count = paddedSampleCount(raw_count);
    if (count > kDtmfCacheSamples) return false;
    memset(s_dtmf_pcm_storage, 0, count * sizeof(s_dtmf_pcm_storage[0]));
    size_t offset = 0u;
    for (const char *p = cfg.dtmf_digits; *p != '\0'; ++p) {
        uint16_t low = 0u, high = 0u;
        if (!DTMF_Frequencies(*p, &low, &high)) return false;
        double phase_low = 0.0, phase_high = 0.0;
        for (size_t i = 0u; i < kDtmfToneSamples; ++i) {
            s_dtmf_pcm_storage[offset++] =
                static_cast<int16_t>((sin(phase_low) + sin(phase_high)) * 5500.0);
            phase_low += 2.0 * kPi * low / kSampleRate;
            phase_high += 2.0 * kPi * high / kSampleRate;
        }
        offset += kDtmfGapSamples; // calloc already supplied the inter-digit silence.
    }
    result->samples = s_dtmf_pcm_storage;
    result->count = count;
    return true;
}

void clearCache(PcmCache *target)
{
    if (target == nullptr) return;
    if (s_cache_mutex == nullptr) {
        target->count = 0u;
        return;
    }
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    target->count = 0u;
    xSemaphoreGive(s_cache_mutex);
}

bool rebuildMdcCache(const SignalingConfig &cfg)
{
    if (!ensureCacheMutex()) return false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    PcmCache replacement{};
    if (!buildMdcCache(cfg, &replacement)) {
        xSemaphoreGive(s_cache_mutex);
        return false;
    }
    s_mdc_cache.count = replacement.count;
    xSemaphoreGive(s_cache_mutex);
    ESP_LOGI(TAG, "MDC PCM cached in PSRAM: %u samples (%u bytes)",
             static_cast<unsigned>(replacement.count),
             static_cast<unsigned>(replacement.count * sizeof(int16_t)));
    return true;
}

bool rebuildDtmfCache(const SignalingConfig &cfg)
{
    if (!ensureCacheMutex()) return false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    PcmCache replacement{};
    if (!buildDtmfCache(cfg, &replacement)) {
        xSemaphoreGive(s_cache_mutex);
        return false;
    }
    s_dtmf_cache.count = replacement.count;
    xSemaphoreGive(s_cache_mutex);
    ESP_LOGI(TAG, "DTMF PCM cached in PSRAM: %u samples (%u bytes)",
             static_cast<unsigned>(replacement.count),
             static_cast<unsigned>(replacement.count * sizeof(int16_t)));
    return true;
}

bool ensureTxCache(bool mdc, const SignalingConfig &cfg)
{
    PcmCache cache{};
    if (s_cache_mutex != nullptr) {
        xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
        cache = mdc ? s_mdc_cache : s_dtmf_cache;
        xSemaphoreGive(s_cache_mutex);
    } else {
        cache = mdc ? s_mdc_cache : s_dtmf_cache;
    }
    if (cache.count != 0u) return true;
    return mdc ? rebuildMdcCache(cfg) : rebuildDtmfCache(cfg);
}

void sendCached(TailDestination destination, bool mdc)
{
    if (s_cache_mutex == nullptr) return;
    const uint8_t source = mdc
        ? (destination == TailDestination::Nrl ? AUDIO_SRC_MDC_NRL : AUDIO_SRC_MDC_SPEAKER)
        : (destination == TailDestination::Nrl ? AUDIO_SRC_DTMF_NRL : AUDIO_SRC_DTMF_SPEAKER);
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    const PcmCache &cache = mdc ? s_mdc_cache : s_dtmf_cache;
    if (cache.samples != nullptr && cache.count != 0u) pushPaced(source, cache.samples, cache.count);
    xSemaphoreGive(s_cache_mutex);
}

void signalingTask(void *)
{
    TailDestination destination;
    while (true) {
        if (xQueueReceive(s_tail_queue, &destination, portMAX_DELAY) != pdTRUE) continue;
        SignalingConfig cfg{};
        copyConfig(&cfg);
        const bool mdc = destination == TailDestination::Nrl ? cfg.mdc_tx_nrl : cfg.mdc_tx_speaker;
        const bool dtmf = destination == TailDestination::Nrl ? cfg.dtmf_tx_nrl : cfg.dtmf_tx_speaker;
        if (mdc) sendCached(destination, true);
        if (mdc && dtmf) vTaskDelay(pdMS_TO_TICKS(50));
        if (dtmf) sendCached(destination, false);
    }
}

bool ensureTailWorker()
{
    if (s_task != nullptr) return true;
    if (s_tail_queue == nullptr) s_tail_queue = xQueueCreate(4u, sizeof(TailDestination));
    if (s_tail_queue == nullptr) return false;
    if (xTaskCreatePinnedToCoreWithCaps(signalingTask, "signaling", 4096u,
                                        nullptr, 5u, &s_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = nullptr;
        return false;
    }
    return true;
}

void releaseMdcEncoderIfUnused(const SignalingConfig &cfg)
{
    if (cfg.mdc_tx_nrl || cfg.mdc_tx_speaker || s_mdc_cache.count != 0u) return;
    if (s_encoder != nullptr) {
        free(s_encoder);
        s_encoder = nullptr;
    }
    if (s_encoder_mutex != nullptr) {
        vSemaphoreDelete(s_encoder_mutex);
        s_encoder_mutex = nullptr;
    }
}

void releaseIdleTxLocks()
{
    if (s_task != nullptr || s_mdc_cache.count != 0u || s_dtmf_cache.count != 0u) return;
    if (s_cache_mutex != nullptr) {
        vSemaphoreDelete(s_cache_mutex);
        s_cache_mutex = nullptr;
    }
}

bool setRouteValue(bool mdc, SignalingRoute route, bool enabled)
{
    SignalingConfig updated{};
    copyConfig(&updated);
    bool *pending = nullptr;
    if (mdc) {
        if (route == SIGNAL_ROUTE_RX_MIC) pending = &updated.mdc_rx_mic;
        else if (route == SIGNAL_ROUTE_RX_NRL) pending = &updated.mdc_rx_nrl;
        else if (route == SIGNAL_ROUTE_TX_NRL) pending = &updated.mdc_tx_nrl;
        else if (route == SIGNAL_ROUTE_TX_SPEAKER) pending = &updated.mdc_tx_speaker;
    } else {
        if (route == SIGNAL_ROUTE_RX_MIC) pending = &updated.dtmf_rx_mic;
        else if (route == SIGNAL_ROUTE_RX_NRL) pending = &updated.dtmf_rx_nrl;
        else if (route == SIGNAL_ROUTE_TX_NRL) pending = &updated.dtmf_tx_nrl;
        else if (route == SIGNAL_ROUTE_TX_SPEAKER) pending = &updated.dtmf_tx_speaker;
    }
    if (pending == nullptr) return false;
    *pending = enabled;
    if (enabled && route == SIGNAL_ROUTE_RX_MIC && mdc && !ensureMdcDecoder(&s_mic)) {
        ESP_LOGE(TAG, "MDC MIC RX route rejected: decoder allocation failed");
        return false;
    }
    if (enabled && route == SIGNAL_ROUTE_RX_NRL && mdc && !ensureMdcDecoder(&s_nrl)) {
        ESP_LOGE(TAG, "MDC NRL RX route rejected: decoder allocation failed");
        return false;
    }
    if (enabled && (route == SIGNAL_ROUTE_TX_NRL || route == SIGNAL_ROUTE_TX_SPEAKER) &&
        !ensureTxCache(mdc, updated)) {
        ESP_LOGE(TAG, "%s TX route rejected: PCM cache allocation failed", mdc ? "MDC" : "DTMF");
        return false;
    }
    if (enabled && (route == SIGNAL_ROUTE_TX_NRL || route == SIGNAL_ROUTE_TX_SPEAKER) &&
        !ensureTailWorker()) {
        ESP_LOGE(TAG, "%s TX route rejected: worker allocation failed", mdc ? "MDC" : "DTMF");
        SignalingConfig current{};
        copyConfig(&current);
        if (!current.mdc_tx_nrl && !current.mdc_tx_speaker) clearCache(&s_mdc_cache);
        if (!current.dtmf_tx_nrl && !current.dtmf_tx_speaker) clearCache(&s_dtmf_cache);
        releaseMdcEncoderIfUnused(current);
        if (s_task == nullptr && s_tail_queue != nullptr) {
            vQueueDelete(s_tail_queue);
            s_tail_queue = nullptr;
        }
        releaseIdleTxLocks();
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    bool *target = nullptr;
    if (mdc) {
        if (route == SIGNAL_ROUTE_RX_MIC) target = &s_config.mdc_rx_mic;
        else if (route == SIGNAL_ROUTE_RX_NRL) target = &s_config.mdc_rx_nrl;
        else if (route == SIGNAL_ROUTE_TX_NRL) target = &s_config.mdc_tx_nrl;
        else if (route == SIGNAL_ROUTE_TX_SPEAKER) target = &s_config.mdc_tx_speaker;
    } else {
        if (route == SIGNAL_ROUTE_RX_MIC) target = &s_config.dtmf_rx_mic;
        else if (route == SIGNAL_ROUTE_RX_NRL) target = &s_config.dtmf_rx_nrl;
        else if (route == SIGNAL_ROUTE_TX_NRL) target = &s_config.dtmf_tx_nrl;
        else if (route == SIGNAL_ROUTE_TX_SPEAKER) target = &s_config.dtmf_tx_speaker;
    }
    if (target != nullptr) *target = enabled;
    portEXIT_CRITICAL(&s_lock);
    if (target == nullptr) return false;
    SignalingConfig cfg_after{};
    copyConfig(&cfg_after);
    if (mdc && !cfg_after.mdc_tx_nrl && !cfg_after.mdc_tx_speaker) clearCache(&s_mdc_cache);
    if (!mdc && !cfg_after.dtmf_tx_nrl && !cfg_after.dtmf_tx_speaker) clearCache(&s_dtmf_cache);
    releaseMdcEncoderIfUnused(cfg_after);
    applyRoutes();
    return saveConfig();
}

bool setCtcssRouteValue(const SignalingRoute route, const bool enabled)
{
    if (route != SIGNAL_ROUTE_RX_MIC && route != SIGNAL_ROUTE_RX_NRL) return false;
    portENTER_CRITICAL(&s_lock);
    if (route == SIGNAL_ROUTE_RX_MIC) s_config.ctcss_rx_mic = enabled;
    else s_config.ctcss_rx_nrl = enabled;
    portEXIT_CRITICAL(&s_lock);
    applyRoutes();
    return saveConfig();
}

bool setCwRouteValue(const SignalingRoute route, const bool enabled)
{
    if (route != SIGNAL_ROUTE_RX_MIC && route != SIGNAL_ROUTE_RX_NRL) return false;
    portENTER_CRITICAL(&s_lock);
    if (route == SIGNAL_ROUTE_RX_MIC) s_config.cw_rx_mic = enabled;
    else s_config.cw_rx_nrl = enabled;
    portEXIT_CRITICAL(&s_lock);
    applyRoutes();
    return saveConfig();
}

} // namespace

void SIGNALING_Init(void)
{
    loadConfig();
    CW_SERVICE_Init();
    bool config_changed = false;
    if (s_config.mdc_rx_mic && !ensureMdcDecoder(&s_mic)) {
        ESP_LOGE(TAG, "failed to allocate MDC MIC decoder, disabling route");
        s_config.mdc_rx_mic = false;
        config_changed = true;
    }
    if (s_config.mdc_rx_nrl && !ensureMdcDecoder(&s_nrl)) {
        ESP_LOGE(TAG, "failed to allocate MDC NRL decoder, disabling route");
        s_config.mdc_rx_nrl = false;
        config_changed = true;
    }
    if ((s_config.mdc_tx_nrl || s_config.mdc_tx_speaker) && !rebuildMdcCache(s_config)) {
        ESP_LOGE(TAG, "failed to build MDC PCM cache in PSRAM, disabling MDC TX routes");
        s_config.mdc_tx_nrl = false;
        s_config.mdc_tx_speaker = false;
        config_changed = true;
    }
    if ((s_config.dtmf_tx_nrl || s_config.dtmf_tx_speaker) && !rebuildDtmfCache(s_config)) {
        ESP_LOGE(TAG, "failed to build DTMF PCM cache in PSRAM, disabling DTMF TX routes");
        s_config.dtmf_tx_nrl = false;
        s_config.dtmf_tx_speaker = false;
        config_changed = true;
    }
    if ((s_config.mdc_tx_nrl || s_config.mdc_tx_speaker ||
         s_config.dtmf_tx_nrl || s_config.dtmf_tx_speaker) && !ensureTailWorker()) {
        ESP_LOGE(TAG, "failed to create signaling TX worker, disabling TX routes");
        s_config.mdc_tx_nrl = false;
        s_config.mdc_tx_speaker = false;
        s_config.dtmf_tx_nrl = false;
        s_config.dtmf_tx_speaker = false;
        clearCache(&s_mdc_cache);
        clearCache(&s_dtmf_cache);
        config_changed = true;
    }
    releaseMdcEncoderIfUnused(s_config);
    releaseIdleTxLocks();
    AudioRouter_RegisterSink(AUDIO_SINK_SIGNALING, kSampleRate, signalingSink, nullptr);
    applyRoutes();
    // Do not write NVS on this low-memory boot path. The runtime routes are
    // safely disabled; a later explicit setting change persists normally.
    if (config_changed) ESP_LOGW(TAG, "one or more signaling routes disabled for this boot");
    ESP_LOGI(TAG, "ready mdc_rx=%d/%d mdc_tx=%d/%d dtmf=%d/%d ctcss=%d/%d cw=%d/%d",
             s_config.mdc_rx_mic, s_config.mdc_rx_nrl,
             s_config.mdc_tx_nrl, s_config.mdc_tx_speaker,
             s_config.dtmf_rx_mic, s_config.dtmf_rx_nrl,
             s_config.ctcss_rx_mic, s_config.ctcss_rx_nrl,
             s_config.cw_rx_mic, s_config.cw_rx_nrl);
}

void SIGNALING_GetConfig(SignalingConfig *out) { if (out != nullptr) copyConfig(out); }
bool SIGNALING_SetMdcRoute(SignalingRoute route, bool enabled) { return setRouteValue(true, route, enabled); }
bool SIGNALING_SetCwRoute(SignalingRoute route, bool enabled) { return setCwRouteValue(route, enabled); }
bool SIGNALING_SetDtmfRoute(SignalingRoute route, bool enabled) { return setRouteValue(false, route, enabled); }
bool SIGNALING_SetCtcssRoute(SignalingRoute route, bool enabled) { return setCtcssRouteValue(route, enabled); }

bool SIGNALING_SetMdcPacket(uint8_t opcode, uint8_t argument, uint16_t unit_id)
{
    SignalingConfig updated{};
    copyConfig(&updated);
    updated.mdc_opcode = opcode;
    updated.mdc_argument = argument;
    updated.mdc_unit_id = unit_id;
    if ((updated.mdc_tx_nrl || updated.mdc_tx_speaker) && !rebuildMdcCache(updated)) {
        ESP_LOGE(TAG, "MDC config rejected: PSRAM cache allocation failed");
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    s_config.mdc_opcode = updated.mdc_opcode;
    s_config.mdc_argument = updated.mdc_argument;
    s_config.mdc_unit_id = updated.mdc_unit_id;
    portEXIT_CRITICAL(&s_lock);
    return saveConfig();
}

bool SIGNALING_SetDtmfDigits(const char *digits)
{
    if (!DTMF_IsValid(digits)) return false;
    SignalingConfig updated{};
    copyConfig(&updated);
    snprintf(updated.dtmf_digits, sizeof(updated.dtmf_digits), "%s", digits);
    if ((updated.dtmf_tx_nrl || updated.dtmf_tx_speaker) && !rebuildDtmfCache(updated)) {
        ESP_LOGE(TAG, "DTMF config rejected: PSRAM cache allocation failed");
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    snprintf(s_config.dtmf_digits, sizeof(s_config.dtmf_digits), "%s", updated.dtmf_digits);
    portEXIT_CRITICAL(&s_lock);
    return saveConfig();
}

void SIGNALING_OnLocalPttReleased(void)
{
    if (s_tail_queue == nullptr) return;
    const TailDestination destination = TailDestination::Nrl;
    (void)xQueueSend(s_tail_queue, &destination, 0u);
}

void SIGNALING_OnNetworkVoiceEnded(void)
{
    if (s_tail_queue == nullptr) return;
    const TailDestination destination = TailDestination::Speaker;
    (void)xQueueSend(s_tail_queue, &destination, 0u);
}

void SIGNALING_FeedRawMic(const int16_t *samples, const size_t sample_count)
{
    if (!s_ctcss_mic_enabled || samples == nullptr || sample_count == 0u) return;
    s_mic.ctcss.feed(samples, sample_count, ctcssDecoded, &s_mic);
}

uint32_t SIGNALING_GetRevision(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t revision = s_revision;
    portEXIT_CRITICAL(&s_lock);
    return revision;
}

void SIGNALING_GetLastResult(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u) return;
    uint32_t posted_ms = 0u;
    portENTER_CRITICAL(&s_lock);
    posted_ms = s_last_result_ms;
    snprintf(out, out_size, "%s", s_last_result);
    portEXIT_CRITICAL(&s_lock);
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (out[0] != '\0' && now - posted_ms >= kDecodeNoticeDurationMs) out[0] = '\0';
}
