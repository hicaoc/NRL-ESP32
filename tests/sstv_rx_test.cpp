#include "lib/sstv_codec.h"
#include "lib/sstv_rx.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

namespace {

unsigned s_vis_count = 0u;
unsigned s_line_count = 0u;
unsigned s_done_count = 0u;
bool s_callback_error = false;
uint32_t s_noise_state = 1u;

void onVis(SSTV_Mode mode, void *)
{
    if (mode != SSTV_MODE_ROBOT36) {
        fprintf(stderr, "unexpected VIS mode: %u\n", static_cast<unsigned>(mode));
        s_callback_error = true;
    }
    ++s_vis_count;
}

void onLine(uint16_t, const uint16_t *, void *)
{
    ++s_line_count;
}

void onDone(void *)
{
    ++s_done_count;
}

} // namespace

static bool feedRobot36(const float gain, const int noise_peak)
{
    std::vector<uint16_t> image(SSTV_IMAGE_WIDTH * 240u, 0xFFFFu);
    SSTV_TxInit(SSTV_MODE_ROBOT36);
    if (!SSTV_TxSetImage(image.data(), SSTV_IMAGE_WIDTH, 240u)) {
        fprintf(stderr, "SSTV_TxSetImage failed\n");
        return false;
    }

    int16_t chunk[SSTV_CHUNK_SAMPLES];
    bool more = true;
    while (more) {
        more = SSTV_TxFillChunk(chunk);
        for (size_t i = 0u; i < SSTV_CHUNK_SAMPLES; ++i) {
            s_noise_state = s_noise_state * 1664525u + 1013904223u;
            const int noise = noise_peak == 0 ? 0 :
                static_cast<int>((s_noise_state >> 16u) % (noise_peak * 2 + 1)) - noise_peak;
            const int sample = static_cast<int>(lroundf(static_cast<float>(chunk[i]) * gain)) + noise;
            chunk[i] = static_cast<int16_t>(sample);
        }
        SSTV_RX_Feed(chunk, SSTV_CHUNK_SAMPLES);
    }
    return true;
}

static bool runRobot36(const float gain, const int noise_peak)
{
    s_vis_count = 0u;
    s_line_count = 0u;
    s_done_count = 0u;
    s_callback_error = false;
    SSTV_RX_Init(onVis, onLine, onDone, nullptr);
    if (!feedRobot36(gain, noise_peak)) return false;

    printf("gain=%.3f noise=%d: vis=%u lines=%u done=%u state=%u quality=%u\n",
           static_cast<double>(gain), noise_peak, s_vis_count, s_line_count, s_done_count,
           static_cast<unsigned>(SSTV_RX_GetState()),
           static_cast<unsigned>(SSTV_RX_SignalQuality()));
    return !s_callback_error && s_vis_count == 1u && s_line_count == 240u &&
           s_done_count == 1u;
}

static bool runSequentialRobot36()
{
    s_vis_count = 0u;
    s_line_count = 0u;
    s_done_count = 0u;
    s_callback_error = false;
    SSTV_RX_Init(onVis, onLine, onDone, nullptr);
    if (!feedRobot36(0.05f, 100)) return false;

    // Half a second of room-like low noise between phone playback attempts.
    int16_t gap[SSTV_CHUNK_SAMPLES];
    for (unsigned chunk = 0u; chunk < 50u; ++chunk) {
        for (size_t i = 0u; i < SSTV_CHUNK_SAMPLES; ++i) {
            s_noise_state = s_noise_state * 1664525u + 1013904223u;
            gap[i] = static_cast<int16_t>(static_cast<int>((s_noise_state >> 16u) % 41u) - 20);
        }
        SSTV_RX_Feed(gap, SSTV_CHUNK_SAMPLES);
    }
    if (!feedRobot36(0.02f, 20)) return false;

    printf("sequential: vis=%u lines=%u done=%u state=%u quality=%u\n",
           s_vis_count, s_line_count, s_done_count,
           static_cast<unsigned>(SSTV_RX_GetState()),
           static_cast<unsigned>(SSTV_RX_SignalQuality()));
    return !s_callback_error && s_vis_count == 2u && s_line_count == 480u &&
           s_done_count == 2u;
}

int main()
{
    bool ok = true;
    ok = runRobot36(1.0f, 0) && ok;
    ok = runRobot36(0.05f, 100) && ok;
    ok = runRobot36(0.02f, 20) && ok;
    ok = runSequentialRobot36() && ok;
    if (!ok) {
        fprintf(stderr, "sstv_rx_test: FAILED\n");
        return 1;
    }
    printf("sstv_rx_test: PASS\n");
    return 0;
}
