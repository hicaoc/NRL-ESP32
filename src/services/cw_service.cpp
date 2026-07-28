#include "services/cw_service.h"

#include "audio/audio_router.h"
#include "lib/cw_codec.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>

#include <math.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr const char *TAG = "CW";
constexpr uint32_t kSampleRate = 16000u;
constexpr uint16_t kDefaultWpm = 15u;
constexpr uint16_t kToneHz = 700u;
constexpr size_t kChunkSamples = 160u;
constexpr uint32_t kTxTaskStackBytes = 8192u;
// Koch learn-order over the codec's alphabet (A-Z, 0-9): start with K and M,
// each new letter joins the practice set once the recent window shows mastery.
constexpr char kKochOrder[] = "KMRSUAPTLOWINJEFYVGQHZXCBD5293847160";
constexpr size_t kKochCount = sizeof(kKochOrder) - 1u; // 36
constexpr uint8_t kKochInitial = 2u;
constexpr size_t kRecentWindow = 20u;      // rolling answers per level
constexpr unsigned kRecentToUnlock = 18u;  // 90% to unlock the next letter
constexpr unsigned kAnswersPerSave = 5u;   // NVS write throttle

enum class CommandType : uint8_t { Element, Send, ToneStart, ToneStop, PaddleStart, PaddleStop };
struct Command { CommandType type; CwElement element; };

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
CwSnapshot s_snapshot = {};
QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
uint32_t s_last_element_ms = 0u;
uint16_t s_practice_correct = 0u;
uint32_t s_practice_timing_sum = 0u;
uint16_t s_practice_timing_samples = 0u;
// Koch progression: per-letter copy stats persist in NVS; the recent-answer
// ring decides when the next letter unlocks.
uint8_t s_koch_unlocked = kKochInitial;
uint16_t s_koch_attempts[kKochCount] = {};
uint16_t s_koch_correct[kKochCount] = {};
bool s_recent[kRecentWindow] = {};
size_t s_recent_pos = 0u;
size_t s_recent_count = 0u;
char s_copy_target = '\0';    // hidden RX answer
char s_play_pending = '\0';   // letter the worker should sound out
bool s_save_pending = false;  // worker flushes Koch stats to NVS
unsigned s_answers_since_save = 0u;
// Practice charset: targets are drawn from the active set. KOCH tracks the
// persistent unlock progression; the other sets are fully available at once.
uint8_t s_charset = CW_CHARSET_KOCH;
char s_custom_set[kKochCount + 1u] = {};
char s_active_set[kKochCount + 1u] = {};
size_t s_active_count = 0u;
// Adaptive practice speed: after every 5 answers, >=4 correct nudges WPM up,
// <=2 correct nudges it down (clamped); persisted with the Koch stats.
bool s_adaptive_wpm = false;
unsigned s_adapt_window_correct = 0u;
unsigned s_adapt_window_count = 0u;
uint16_t s_persisted_wpm = 0u; // loaded from NVS, applied in Init
constexpr uint16_t kPracticeWpmMin = 10u;
constexpr uint16_t kPracticeWpmMax = 30u;
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
void rebuildActiveSetLocked();

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

// Random target from the unlocked Koch set, avoiding an immediate repeat of
// the previous target. Caller holds s_lock.
char pickTargetLocked()
{
    if (s_active_count == 0u) rebuildActiveSetLocked();
    size_t index = esp_random() % s_active_count;
    if (s_active_count > 1u && s_active_set[index] == s_snapshot.practice_target) {
        index = (index + 1u) % s_active_count;
    }
    return s_active_set[index];
}

// The set targets are drawn from, per the charset selection. Caller holds s_lock.
void rebuildActiveSetLocked()
{
    size_t n = 0u;
    switch (s_charset) {
        case CW_CHARSET_LETTERS:
            for (char c = 'A'; c <= 'Z'; ++c) s_active_set[n++] = c;
            break;
        case CW_CHARSET_DIGITS:
            for (char c = '0'; c <= '9'; ++c) s_active_set[n++] = c;
            break;
        case CW_CHARSET_CUSTOM:
            if (s_custom_set[0] != '\0') {
                for (const char *p = s_custom_set; *p != '\0'; ++p) s_active_set[n++] = *p;
                break;
            }
            // No custom set stored yet: use the Koch progression instead.
            [[fallthrough]];
        case CW_CHARSET_KOCH:
        default:
            for (size_t i = 0u; i < s_koch_unlocked; ++i) s_active_set[n++] = kKochOrder[i];
            break;
    }
    s_active_set[n] = '\0';
    s_active_count = n;
}

void recordLetterStatLocked(const char letter, const bool correct)
{
    for (size_t i = 0u; i < kKochCount; ++i) {
        if (kKochOrder[i] != letter) continue;
        if (s_koch_attempts[i] < 0xFFFFu) ++s_koch_attempts[i];
        if (correct && s_koch_correct[i] < 0xFFFFu) ++s_koch_correct[i];
        return;
    }
}

void adaptWpmLocked(const bool correct)
{
    if (!s_adaptive_wpm) return;
    ++s_adapt_window_count;
    if (correct) ++s_adapt_window_correct;
    if (s_adapt_window_count < 5u) return;
    const unsigned hits = s_adapt_window_correct;
    s_adapt_window_correct = 0u;
    s_adapt_window_count = 0u;
    uint16_t wpm = s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm;
    if (hits >= 4u && wpm < kPracticeWpmMax) {
        ++wpm;
    } else if (hits <= 2u && wpm > kPracticeWpmMin) {
        --wpm;
    } else {
        return;
    }
    s_snapshot.wpm = wpm;
    s_save_pending = true; // keep the learned practice speed across reboots
}

struct KochStore {
    uint8_t unlocked;
    uint16_t attempts[kKochCount];
    uint16_t correct[kKochCount];
    // v2 fields; blobs written before the charset/adaptive update end here.
    uint8_t charset;
    uint8_t adaptive;
    uint16_t practice_wpm;
    char custom[kKochCount + 1u];
};

// Runs in the worker task (NVS writes block); snapshots the stats under lock.
void saveKoch()
{
    KochStore store{};
    portENTER_CRITICAL(&s_lock);
    store.unlocked = s_koch_unlocked;
    memcpy(store.attempts, s_koch_attempts, sizeof(store.attempts));
    memcpy(store.correct, s_koch_correct, sizeof(store.correct));
    store.charset = s_charset;
    store.adaptive = s_adaptive_wpm ? 1u : 0u;
    store.practice_wpm = s_snapshot.wpm;
    memcpy(store.custom, s_custom_set, sizeof(store.custom));
    portEXIT_CRITICAL(&s_lock);
    nvs_handle_t handle;
    if (nvs_open("cw", NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_blob(handle, "koch", &store, sizeof(store));
    (void)nvs_commit(handle);
    nvs_close(handle);
}

void loadKoch()
{
    KochStore store{};
    size_t length = sizeof(store);
    nvs_handle_t handle;
    if (nvs_open("cw", NVS_READONLY, &handle) != ESP_OK) return;
    const esp_err_t err = nvs_get_blob(handle, "koch", &store, &length);
    nvs_close(handle);
    const size_t v1_bytes = offsetof(KochStore, charset);
    if (err != ESP_OK || (length != sizeof(store) && length != v1_bytes) ||
        store.unlocked < kKochInitial || store.unlocked > kKochCount) {
        return;
    }
    s_koch_unlocked = store.unlocked;
    memcpy(s_koch_attempts, store.attempts, sizeof(s_koch_attempts));
    memcpy(s_koch_correct, store.correct, sizeof(s_koch_correct));
    if (length != sizeof(store)) return; // v1 blob: defaults for the rest
    if (store.charset <= CW_CHARSET_CUSTOM) s_charset = store.charset;
    s_adaptive_wpm = store.adaptive != 0u;
    if (store.practice_wpm >= 4u && store.practice_wpm <= 60u) {
        s_persisted_wpm = store.practice_wpm;
    }
    store.custom[kKochCount] = '\0';
    memcpy(s_custom_set, store.custom, sizeof(s_custom_set));
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
            char play = '\0';
            bool save = false;
            portENTER_CRITICAL(&s_lock);
            const uint16_t wpm = s_snapshot.wpm == 0u ? kDefaultWpm : s_snapshot.wpm;
            if (s_snapshot.current_pattern[0] != '\0' && s_last_element_ms != 0u &&
                now - s_last_element_ms >= static_cast<uint32_t>(3600u / wpm)) {
                finishCharacterLocked();
            }
            play = s_play_pending;
            s_play_pending = '\0';
            save = s_save_pending;
            s_save_pending = false;
            portEXIT_CRITICAL(&s_lock);
            // RX practice prompt/replay: sound out one letter on the same
            // tone path as keyed elements.
            if (play != '\0') {
                const uint16_t dit_ms = static_cast<uint16_t>(1200u / wpm);
                const char *pattern = CW_EncodeCharacter(play);
                if (pattern != nullptr) {
                    for (const char *e = pattern; *e != '\0'; ++e) {
                        playElement(*e == '-' ? CW_ELEMENT_DAH : CW_ELEMENT_DIT, dit_ms);
                    }
                }
            }
            if (save) saveKoch();
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
    if (s_snapshot.practice_mode == CW_PRACTICE_TX) {
        ++s_snapshot.practice_attempts;
        if (decoded == s_snapshot.practice_target) ++s_practice_correct;
        s_snapshot.accuracy_percent = static_cast<uint8_t>(
            (static_cast<uint32_t>(s_practice_correct) * 100u) / s_snapshot.practice_attempts);
        recordLetterStatLocked(s_snapshot.practice_target,
                               decoded == s_snapshot.practice_target);
        adaptWpmLocked(decoded == s_snapshot.practice_target);
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
        s_snapshot.practice_target = pickTargetLocked();
    } else if (s_snapshot.practice_mode == CW_PRACTICE_RX && s_snapshot.copy_awaiting) {
        // RX copy: the keyed character is the answer to the sounded target.
        const bool correct = decoded == s_copy_target;
        ++s_snapshot.practice_attempts;
        if (correct) ++s_practice_correct;
        s_snapshot.accuracy_percent = static_cast<uint8_t>(
            (static_cast<uint32_t>(s_practice_correct) * 100u) / s_snapshot.practice_attempts);
        recordLetterStatLocked(s_copy_target, correct);
        adaptWpmLocked(correct);
        s_recent[s_recent_pos] = correct;
        s_recent_pos = (s_recent_pos + 1u) % kRecentWindow;
        if (s_recent_count < kRecentWindow) ++s_recent_count;
        if (s_recent_count >= kRecentWindow && s_koch_unlocked < kKochCount) {
            unsigned hits = 0u;
            for (size_t i = 0u; i < kRecentWindow; ++i) hits += s_recent[i] ? 1u : 0u;
            if (hits >= kRecentToUnlock) {
                ++s_koch_unlocked;
                s_snapshot.koch_unlocked = s_koch_unlocked;
                rebuildActiveSetLocked();
                s_recent_pos = 0u;
                s_recent_count = 0u;
                s_save_pending = true;
            }
        }
        if (++s_answers_since_save >= kAnswersPerSave) {
            s_answers_since_save = 0u;
            s_save_pending = true;
        }
        s_snapshot.copy_revealed = s_copy_target;
        s_snapshot.copy_last_correct = correct;
        s_copy_target = pickTargetLocked();
        s_snapshot.practice_target = s_copy_target;
        s_play_pending = s_copy_target;
    }
    s_element_count = 0u;
    s_snapshot.current_pattern[0] = '\0';
    ++s_snapshot.revision;
}

} // namespace

void CW_SERVICE_Init(void)
{
    loadKoch();
    portENTER_CRITICAL(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.wpm = kDefaultWpm;
    if (s_persisted_wpm >= 4u && s_persisted_wpm <= 60u) s_snapshot.wpm = s_persisted_wpm;
    s_snapshot.accuracy_percent = 100u;
    s_snapshot.timing_percent = 100u;
    s_snapshot.koch_unlocked = s_koch_unlocked;
    s_snapshot.charset = s_charset;
    s_snapshot.adaptive_wpm = s_adaptive_wpm;
    rebuildActiveSetLocked();
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
    CW_SERVICE_SetPracticeMode(enabled ? CW_PRACTICE_TX : CW_PRACTICE_OFF);
}

void CW_SERVICE_SetPracticeMode(const CwPracticeMode mode)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.practice_mode = mode;
    s_snapshot.practice_enabled = mode != CW_PRACTICE_OFF;
    s_snapshot.practice_attempts = 0u;
    s_snapshot.accuracy_percent = 100u;
    s_snapshot.timing_percent = 100u;
    s_practice_correct = 0u;
    s_practice_timing_sum = 0u;
    s_practice_timing_samples = 0u;
    s_snapshot.copy_awaiting = false;
    s_snapshot.copy_revealed = '\0';
    s_snapshot.copy_last_correct = false;
    if (mode == CW_PRACTICE_TX) {
        s_snapshot.practice_target = pickTargetLocked();
    } else if (mode == CW_PRACTICE_RX) {
        s_copy_target = pickTargetLocked();
        s_snapshot.practice_target = s_copy_target;
        s_snapshot.copy_awaiting = true;
        s_play_pending = s_copy_target;
    } else {
        s_save_pending = true; // flush copy stats when leaving practice
    }
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_ReplayTarget(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_snapshot.practice_mode == CW_PRACTICE_RX && s_snapshot.copy_awaiting &&
        s_copy_target != '\0') {
        s_play_pending = s_copy_target;
    }
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_SetCharset(const CwCharset charset, const char *custom)
{
    portENTER_CRITICAL(&s_lock);
    if (charset == CW_CHARSET_CUSTOM && custom != nullptr) {
        // Keep only characters the codec can encode, deduplicated.
        size_t n = 0u;
        for (const char *p = custom; *p != '\0' && n < kKochCount; ++p) {
            const char c = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
            if (CW_EncodeCharacter(c) == nullptr) continue;
            bool dup = false;
            for (size_t i = 0u; i < n; ++i) dup = dup || s_custom_set[i] == c;
            if (!dup) s_custom_set[n++] = c;
        }
        s_custom_set[n] = '\0';
    }
    s_charset = charset;
    s_snapshot.charset = charset;
    rebuildActiveSetLocked();
    // Draw a fresh target from the new set when a practice session is live.
    if (s_snapshot.practice_mode == CW_PRACTICE_TX) {
        s_snapshot.practice_target = pickTargetLocked();
    } else if (s_snapshot.practice_mode == CW_PRACTICE_RX && s_snapshot.copy_awaiting) {
        s_copy_target = pickTargetLocked();
        s_snapshot.practice_target = s_copy_target;
        s_play_pending = s_copy_target;
    }
    s_save_pending = true;
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_SetAdaptiveWpm(const bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_adaptive_wpm = enabled;
    s_snapshot.adaptive_wpm = enabled;
    s_adapt_window_correct = 0u;
    s_adapt_window_count = 0u;
    s_save_pending = true;
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

size_t CW_SERVICE_GetLetterStats(CwLetterStat *out, const size_t capacity)
{
    if (out == nullptr || capacity == 0u) return 0u;
    const size_t n = capacity < kKochCount ? capacity : kKochCount;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0u; i < n; ++i) {
        out[i].letter = kKochOrder[i];
        out[i].attempts = s_koch_attempts[i];
        out[i].correct = s_koch_correct[i];
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void CW_SERVICE_GetCustomCharset(char *out, const size_t capacity)
{
    if (out == nullptr || capacity == 0u) return;
    portENTER_CRITICAL(&s_lock);
    snprintf(out, capacity, "%s", s_custom_set);
    portEXIT_CRITICAL(&s_lock);
}

void CW_SERVICE_GetSnapshot(CwSnapshot *out)
{
    if (out == nullptr) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}
