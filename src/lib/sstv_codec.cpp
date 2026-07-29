// SSTV transmit modulator, see sstv_codec.h for the usage contract.
//
// Timing reference (de-facto standard, as emitted by MMSSTV; verified against
// the zero-crossing analysis at bruxy.regnet.cz/web/hamradio/EN/a-look-into-sstv-mode):
//
//   VIS header: 300 ms 1200 Hz leader, 30 ms 1300 Hz start bit, 7 data bits
//   LSB-first (1100 Hz = 1, 1300 Hz = 0, 30 ms each), even-parity bit 30 ms,
//   30 ms 1200 Hz stop bit. VIS codes: Robot 36 = 8, Martin M1 = 44.
//
//   Robot 36, per 2-line group (Y / R-Y / Y / B-Y):
//     sync 9.0 ms 1200 Hz, porch 3.0 ms 1500 Hz, Y scan 88.0 ms (320 px),
//     even separator 4.5 ms 1500 Hz, porch 1.5 ms 1900 Hz, R-Y scan 44 ms
//     (160 px); then sync/porch/Y again, odd separator 4.5 ms 2300 Hz, porch
//     1.5 ms 1900 Hz, B-Y scan 44 ms. Chroma: signed value mapped around
//     1900 Hz (1500..2300).
//
//   Martin M1, per line (G-B-R):
//     sync 4.862 ms 1200 Hz, porch 0.572 ms 1500 Hz, green scan 146.432 ms
//     (320 px), separator 0.572 ms 1500 Hz, blue scan, separator, red scan,
//     separator.
//
//   PD120, per 2-line group (Y0 / R-Y / B-Y / Y1, no separators; VIS 95):
//     sync 20 ms 1200 Hz, porch 2.08 ms 1500 Hz, then four components of
//     121.6 ms each (640 px). Chroma: full horizontal resolution, vertical
//     average of the group's two lines (classicsstv.com/pdmodes).
//
// All durations are converted to exact sample counts at 16 kHz below; scans
// that do not divide evenly distribute their pixels over the segment length
// (pixel boundaries computed from the segment-relative sample index).

#include "sstv_codec.h"

#include <math.h>
#include <string.h>

namespace {

constexpr uint32_t kSampleRate = SSTV_SAMPLE_RATE_HZ;
constexpr float kAmplitude = 7000.0f;
constexpr uint16_t kSinBits = 10u; // 1024-entry full-wave table
constexpr size_t kSinEntries = 1u << kSinBits;

// Sub-carrier frequencies (Hz).
constexpr uint16_t kFreqSync = 1200u;
constexpr uint16_t kFreqBlack = 1500u;
constexpr uint16_t kFreqWhite = 2300u;
constexpr uint16_t kFreqVisOne = 1100u;
constexpr uint16_t kFreqVisZero = 1300u;
constexpr uint16_t kFreqChromaCenter = 1900u;

constexpr uint8_t kVisRobot36 = 8u;
constexpr uint8_t kVisMartinM1 = 44u;
constexpr uint8_t kVisPd120 = 95u;

// Scan channels (s_seg_channel).
constexpr uint8_t kChY = 0u;  // Robot 36 luminance
constexpr uint8_t kChRy = 1u; // Robot 36 R-Y (even group scan)
constexpr uint8_t kChBy = 2u; // Robot 36 B-Y (odd group scan)
constexpr uint8_t kChG = 0u;  // Martin M1 green
constexpr uint8_t kChB = 1u;  // Martin M1 blue
constexpr uint8_t kChR = 2u;  // Martin M1 red

int16_t s_sin[kSinEntries];
bool s_sin_ready = false;

SSTV_Mode s_mode = SSTV_MODE_ROBOT36;
const uint16_t *s_image = nullptr;
uint16_t s_img_height = 0u;
uint16_t s_img_width = 0u; // 320 (Robot/Martin) or 640 (PD120)
uint8_t s_vis_code = 0u;
uint32_t s_total = 0u;
uint32_t s_elapsed = 0u;
uint32_t s_phase = 0u;
bool s_active = false;
bool s_finished = false;

// Current segment.
bool s_seg_scan = false;
uint16_t s_seg_freq = 0u;    // tone segments
uint8_t s_seg_channel = 0u;  // scan segments
uint16_t s_seg_line = 0u;    // scan: image line (Y/RGB) or line group (chroma)
uint16_t s_seg_pixels = 0u;  // scan: pixels across the segment
uint32_t s_seg_len = 0u;
uint32_t s_seg_pos = 0u;

// Sequencer: VIS header, then image lines/groups.
uint8_t s_stage = 0u;    // 0 = VIS, 1 = image, 2 = done
uint8_t s_vis_step = 0u; // 0..10
uint16_t s_line = 0u;    // Martin: line; Robot: 2-line group
uint8_t s_line_seg = 0u; // segment index within the line/group

void toneSegment(uint16_t freq, uint32_t samples)
{
    s_seg_scan = false;
    s_seg_freq = freq;
    s_seg_len = samples;
}

void scanSegment(uint8_t channel, uint16_t line, uint16_t pixels, uint32_t samples)
{
    s_seg_scan = true;
    s_seg_channel = channel;
    s_seg_line = line;
    s_seg_pixels = pixels;
    s_seg_len = samples;
}

// Map a 0..255 video level to the 1500..2300 Hz sub-carrier range.
uint16_t levelToFreq(uint8_t level)
{
    return static_cast<uint16_t>(kFreqBlack +
                                 (static_cast<uint32_t>(level) * 800u) / 255u);
}

// Signed chroma (-128..127) around the 1900 Hz center.
uint16_t chromaToFreq(int value)
{
    if (value < -128) value = -128;
    if (value > 127) value = 127;
    return static_cast<uint16_t>(kFreqChromaCenter + (value * 800) / 256);
}

void pixelRgb(uint16_t x, uint16_t y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    const uint16_t px = s_image[static_cast<uint32_t>(y) * s_img_width + x];
    *r = static_cast<uint8_t>((px >> 8u) & 0xF8u);
    *g = static_cast<uint8_t>((px >> 3u) & 0xFCu);
    *b = static_cast<uint8_t>((px << 3u) & 0xF8u);
}

uint8_t luma(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8u);
}

// Frequency for the scan sample at s_seg_pos: pixel boundaries derive from
// the segment-relative position, so a non-integer samples-per-pixel ratio
// spreads the rounding across the scan instead of accumulating drift.
uint16_t scanFreq()
{
    const uint32_t px = s_seg_pos * s_seg_pixels / s_seg_len;
    if (s_mode == SSTV_MODE_PD120) {
        if (s_seg_channel == kChY) {
            uint8_t r, g, b;
            pixelRgb(static_cast<uint16_t>(px), s_seg_line, &r, &g, &b);
            return levelToFreq(luma(r, g, b));
        }
        // PD chroma: full horizontal resolution, vertical average of the
        // group's two lines.
        uint32_t r_sum = 0u, g_sum = 0u, b_sum = 0u;
        const uint16_t row = static_cast<uint16_t>(s_seg_line * 2u);
        for (uint16_t dy = 0u; dy < 2u; ++dy) {
            uint8_t r, g, b;
            pixelRgb(static_cast<uint16_t>(px), static_cast<uint16_t>(row + dy), &r, &g, &b);
            r_sum += r;
            g_sum += g;
            b_sum += b;
        }
        const uint8_t r_avg = static_cast<uint8_t>(r_sum >> 1u);
        const uint8_t g_avg = static_cast<uint8_t>(g_sum >> 1u);
        const uint8_t b_avg = static_cast<uint8_t>(b_sum >> 1u);
        const int y_avg = luma(r_avg, g_avg, b_avg);
        const int diff = (s_seg_channel == kChRy) ? static_cast<int>(r_avg) - y_avg
                                                  : static_cast<int>(b_avg) - y_avg;
        return chromaToFreq(diff);
    }
    if (s_mode == SSTV_MODE_ROBOT36) {
        if (s_seg_channel == kChY) {
            uint8_t r, g, b;
            pixelRgb(static_cast<uint16_t>(px), s_seg_line, &r, &g, &b);
            return levelToFreq(luma(r, g, b));
        }
        // 4:2:0 chroma: average the 2x2 block this sample covers (both lines
        // of the group, two adjacent columns).
        uint32_t r_sum = 0u, g_sum = 0u, b_sum = 0u;
        const uint16_t col = static_cast<uint16_t>(px * 2u);
        const uint16_t row = static_cast<uint16_t>(s_seg_line * 2u);
        for (uint16_t dy = 0u; dy < 2u; ++dy) {
            for (uint16_t dx = 0u; dx < 2u; ++dx) {
                uint8_t r, g, b;
                pixelRgb(static_cast<uint16_t>(col + dx), static_cast<uint16_t>(row + dy),
                         &r, &g, &b);
                r_sum += r;
                g_sum += g;
                b_sum += b;
            }
        }
        const uint8_t r_avg = static_cast<uint8_t>(r_sum >> 2u);
        const uint8_t g_avg = static_cast<uint8_t>(g_sum >> 2u);
        const uint8_t b_avg = static_cast<uint8_t>(b_sum >> 2u);
        const int y_avg = luma(r_avg, g_avg, b_avg);
        const int diff = (s_seg_channel == kChRy) ? static_cast<int>(r_avg) - y_avg
                                                  : static_cast<int>(b_avg) - y_avg;
        return chromaToFreq(diff);
    }
    uint8_t r, g, b;
    pixelRgb(static_cast<uint16_t>(px), s_seg_line, &r, &g, &b);
    const uint8_t level = (s_seg_channel == kChG) ? g : (s_seg_channel == kChB) ? b : r;
    return levelToFreq(level);
}

// Load the next segment into s_seg_*. Returns false when the frame is done.
bool advanceSegment()
{
    if (s_stage == 0u) {
        // VIS header (durations in samples at 16 kHz).
        switch (s_vis_step) {
            case 0: toneSegment(kFreqSync, 4800u); break; // 300 ms leader
            case 1: toneSegment(kFreqVisZero, 480u); break; // start bit
            case 2: case 3: case 4: case 5: case 6: case 7: case 8: {
                const uint8_t bit = static_cast<uint8_t>((s_vis_code >> (s_vis_step - 2u)) & 1u);
                toneSegment(bit != 0u ? kFreqVisOne : kFreqVisZero, 480u);
                break;
            }
            case 9: {
                // Even parity over the 7 data bits (1100 Hz = 1).
                uint8_t ones = 0u;
                for (uint8_t i = 0u; i < 7u; ++i) {
                    ones = static_cast<uint8_t>(ones + ((s_vis_code >> i) & 1u));
                }
                toneSegment((ones & 1u) != 0u ? kFreqVisOne : kFreqVisZero, 480u);
                break;
            }
            default: toneSegment(kFreqSync, 480u); break; // stop bit
        }
        if (++s_vis_step > 10u) {
            s_stage = 1u;
            s_line = 0u;
            s_line_seg = 0u;
        }
    } else if (s_stage == 1u) {
        if (s_mode == SSTV_MODE_PD120) {
            // PD120 (classicsstv.com/pdmodes): 20 ms sync + 2.08 ms porch,
            // then Y0 / R-Y / B-Y / Y1 with NO separators; every component is
            // 121.6 ms (640 px). 248 double-lines.
            if (s_line >= 248u) {
                s_stage = 2u;
                return false;
            }
            const uint16_t group = s_line;
            switch (s_line_seg) {
                case 0: toneSegment(kFreqSync, 320u); break;  // 20 ms sync
                case 1: toneSegment(kFreqBlack, 33u); break;  // 2.08 ms porch
                case 2: scanSegment(kChY, static_cast<uint16_t>(group * 2u), 640u, 1946u); break;
                case 3: scanSegment(kChRy, group, 640u, 1946u); break;
                case 4: scanSegment(kChBy, group, 640u, 1946u); break;
                default: scanSegment(kChY, static_cast<uint16_t>(group * 2u + 1u), 640u, 1946u); break;
            }
            if (++s_line_seg >= 6u) {
                s_line_seg = 0u;
                ++s_line;
            }
        } else if (s_mode == SSTV_MODE_ROBOT36) {
            if (s_line >= 120u) {
                s_stage = 2u;
                return false;
            }
            const uint16_t group = s_line;
            switch (s_line_seg) {
                case 0: toneSegment(kFreqSync, 144u); break;          // 9.0 ms sync
                case 1: toneSegment(kFreqBlack, 48u); break;          // 3.0 ms porch
                case 2: scanSegment(kChY, static_cast<uint16_t>(group * 2u), 320u, 1408u); break;
                case 3: toneSegment(kFreqBlack, 72u); break;          // 4.5 ms even separator
                case 4: toneSegment(kFreqChromaCenter, 24u); break;   // 1.5 ms porch
                case 5: scanSegment(kChRy, group, 160u, 704u); break; // 44 ms R-Y
                case 6: toneSegment(kFreqSync, 144u); break;
                case 7: toneSegment(kFreqBlack, 48u); break;
                case 8: scanSegment(kChY, static_cast<uint16_t>(group * 2u + 1u), 320u, 1408u); break;
                case 9: toneSegment(kFreqWhite, 72u); break;          // 4.5 ms odd separator
                case 10: toneSegment(kFreqChromaCenter, 24u); break;
                default: scanSegment(kChBy, group, 160u, 704u); break; // 44 ms B-Y
            }
            if (++s_line_seg >= 12u) {
                s_line_seg = 0u;
                ++s_line;
            }
        } else {
            if (s_line >= 256u) {
                s_stage = 2u;
                return false;
            }
            switch (s_line_seg) {
                case 0: toneSegment(kFreqSync, 78u); break;        // 4.862 ms sync
                case 1: toneSegment(kFreqBlack, 9u); break;        // 0.572 ms porch
                case 2: scanSegment(kChG, s_line, 320u, 2343u); break; // 146.432 ms
                case 3: toneSegment(kFreqBlack, 9u); break;
                case 4: scanSegment(kChB, s_line, 320u, 2343u); break;
                case 5: toneSegment(kFreqBlack, 9u); break;
                case 6: scanSegment(kChR, s_line, 320u, 2343u); break;
                default: toneSegment(kFreqBlack, 9u); break;
            }
            if (++s_line_seg >= 8u) {
                s_line_seg = 0u;
                ++s_line;
            }
        }
    } else {
        return false;
    }
    s_seg_pos = 0u;
    return true;
}

} // namespace

void SSTV_TxInit(SSTV_Mode mode)
{
    if (!s_sin_ready) {
        for (size_t i = 0u; i < kSinEntries; ++i) {
            s_sin[i] = static_cast<int16_t>(
                sinf(2.0f * 3.14159265358979323846f * static_cast<float>(i) /
                     static_cast<float>(kSinEntries)) * kAmplitude);
        }
        s_sin_ready = true;
    }
    s_mode = mode;
    s_image = nullptr;
    s_img_height = 0u;
    s_img_width = 0u;
    s_vis_code = (mode == SSTV_MODE_ROBOT36) ? kVisRobot36 :
                 (mode == SSTV_MODE_MARTIN_M1) ? kVisMartinM1 : kVisPd120;
    // VIS header: 4800 + 10 x 480 samples. Robot: 120 groups x 4800 samples;
    // Martin: 256 lines x 7143; PD120: 248 double-lines x 8137.
    s_total = 9600u + ((mode == SSTV_MODE_ROBOT36) ? 120u * 4800u :
                       (mode == SSTV_MODE_MARTIN_M1) ? 256u * 7143u : 248u * 8137u);
    s_elapsed = 0u;
    s_phase = 0u;
    s_active = false;
    s_finished = false;
    s_stage = 0u;
    s_vis_step = 0u;
    s_line = 0u;
    s_line_seg = 0u;
    s_seg_len = 0u;
    s_seg_pos = 0u;
}

bool SSTV_TxSetImage(const uint16_t *rgb565, uint16_t width, uint16_t height)
{
    const uint16_t exp_w = (s_mode == SSTV_MODE_PD120) ? 640u : SSTV_IMAGE_WIDTH;
    const uint16_t exp_h = (s_mode == SSTV_MODE_ROBOT36) ? 240u :
                           (s_mode == SSTV_MODE_MARTIN_M1) ? 256u : 496u;
    if (rgb565 == nullptr || width != exp_w || height != exp_h) {
        s_active = false;
        return false;
    }
    s_image = rgb565;
    s_img_height = height;
    s_img_width = width;
    s_active = true;
    return true;
}

bool SSTV_TxFillChunk(int16_t *out)
{
    if (out == nullptr) {
        return false;
    }
    if (!s_active || s_finished) {
        memset(out, 0, SSTV_CHUNK_SAMPLES * sizeof(int16_t));
        return false;
    }
    for (size_t i = 0u; i < SSTV_CHUNK_SAMPLES; ++i) {
        if (s_seg_pos >= s_seg_len) {
            if (!advanceSegment()) {
                s_finished = true;
                memset(out + i, 0, (SSTV_CHUNK_SAMPLES - i) * sizeof(int16_t));
                return false;
            }
        }
        const uint32_t freq = s_seg_scan ? scanFreq() : s_seg_freq;
        s_phase += static_cast<uint32_t>((static_cast<uint64_t>(freq) << 32u) / kSampleRate);
        out[i] = s_sin[s_phase >> (32u - kSinBits)];
        ++s_seg_pos;
        ++s_elapsed;
    }
    return true;
}

uint32_t SSTV_TxTotalSamples(void)
{
    return s_total;
}

uint32_t SSTV_TxElapsedSamples(void)
{
    return s_elapsed;
}
