#include "app/driver/vox.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <vector>

namespace {

constexpr size_t kFrameSamples = 160u; // 10 ms at 16 kHz

std::vector<bool> g_ptt_calls;

void feedFrame(const int16_t amplitude, const unsigned frames)
{
    int16_t frame[kFrameSamples];
    for (size_t i = 0; i < kFrameSamples; ++i) {
        frame[i] = amplitude;
    }
    for (unsigned n = 0; n < frames; ++n) {
        VOX_ProcessFrame(frame, kFrameSamples);
    }
}

void resetVox()
{
    VOX_Configure(false, -40, -48, 30, 100);
    feedFrame(0, 1u);
    g_ptt_calls.clear();
    VOX_Configure(true, -40, -48, 30, 100);
}

} // namespace

extern "C" void STATUS_IO_SetSoftPtt(const bool held)
{
    g_ptt_calls.push_back(held);
}

int main()
{
    // Silence never keys up.
    resetVox();
    feedFrame(0, 50u);
    assert(g_ptt_calls.empty());
    assert(!VOX_IsActive());

    // Loud audio shorter than the 30 ms attack time does not key up...
    resetVox();
    feedFrame(2000, 2u);
    assert(g_ptt_calls.empty());
    assert(!VOX_IsActive());

    // ...and the third consecutive loud frame keys up exactly once.
    feedFrame(2000, 1u);
    assert(VOX_IsActive());
    assert(g_ptt_calls.size() == 1u && g_ptt_calls[0]);
    feedFrame(2000, 5u);
    assert(g_ptt_calls.size() == 1u);

    // Hysteresis: a level between close (-48) and open (-40) holds TX.
    feedFrame(184, 30u); // -45 dBFS
    assert(VOX_IsActive());
    assert(g_ptt_calls.size() == 1u);

    // Below close, the 100 ms hang keeps TX for 9 frames; frame 10 closes.
    feedFrame(50, 9u); // -56 dBFS
    assert(VOX_IsActive());
    feedFrame(50, 1u);
    assert(!VOX_IsActive());
    assert(g_ptt_calls.size() == 2u && !g_ptt_calls[1]);

    // Speech during the hang window resets the hang countdown.
    feedFrame(2000, 3u);
    assert(VOX_IsActive());
    assert(g_ptt_calls.size() == 3u && g_ptt_calls[2]);
    feedFrame(50, 9u);
    feedFrame(2000, 1u);
    feedFrame(50, 9u);
    assert(VOX_IsActive());
    feedFrame(50, 1u);
    assert(!VOX_IsActive());
    assert(g_ptt_calls.size() == 4u && !g_ptt_calls[3]);

    // The attack countdown is not cumulative across dropouts.
    resetVox();
    feedFrame(2000, 2u);
    feedFrame(50, 1u);
    feedFrame(2000, 2u);
    assert(!VOX_IsActive());
    assert(g_ptt_calls.empty());
    feedFrame(2000, 1u);
    assert(VOX_IsActive());

    // Disabled VOX ignores audio entirely.
    resetVox();
    VOX_Configure(false, -40, -48, 30, 100);
    feedFrame(2000, 10u);
    assert(g_ptt_calls.empty());
    assert(!VOX_IsActive());

    // Disabling while transmitting releases the soft PTT on the next frame.
    resetVox();
    feedFrame(2000, 3u);
    assert(VOX_IsActive());
    VOX_Configure(false, -40, -48, 30, 100);
    feedFrame(2000, 1u);
    assert(!VOX_IsActive());
    assert(g_ptt_calls.size() == 2u && !g_ptt_calls[1]);

    // Zero attack keys up on the first loud frame.
    resetVox();
    VOX_Configure(true, -40, -48, 0, 100);
    feedFrame(2000, 1u);
    assert(VOX_IsActive());

    // open_db is clamped to -10: -20 dBFS must not key up, -4 dBFS must.
    resetVox();
    VOX_Configure(true, -5, -20, 30, 100);
    feedFrame(3277, 10u); // -20 dBFS
    assert(!VOX_IsActive());
    feedFrame(20000, 3u); // -4.3 dBFS
    assert(VOX_IsActive());

    // close_db is forced to open-2 (-42): -41 dBFS holds TX, -45 dBFS hangs up.
    resetVox();
    VOX_Configure(true, -40, -38, 30, 100);
    feedFrame(2000, 3u);
    assert(VOX_IsActive());
    feedFrame(292, 30u); // -41 dBFS
    assert(VOX_IsActive());
    feedFrame(184, 10u); // -45 dBFS
    assert(!VOX_IsActive());

    // attack_ms / hang_ms are clamped to 500 / 3000 (50 / 300 frames).
    resetVox();
    VOX_Configure(true, -40, -48, 60000, 60000);
    feedFrame(2000, 49u);
    assert(!VOX_IsActive());
    feedFrame(2000, 1u);
    assert(VOX_IsActive());
    feedFrame(50, 299u);
    assert(VOX_IsActive());
    feedFrame(50, 1u);
    assert(!VOX_IsActive());

    // Level reporting in dBFS.
    resetVox();
    feedFrame(3277, 1u);
    assert(fabsf(VOX_CurrentLevelDb() + 20.0f) < 0.05f);
    feedFrame(0, 1u);
    assert(VOX_CurrentLevelDb() <= -99.0f);

    return 0;
}
