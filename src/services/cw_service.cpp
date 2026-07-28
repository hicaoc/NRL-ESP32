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

enum class CommandType : uint8_t { Element, Send, ToneStart, ToneStop, PaddleStart, PaddleStop };
struct Command { CommandType type; CwElement element; };

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
CwSnapshot s_snapshot = {};
QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
uint32_t s_last_element_ms = 0u;
uint16_t s_practice_correct = 0u;
size_t s_practice_index = 0u;
uint32_t s_practice_timing_sum = 0u;
uint16_t s_practice_timing_samples = 0u;
// Straight-key state (touch UI): key-down timestamp and per-element held
// durations for the current character, used by the practice timing score.
bool s_key_down = false;
uint32_t s_key_down_ms = 0u;
uint16_t s_element_durations[7] = {};
size_t s_element_count = 0u;
// Keep audio and message buffers out of the worker stack. AudioRouter performs
// rate conversion synchronously and has its own scratch/callback stack usage,
// so stacking another two PCM arrays here left too little headroom on ESP32-P4.
int16_t s_tone_pcm[kChunkSamples] = {};
int16_t s_silence_pcm[kChunkSamples] = {};
char s_send_text[sizeof(s_snapshot.tx_text)] = {};

void finishCharacterLocked();

// Append one keyed element to the current pattern (caller holds s_lock).
// Shared by the button-style InputElement and the straight-key KeyUp path.
void inputElementLocked(const CwElement element, const uint32_t now)
{
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
}

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

// Commit one keyer element: append it to the pattern (with its ideal
// duration for the practice timing score) and start the tone phase.
// Runs in the worker task.
void paddleBeginElement(const CwElement element, const uint16_t dit_ms,
                        bool &tone_phase, int32_t &phase_ms)
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    const uint16_t ideal_ms =
        static_cast<uint16_t>((element == CW_ELEMENT_DAH ? 3u : 1u) * dit_ms);
    portENTER_CRITICAL(&s_lock);
    if (strlen(s_snapshot.current_pattern) == 0u) s_element_count = 0u;
    if (s_element_count < sizeof(s_element_durations) / sizeof(s_element_durations[0])) {
        s_element_durations[s_element_count++] = ideal_ms;
    }
    inputElementLocked(element, now);
    portEXIT_CRITICAL(&s_lock);
    tone_phase = true;
    phase_ms = ideal_ms;
}

void worker(void *)
{
    Command command{};
    // Straight-key sidetone gate: while the touch key is held, keep feeding
    // 10 ms tone chunks so the tone tracks the finger instead of playing a
    // fixed-length element after the fact.
    bool tone_gate = false;
    // Single-paddle keyer: while active, emit a stream of paddle_element with
    // standard 1:3:1 timing at the WPM captured on PaddleStart. A PaddleStop
    // finishes the current element plus inter-element space, then stops --
    // the same feel as releasing a real keyer paddle.
    bool paddle_active = false;
    bool paddle_stop_pending = false;
    bool paddle_tone = false;
    CwElement paddle_element = CW_ELEMENT_DIT;
    uint16_t paddle_dit_ms = 80u;
    int32_t paddle_phase_ms = 0;
    while (true) {
        const bool fast_tick = tone_gate || paddle_active;
        const TickType_t wait = fast_tick ? pdMS_TO_TICKS(10) : pdMS_TO_TICKS(20);
        if (xQueueReceive(s_queue, &command, wait) != pdTRUE) {
            if (tone_gate) {
                AudioRouter_PushFrame(AUDIO_SRC_CW_NRL, kSampleRate, s_tone_pcm, kChunkSamples);
                AudioRouter_PushFrame(AUDIO_SRC_CW_SPEAKER, kSampleRate, s_tone_pcm, kChunkSamples);
            } else if (paddle_active) {
                const int16_t *pcm = paddle_tone ? s_tone_pcm : s_silence_pcm;
                AudioRouter_PushFrame(AUDIO_SRC_CW_NRL, kSampleRate, pcm, kChunkSamples);
                AudioRouter_PushFrame(AUDIO_SRC_CW_SPEAKER, kSampleRate, pcm, kChunkSamples);
                paddle_phase_ms -= 10;
                if (paddle_phase_ms <= 0) {
                    if (paddle_tone) {
                        paddle_tone = false;
                        paddle_phase_ms = paddle_dit_ms;
                    } else if (paddle_stop_pending) {
                        paddle_active = false;
                        paddle_stop_pending = false;
                    } else {
                        paddleBeginElement(paddle_element, paddle_dit_ms,
                                           paddle_tone, paddle_phase_ms);
                    }
                }
            }
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
        if (command.type == CommandType::ToneStart) {
            tone_gate = true;
        } else if (command.type == CommandType::ToneStop) {
            tone_gate = false;
        } else if (command.type == CommandType::PaddleStart) {
            paddle_element = command.element; // a slide switches the next element
            if (!paddle_active) {
                paddle_active = true;
                paddle_stop_pending = false;
                uint16_t wpm = kDefaultWpm;
                portENTER_CRITICAL(&s_lock);
                wpm = s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm;
                portEXIT_CRITICAL(&s_lock);
                paddle_dit_ms = static_cast<uint16_t>(1200u / wpm);
                paddleBeginElement(paddle_element, paddle_dit_ms,
                                   paddle_tone, paddle_phase_ms);
            }
        } else if (command.type == CommandType::PaddleStop) {
            if (paddle_active) {
                paddle_stop_pending = true;
            }
        } else if (command.type == CommandType::Element) {
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
        // Timing quality: compare the average dah:dit ratio of the keyed
        // elements against the ideal 3:1. Characters with only dits or only
        // dahs carry no ratio information and are skipped.
        uint32_t dit_sum = 0u, dah_sum = 0u;
        size_t dit_n = 0u, dah_n = 0u;
        const size_t n = strlen(s_snapshot.current_pattern);
        for (size_t i = 0u; i < n && i < s_element_count; ++i) {
            if (s_snapshot.current_pattern[i] == '-') {
                dah_sum += s_element_durations[i];
                ++dah_n;
            } else {
                dit_sum += s_element_durations[i];
                ++dit_n;
            }
        }
        if (dit_n > 0u && dah_n > 0u) {
            const float ratio =
                (static_cast<float>(dah_sum) / static_cast<float>(dah_n)) /
                (static_cast<float>(dit_sum) / static_cast<float>(dit_n));
            float score = 100.0f - (ratio > 3.0f ? ratio - 3.0f : 3.0f - ratio) * 50.0f;
            if (score < 0.0f) score = 0.0f;
            s_practice_timing_sum += static_cast<uint32_t>(score);
            ++s_practice_timing_samples;
            s_snapshot.timing_percent = static_cast<uint8_t>(
                s_practice_timing_sum / s_practice_timing_samples);
        }
        do {
            s_practice_index = (s_practice_index + 1u) % (sizeof(kPracticeCharacters) - 1u);
            s_snapshot.practice_target = kPracticeCharacters[s_practice_index];
        } while (s_snapshot.practice_target == '?');
    }
    s_element_count = 0u;
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
    s_snapshot.timing_percent = 100u;
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
    inputElementLocked(element, now);
    portEXIT_CRITICAL(&s_lock);
    if (s_queue != nullptr) {
        const Command command{CommandType::Element, element};
        (void)xQueueSend(s_queue, &command, 0u);
    }
}

void CW_SERVICE_KeyDown(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool was_down = s_key_down;
    s_key_down = true;
    s_key_down_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    portEXIT_CRITICAL(&s_lock);
    if (was_down || s_queue == nullptr) return;
    const Command command{CommandType::ToneStart, CW_ELEMENT_DIT};
    (void)xQueueSend(s_queue, &command, 0u);
}

void CW_SERVICE_KeyUp(void)
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_lock);
    if (!s_key_down) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_key_down = false;
    const uint32_t held = now - s_key_down_ms; // uint32 subtraction is wrap-safe
    const uint16_t wpm = s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm;
    // Ideal lengths are 1 and 3 dit-times; split at 2.
    const CwElement element =
        held >= static_cast<uint32_t>(2400u / wpm) ? CW_ELEMENT_DAH : CW_ELEMENT_DIT;
    if (strlen(s_snapshot.current_pattern) == 0u) s_element_count = 0u;
    if (s_element_count < sizeof(s_element_durations) / sizeof(s_element_durations[0])) {
        s_element_durations[s_element_count++] =
            static_cast<uint16_t>(held > 60000u ? 60000u : held);
    }
    inputElementLocked(element, now);
    portEXIT_CRITICAL(&s_lock);
    if (s_queue != nullptr) {
        const Command command{CommandType::ToneStop, CW_ELEMENT_DIT};
        (void)xQueueSend(s_queue, &command, 0u);
    }
}

void CW_SERVICE_PaddleStart(const CwElement element)
{
    if (s_queue == nullptr) return;
    const Command command{CommandType::PaddleStart, element};
    (void)xQueueSend(s_queue, &command, 0u);
}

void CW_SERVICE_PaddleStop(void)
{
    if (s_queue == nullptr) return;
    const Command command{CommandType::PaddleStop, CW_ELEMENT_DIT};
    (void)xQueueSend(s_queue, &command, 0u);
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
    s_snapshot.timing_percent = 100u;
    s_practice_correct = 0u;
    s_practice_index = 0u;
    s_practice_timing_sum = 0u;
    s_practice_timing_samples = 0u;
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
