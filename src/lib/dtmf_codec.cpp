#include "lib/dtmf_codec.h"

#include <math.h>

namespace {
constexpr uint32_t kSampleRate = 16000u;
constexpr double kPi = 3.14159265358979323846;

double goertzelPower(const int16_t *samples, size_t count, double frequency)
{
    // The ESP32-S3/S31 FPU is single-precision only; double arithmetic in the
    // per-sample loop is emulated in software. Iterate in float (standard
    // practice for a 320-point Goertzel window) and only promote to double
    // for the final power term.
    const float coefficient =
        static_cast<float>(2.0 * cos(2.0 * kPi * frequency / static_cast<double>(kSampleRate)));
    float q1 = 0.0f;
    float q2 = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float q0 = static_cast<float>(samples[i]) + coefficient * q1 - q2;
        q2 = q1;
        q1 = q0;
    }
    return static_cast<double>(q1) * q1 + static_cast<double>(q2) * q2 -
           static_cast<double>(coefficient) * q1 * q2;
}
} // namespace

DtmfDecoder::DtmfDecoder() { reset(); }

void DtmfDecoder::reset()
{
    fill_ = 0u;
    candidate_ = '\0';
    emitted_ = '\0';
    stable_frames_ = 0u;
}

char DtmfDecoder::decodeFrame() const
{
    static const double frequencies[8] = {697, 770, 852, 941, 1209, 1336, 1477, 1633};
    static const char keys[4][4] = {{'1','2','3','A'}, {'4','5','6','B'},
                                    {'7','8','9','C'}, {'*','0','#','D'}};
    double power[8];
    for (size_t i = 0; i < 8; ++i) power[i] = goertzelPower(frame_, 320u, frequencies[i]);
    size_t low = 0u;
    size_t high = 4u;
    for (size_t i = 1; i < 4; ++i) if (power[i] > power[low]) low = i;
    for (size_t i = 5; i < 8; ++i) if (power[i] > power[high]) high = i;
    double low_second = 0.0;
    double high_second = 0.0;
    for (size_t i = 0; i < 4; ++i) if (i != low && power[i] > low_second) low_second = power[i];
    for (size_t i = 4; i < 8; ++i) if (i != high && power[i] > high_second) high_second = power[i];
    if (power[low] < 1.0e10 || power[high] < 1.0e10) return '\0';
    if (power[low] < low_second * 2.5 || power[high] < high_second * 2.5) return '\0';
    const double twist = power[low] / power[high];
    if (twist < 0.20 || twist > 5.0) return '\0';
    return keys[low][high - 4u];
}

void DtmfDecoder::feed(const int16_t *samples, size_t count,
                       DtmfDigitCallback callback, void *context)
{
    if (samples == nullptr) return;
    for (size_t i = 0; i < count; ++i) {
        frame_[fill_++] = samples[i];
        if (fill_ != 320u) continue;
        fill_ = 0u;
        const char digit = decodeFrame();
        if (digit == candidate_) {
            if (stable_frames_ < 255u) ++stable_frames_;
        } else {
            candidate_ = digit;
            stable_frames_ = digit == '\0' ? 0u : 1u;
        }
        if (digit == '\0') {
            emitted_ = '\0';
        } else if (stable_frames_ >= 2u && emitted_ != digit) {
            emitted_ = digit;
            if (callback != nullptr) callback(digit, context);
        }
    }
}

bool DTMF_Frequencies(char digit, uint16_t *low_hz, uint16_t *high_hz)
{
    static const char keys[4][4] = {{'1','2','3','A'}, {'4','5','6','B'},
                                    {'7','8','9','C'}, {'*','0','#','D'}};
    static const uint16_t low[4] = {697,770,852,941};
    static const uint16_t high[4] = {1209,1336,1477,1633};
    if (digit >= 'a' && digit <= 'd') digit = static_cast<char>(digit - 'a' + 'A');
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            if (keys[row][col] == digit) {
                if (low_hz != nullptr) *low_hz = low[row];
                if (high_hz != nullptr) *high_hz = high[col];
                return true;
            }
        }
    }
    return false;
}

bool DTMF_IsValid(const char *digits)
{
    if (digits == nullptr || digits[0] == '\0') return false;
    size_t count = 0u;
    for (; digits[count] != '\0'; ++count) {
        if (count >= 16u || !DTMF_Frequencies(digits[count], nullptr, nullptr)) return false;
    }
    return count > 0u;
}
