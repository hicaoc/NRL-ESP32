#include "services/cw_service.h"

#include "audio/audio_router.h"
#include "lib/cw_codec.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *TAG = "CW";
constexpr uint32_t kSampleRate = 16000u;
constexpr uint16_t kDefaultWpm = 15u;
constexpr uint16_t kToneHz = 700u;
constexpr size_t kChunkSamples = 160u;
constexpr uint32_t kTxTaskStackBytes = 8192u;
constexpr char kPracticeCharacters[] = "ETIANMSURWDKGOHVF?L?PJBXCYZQ1234567890";

enum class CommandType : uint8_t { Element, Send };
struct Command { CommandType type; CwElement element; };

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
CwSnapshot s_snapshot = {};
QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
uint32_t s_last_element_ms = 0u;
uint16_t s_practice_correct = 0u;
size_t s_practice_index = 0u;
// Keep audio and message buffers out of the worker stack. AudioRouter performs
// rate conversion synchronously and has its own scratch/callback stack usage,
// so stacking another two PCM arrays here left too little headroom on ESP32-P4.
int16_t s_tone_pcm[kChunkSamples] = {};
int16_t s_silence_pcm[kChunkSamples] = {};
char s_send_text[sizeof(s_snapshot.tx_text)] = {};

void finishCharacterLocked();

void appendBounded(char *text, const size_t capacity, const char *suffix)
{
    if (text == nullptr || suffix == nullptr || capacity == 0u) return;
    size_t used = strlen(text);
    const size_t add = strlen(suffix);
    if (used + add >= capacity) {
        const size_t remove = used + add - capacity + 1u;
        if (remove < used) {
            memmove(text, text + remove, used - remove + 1u);
            used -= remove;
        } else {
            text[0] = '\0';
            used = 0u;
        }
    }
    strncat(text, suffix, capacity - used - 1u);
}

void appendLearningCell(char *letters, char *code, const size_t capacity,
                        const char character, const char *pattern)
{
    if (letters == nullptr || code == nullptr || pattern == nullptr || capacity < 3u) return;
    const size_t cell_width = strlen(pattern) + 1u;
    size_t used = strlen(code);
    if (used + cell_width >= capacity) {
        letters[0] = '\0';
        code[0] = '\0';
        used = 0u;
    }
    letters[used] = character;
    for (size_t i = 1u; i < cell_width; ++i) letters[used + i] = ' ';
    letters[used + cell_width] = '\0';
    strncat(code, pattern, capacity - strlen(code) - 1u);
    strncat(code, " ", capacity - strlen(code) - 1u);
}

void rebuildTxLearningRows()
{
    s_snapshot.tx_letters[0] = '\0';
    s_snapshot.tx_code[0] = '\0';
    for (const char *p = s_snapshot.tx_text; *p != '\0'; ++p) {
        const char *pattern = CW_EncodeCharacter(*p);
        if (pattern != nullptr) {
            appendLearningCell(s_snapshot.tx_letters, s_snapshot.tx_code,
                               sizeof(s_snapshot.tx_code), *p, pattern);
        }
    }
}

void sendTone(const uint16_t duration_ms)
{
    const size_t total = static_cast<size_t>(duration_ms) * kSampleRate / 1000u;
    for (size_t sent = 0u; sent < total; sent += kChunkSamples) {
        const size_t count = total - sent < kChunkSamples ? total - sent : kChunkSamples;
        AudioRouter_PushFrame(AUDIO_SRC_CW_NRL, kSampleRate, s_tone_pcm, count);
        AudioRouter_PushFrame(AUDIO_SRC_CW_SPEAKER, kSampleRate, s_tone_pcm, count);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void sendSilence(const uint16_t duration_ms)
{
    const size_t total = static_cast<size_t>(duration_ms) * kSampleRate / 1000u;
    for (size_t sent = 0u; sent < total; sent += kChunkSamples) {
        const size_t count = total - sent < kChunkSamples ? total - sent : kChunkSamples;
        AudioRouter_PushFrame(AUDIO_SRC_CW_NRL, kSampleRate, s_silence_pcm, count);
        AudioRouter_PushFrame(AUDIO_SRC_CW_SPEAKER, kSampleRate, s_silence_pcm, count);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void playElement(const CwElement element, const uint16_t dit_ms)
{
    sendTone(element == CW_ELEMENT_DAH ? static_cast<uint16_t>(dit_ms * 3u) : dit_ms);
    sendSilence(dit_ms);
}

void playText(const char *text, const uint16_t wpm)
{
    const uint16_t dit_ms = wpm > 0u ? static_cast<uint16_t>(1200u / wpm) : 80u;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p == ' ') {
            sendSilence(static_cast<uint16_t>(dit_ms * 4u));
            continue;
        }
        const char *pattern = CW_EncodeCharacter(*p);
        if (pattern == nullptr) continue;
        for (const char *element = pattern; *element != '\0'; ++element) {
            playElement(*element == '-' ? CW_ELEMENT_DAH : CW_ELEMENT_DIT, dit_ms);
        }
        sendSilence(static_cast<uint16_t>(dit_ms * 2u));
    }
}

void worker(void *)
{
    Command command{};
    while (true) {
        if (xQueueReceive(s_queue, &command, pdMS_TO_TICKS(20)) != pdTRUE) {
            const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
            portENTER_CRITICAL(&s_lock);
            const uint16_t wpm = s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm;
            if (s_snapshot.current_pattern[0] != '\0' && s_last_element_ms != 0u &&
                now - s_last_element_ms >= static_cast<uint32_t>(3600u / wpm)) {
                finishCharacterLocked();
            }
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        if (command.type == CommandType::Element) {
            uint16_t wpm = kDefaultWpm;
            portENTER_CRITICAL(&s_lock);
            wpm = s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm;
            portEXIT_CRITICAL(&s_lock);
            playElement(command.element, static_cast<uint16_t>(1200u / wpm));
        } else {
            uint16_t wpm = kDefaultWpm;
            portENTER_CRITICAL(&s_lock);
            snprintf(s_send_text, sizeof(s_send_text), "%s", s_snapshot.tx_text);
            wpm = s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm;
            s_snapshot.sending = true;
            ++s_snapshot.revision;
            portEXIT_CRITICAL(&s_lock);
            playText(s_send_text, wpm);
            portENTER_CRITICAL(&s_lock);
            s_snapshot.sending = false;
            ++s_snapshot.revision;
            portEXIT_CRITICAL(&s_lock);
        }
        const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
        if (stack_free < 1024u) {
            ESP_LOGW(TAG, "cw_tx low stack watermark: %u bytes", static_cast<unsigned>(stack_free));
        }
    }
}

void finishCharacterLocked()
{
    if (s_snapshot.current_pattern[0] == '\0') return;
    const char decoded = CW_DecodePattern(s_snapshot.current_pattern);
    char character[2] = {decoded, '\0'};
    appendBounded(s_snapshot.tx_text, sizeof(s_snapshot.tx_text), character);
    appendLearningCell(s_snapshot.tx_letters, s_snapshot.tx_code,
                       sizeof(s_snapshot.tx_code), decoded, s_snapshot.current_pattern);
    if (s_snapshot.practice_enabled) {
        ++s_snapshot.practice_attempts;
        if (decoded == s_snapshot.practice_target) ++s_practice_correct;
        s_snapshot.accuracy_percent = static_cast<uint8_t>(
            (static_cast<uint32_t>(s_practice_correct) * 100u) / s_snapshot.practice_attempts);
        do {
            s_practice_index = (s_practice_index + 1u) % (sizeof(kPracticeCharacters) - 1u);
            s_snapshot.practice_target = kPracticeCharacters[s_practice_index];
        } while (s_snapshot.practice_target == '?');
    }
    s_snapshot.current_pattern[0] = '\0';
    ++s_snapshot.revision;
}

} // namespace

void CW_SERVICE_Init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.wpm = kDefaultWpm;
    s_snapshot.accuracy_percent = 100u;
    s_snapshot.practice_target = 'E';
    portEXIT_CRITICAL(&s_lock);
    // A 10 ms block at 700 Hz contains exactly seven cycles, so it can be
    // repeated without a phase discontinuity and without calling sinf in the
    // real-time transmit loop.
    for (size_t i = 0u; i < kChunkSamples; ++i) {
        const float phase = 2.0f * 3.14159265358979323846f *
                            static_cast<float>(kToneHz) * static_cast<float>(i) /
                            static_cast<float>(kSampleRate);
        s_tone_pcm[i] = static_cast<int16_t>(sinf(phase) * 7000.0f);
    }
    AudioRouter_SetRoute(AUDIO_SRC_CW_NRL, AUDIO_SINK_NRL_UPLINK, true);
    AudioRouter_SetRoute(AUDIO_SRC_CW_SPEAKER, AUDIO_SINK_SPEAKER, true);
    s_queue = xQueueCreate(12u, sizeof(Command));
    if (s_queue == nullptr ||
        xTaskCreate(worker, "cw_tx", kTxTaskStackBytes, nullptr, 5u, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "CW transmit worker unavailable");
        s_task = nullptr;
    }
}

void CW_SERVICE_RecordReceived(const CwSource, const char character,
                               const char *pattern, const uint16_t wpm)
{
    char token[16];
    snprintf(token, sizeof(token), "%c", character);
    portENTER_CRITICAL(&s_lock);
    appendBounded(s_snapshot.rx_text, sizeof(s_snapshot.rx_text), token);
    appendLearningCell(s_snapshot.rx_letters, s_snapshot.rx_code,
                       sizeof(s_snapshot.rx_code), character,
                       pattern != nullptr ? pattern : "?");
    if (wpm >= 4u && wpm <= 60u) s_snapshot.wpm = wpm;
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_InputElement(const CwElement element)
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_lock);
    if (s_last_element_ms != 0u && now - s_last_element_ms >
        static_cast<uint32_t>(3600u / (s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm))) {
        finishCharacterLocked();
    }
    const size_t length = strlen(s_snapshot.current_pattern);
    if (length < sizeof(s_snapshot.current_pattern) - 1u) {
        s_snapshot.current_pattern[length] = element == CW_ELEMENT_DAH ? '-' : '.';
        s_snapshot.current_pattern[length + 1u] = '\0';
        ++s_snapshot.revision;
    }
    s_last_element_ms = now;
    portEXIT_CRITICAL(&s_lock);
    if (s_queue != nullptr) {
        const Command command{CommandType::Element, element};
        (void)xQueueSend(s_queue, &command, 0u);
    }
}

void CW_SERVICE_FinishCharacter(void)
{
    portENTER_CRITICAL(&s_lock);
    finishCharacterLocked();
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_Delete(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_snapshot.current_pattern[0] != '\0') {
        const size_t n = strlen(s_snapshot.current_pattern);
        s_snapshot.current_pattern[n - 1u] = '\0';
    } else {
        const size_t n = strlen(s_snapshot.tx_text);
        if (n > 0u) s_snapshot.tx_text[n - 1u] = '\0';
        rebuildTxLearningRows();
    }
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

bool CW_SERVICE_Send(void)
{
    CW_SERVICE_FinishCharacter();
    if (s_queue == nullptr) return false;
    const Command command{CommandType::Send, CW_ELEMENT_DIT};
    return xQueueSend(s_queue, &command, 0u) == pdTRUE;
}

void CW_SERVICE_Clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.tx_text[0] = '\0';
    s_snapshot.tx_letters[0] = '\0';
    s_snapshot.tx_code[0] = '\0';
    s_snapshot.current_pattern[0] = '\0';
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_SetPractice(const bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.practice_enabled = enabled;
    s_snapshot.practice_attempts = 0u;
    s_snapshot.accuracy_percent = 100u;
    s_practice_correct = 0u;
    s_practice_index = 0u;
    s_snapshot.practice_target = 'E';
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_GetSnapshot(CwSnapshot *out)
{
    if (out == nullptr) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}
