#pragma once

#include <stddef.h>
#include <stdint.h>

// Small, allocation-free Morse alphabet helpers shared by the decoder and UI.
const char *CW_EncodeCharacter(char character);
char CW_DecodePattern(const char *pattern);

typedef void (*CwDecoderCallback)(char character,
                                  const char *pattern,
                                  uint16_t estimated_wpm,
                                  void *context);

// Audio Morse decoder for 16 kHz mono PCM. It evaluates a bank of CW tone
// frequencies every 10 ms, then derives dots/dashes and spacing with an
// adaptive dit estimate. Each instance is independent, so MIC and NRL timing
// cannot contaminate one another.
class CwAudioDecoder {
public:
    CwAudioDecoder();
    void reset();
    void feed(const int16_t *samples, size_t count,
              CwDecoderCallback callback, void *context);

private:
    void processBlock(const int16_t *samples, CwDecoderCallback callback, void *context);
    void updateState(bool tone, CwDecoderCallback callback, void *context);
    void finishCharacter(CwDecoderCallback callback, void *context);

    int16_t block_[160];
    size_t block_count_;
    bool raw_tone_;
    bool tone_;
    uint8_t debounce_blocks_;
    uint16_t state_ms_;
    uint16_t dit_ms_;
    char pattern_[8];
    uint8_t pattern_length_;
    bool character_emitted_;
};
