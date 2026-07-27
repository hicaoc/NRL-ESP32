#include "lib/cw_codec.h"

#include <ctype.h>
#include <string.h>

namespace {

struct MorseEntry {
    char character;
    const char *pattern;
};

constexpr MorseEntry kMorse[] = {
    {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."},
    {'F', "..-."}, {'G', "--."}, {'H', "...."}, {'I', ".."}, {'J', ".---"},
    {'K', "-.-"}, {'L', ".-.."}, {'M', "--"}, {'N', "-."}, {'O', "---"},
    {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"},
    {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"}, {'Y', "-.--"},
    {'Z', "--.."}, {'0', "-----"}, {'1', ".----"}, {'2', "..---"},
    {'3', "...--"}, {'4', "....-"}, {'5', "....."}, {'6', "-...."},
    {'7', "--..."}, {'8', "---.."}, {'9', "----."},
};

float goertzelPower(const int16_t *samples, const float coefficient)
{
    float q1 = 0.0f;
    float q2 = 0.0f;
    for (size_t i = 0; i < 160u; ++i) {
        const float q0 = static_cast<float>(samples[i]) + coefficient * q1 - q2;
        q2 = q1;
        q1 = q0;
    }
    return q1 * q1 + q2 * q2 - coefficient * q1 * q2;
}

} // namespace

const char *CW_EncodeCharacter(const char character)
{
    const char upper = static_cast<char>(toupper(static_cast<unsigned char>(character)));
    for (const MorseEntry &entry : kMorse) {
        if (entry.character == upper) return entry.pattern;
    }
    return nullptr;
}

char CW_DecodePattern(const char *pattern)
{
    if (pattern == nullptr) return '?';
    for (const MorseEntry &entry : kMorse) {
        if (strcmp(entry.pattern, pattern) == 0) return entry.character;
    }
    return '?';
}

CwAudioDecoder::CwAudioDecoder() { reset(); }

void CwAudioDecoder::reset()
{
    block_count_ = 0u;
    raw_tone_ = false;
    tone_ = false;
    debounce_blocks_ = 0u;
    state_ms_ = 0u;
    dit_ms_ = 80u; // 15 WPM until the first well-formed element teaches us.
    pattern_[0] = '\0';
    pattern_length_ = 0u;
    character_emitted_ = false;
}

void CwAudioDecoder::feed(const int16_t *samples, const size_t count,
                          CwDecoderCallback callback, void *context)
{
    if (samples == nullptr) return;
    size_t offset = 0u;
    while (offset < count) {
        const size_t take = (count - offset < 160u - block_count_)
                                ? count - offset : 160u - block_count_;
        memcpy(block_ + block_count_, samples + offset, take * sizeof(int16_t));
        block_count_ += take;
        offset += take;
        if (block_count_ == 160u) {
            processBlock(block_, callback, context);
            block_count_ = 0u;
        }
    }
}

void CwAudioDecoder::processBlock(const int16_t *samples,
                                  CwDecoderCallback callback, void *context)
{
    double energy = 0.0;
    for (size_t i = 0; i < 160u; ++i) {
        const double sample = samples[i];
        energy += sample * sample;
    }

    float peak = 0.0f;
    // Covers the common 400..1000 Hz sidetone range. A bank makes receive
    // tolerant of radios whose CW pitch is not the local 700 Hz default.
    static constexpr float kToneCoefficients[] = {
        1.975376681f, 1.968853136f, 1.961570561f, 1.953531763f, 1.944739841f,
        1.935198185f, 1.924910473f, 1.913880671f, 1.902113033f, 1.889612093f,
        1.876382672f, 1.862429870f, 1.847759065f,
    };
    for (const float coefficient : kToneCoefficients) {
        const float power = goertzelPower(samples, coefficient);
        if (power > peak) peak = power;
    }
    const double mean_square = energy / 160.0;
    // For an on-bin sine, Goertzel power is roughly N/2 times total energy.
    // Requiring both absolute level and spectral concentration rejects normal
    // room noise and most speech while retaining moderately weak CW.
    const bool detected = mean_square > 180.0 * 180.0 &&
                          static_cast<double>(peak) > energy * 18.0;
    updateState(detected, callback, context);
}

void CwAudioDecoder::updateState(const bool detected,
                                 CwDecoderCallback callback, void *context)
{
    if (detected != raw_tone_) {
        raw_tone_ = detected;
        debounce_blocks_ = 1u;
    } else if (debounce_blocks_ < 2u) {
        ++debounce_blocks_;
    }

    if (raw_tone_ != tone_ && debounce_blocks_ >= 2u) {
        const uint16_t completed_ms = state_ms_ > 10u ? state_ms_ - 10u : state_ms_;
        if (tone_) {
            if (pattern_length_ < sizeof(pattern_) - 1u) {
                const bool dash = completed_ms > static_cast<uint16_t>(dit_ms_ * 2u);
                pattern_[pattern_length_++] = dash ? '-' : '.';
                pattern_[pattern_length_] = '\0';
                const uint16_t observed_dit = dash ? completed_ms / 3u : completed_ms;
                if (observed_dit >= 30u && observed_dit <= 300u) {
                    dit_ms_ = static_cast<uint16_t>((dit_ms_ * 3u + observed_dit) / 4u);
                }
            }
            character_emitted_ = false;
        }
        tone_ = raw_tone_;
        state_ms_ = 10u;
    } else if (state_ms_ < 60000u) {
        state_ms_ += 10u;
    }

    if (!tone_ && pattern_length_ > 0u && !character_emitted_ &&
        state_ms_ >= static_cast<uint16_t>(dit_ms_ * 3u)) {
        finishCharacter(callback, context);
    }
}

void CwAudioDecoder::finishCharacter(CwDecoderCallback callback, void *context)
{
    character_emitted_ = true;
    const char character = CW_DecodePattern(pattern_);
    const uint16_t wpm = dit_ms_ > 0u ? static_cast<uint16_t>(1200u / dit_ms_) : 0u;
    if (callback != nullptr) callback(character, pattern_, wpm, context);
    pattern_length_ = 0u;
    pattern_[0] = '\0';
}
