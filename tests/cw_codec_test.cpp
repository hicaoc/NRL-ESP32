#include "lib/cw_codec.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace {

struct Result {
    char character = '\0';
    char pattern[8] = {};
    uint16_t wpm = 0u;
};

void decoded(char character, const char *pattern, uint16_t wpm, void *context)
{
    auto *result = static_cast<Result *>(context);
    result->character = character;
    result->wpm = wpm;
    strncpy(result->pattern, pattern, sizeof(result->pattern) - 1u);
}

void appendTone(std::vector<int16_t> &pcm, unsigned duration_ms)
{
    const size_t count = duration_ms * 16u;
    for (size_t i = 0u; i < count; ++i) {
        pcm.push_back(static_cast<int16_t>(7000.0 *
            sin(2.0 * 3.14159265358979323846 * 700.0 * i / 16000.0)));
    }
}

void appendSilence(std::vector<int16_t> &pcm, unsigned duration_ms)
{
    pcm.insert(pcm.end(), duration_ms * 16u, 0);
}

} // namespace

int main()
{
    assert(strcmp(CW_EncodeCharacter('A'), ".-") == 0);
    assert(strcmp(CW_EncodeCharacter('5'), ".....") == 0);
    assert(CW_DecodePattern("--.-") == 'Q');
    assert(CW_DecodePattern("..---") == '2');

    std::vector<int16_t> pcm;
    appendSilence(pcm, 40u);
    appendTone(pcm, 80u);
    appendSilence(pcm, 80u);
    appendTone(pcm, 240u);
    appendSilence(pcm, 400u);

    CwAudioDecoder decoder;
    Result result;
    decoder.feed(pcm.data(), pcm.size(), decoded, &result);
    assert(result.character == 'A');
    assert(strcmp(result.pattern, ".-") == 0);
    assert(result.wpm >= 12u && result.wpm <= 20u);
    return 0;
}
