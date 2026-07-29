// SSTV receive demodulator, see sstv_rx.h for the module contract.
//
// DSP chain per fed sample (16 kHz int16):
//   1. Quadrature mix against a 1900 Hz NCO, single-pole I/Q low-pass, then a
//      phase-difference discriminator: sin(dphi) = (I*Qprev - Q*Iprev) / |Z|^2
//      scaled back to Hz. Chosen over zero-crossing because it stays accurate
//      for the fast 275 us Robot-36 pixels (4.4 samples/px), where a
//      zero-crossing estimator sees barely five cycles of sub-carrier.
//   2. Sliding Goertzel at 1200 Hz (64-sample window, O(1) per sample) with
//      Schmitt thresholds for sync/leader detection.
// The line/state machine re-anchors on every sync leading edge and samples
// pixels at segment-relative centers using the TX timing table.

#include "sstv_rx.h"

#include <math.h>
#include <string.h>

namespace {

constexpr uint32_t kSampleRate = SSTV_SAMPLE_RATE_HZ;
constexpr uint16_t kSinBits = 10u;
constexpr size_t kSinEntries = 1u << kSinBits;
constexpr float kCenterHz = 1900.0f;
constexpr float kIqAlpha = 0.15f;  // I/Q low-pass (swept: 0.15 wins clean+noise)
constexpr float kFreqBeta = 0.3f;   // discriminator output smoothing
constexpr uint16_t kGoertzelN = 80u; // 5 ms window: exactly 6 cycles of 1200 Hz
// (a sliding Goertzel needs an integer number of cycles per window, otherwise
// the comb zero misses the resonator pole and the output grows unbounded)
constexpr float kToneOnRatio = 0.45f;
constexpr float kToneOffRatio = 0.25f;
constexpr uint32_t kLeaderMinSamples = 3520u; // 220 ms of the 300 ms leader
constexpr uint16_t kVisSlotSamples = 480u;    // 30 ms
constexpr uint16_t kVisMargin = 120u;         // measure the middle 50% of a slot
constexpr int kSyncLeadBack = 54;  // Goertzel 'on' crossing lags tone begin ~0.67*N
constexpr uint16_t kWidth = SSTV_IMAGE_WIDTH;

constexpr uint8_t kVisRobot36 = 8u;
constexpr uint8_t kVisMartinM1 = 44u;

// Per-mode timing in samples (mirror of sstv_codec.cpp).
struct ModeTiming {
    uint32_t sync_len;
    uint32_t porch_len;
    uint32_t scan_len;   // Y (Robot) or RGB channel (Martin)
    uint32_t skip_len;   // between Y and chroma (Robot) / RGB channels (Martin)
    uint32_t chroma_len; // Robot only
    uint32_t line_period;
    uint16_t lines;      // image lines
    uint16_t groups;     // Robot: 2-line groups; Martin: lines
};
constexpr ModeTiming kTimingRobot = {144u, 48u, 1408u, 96u, 704u, 4800u, 240u, 120u};
constexpr ModeTiming kTimingMartin = {78u, 9u, 2343u, 9u, 0u, 7143u, 256u, 256u};

int16_t s_sin[kSinEntries];
bool s_sin_ready = false;

SstvRxVisCallback s_on_vis = nullptr;
SstvRxLineCallback s_on_line = nullptr;
SstvRxDoneCallback s_on_done = nullptr;
void *s_user = nullptr;

// DSP state.
uint32_t s_nco = 0u;
float s_i = 0.0f;
float s_q = 0.0f;
float s_i_prev = 0.0f;
float s_q_prev = 0.0f;
float s_freq = kCenterHz;
int16_t s_ring[kGoertzelN] = {};
uint16_t s_ring_pos = 0u;
float s_g1 = 0.0f;
float s_g2 = 0.0f;
float s_eng = 0.0f;
float s_ratio = 0.0f;

uint32_t s_index = 0u;
bool s_tone = false;
uint32_t s_tone_begin = 0u;

SstvRxState s_state = SSTV_RX_HUNT;
SSTV_Mode s_mode = SSTV_MODE_ROBOT36;
const ModeTiming *s_timing = &kTimingRobot;
uint16_t s_lines_received = 0u;

// VIS bit reader.
uint32_t s_leader_end = 0u;
uint8_t s_vis_slot = 0u; // 0=start, 1..7=data, 8=parity, 9=stop
uint8_t s_vis_code = 0u;
uint8_t s_vis_ones = 0u;
float s_slot_acc = 0.0f;
uint32_t s_slot_count = 0u;

// Line engine.
bool s_sampling = false;  // false = waiting for a sync
uint16_t s_group = 0u;    // Robot: group; Martin: line
uint8_t s_half = 0u;      // Robot: 0 = first line + R-Y, 1 = second + B-Y
uint8_t s_plan_pos = 0u;  // current scan segment within the plan
uint32_t s_seg_begin = 0u;
uint32_t s_last_sync = 0u;
uint16_t s_px = 0u;
float s_px_acc = 0.0f;
uint16_t s_px_count = 0u;

// Scan line buffers.
uint8_t s_ybuf[2][kWidth];  // Robot luminance of both group lines
int8_t s_chroma[2][160];    // [0]=R-Y, [1]=B-Y
uint8_t s_rgb[3][kWidth];   // Martin G, B, R
uint16_t s_row[kWidth];     // composed output row

// Robot odd/even slip recovery: Y pixels stage here until the separator
// after the scan (1500 Hz even / 2300 Hz odd) confirms which group half the
// line really is -- a missed sync would otherwise shift every later line.
uint8_t s_ystage[kWidth];
bool s_half_valid[2] = {false, false};
bool s_commit_pending = false; // Y scan done, separator not judged yet
uint32_t s_sep_win = 0u;       // separator window start (Y scan end)
float s_sep_acc = 0.0f;
uint16_t s_sep_count = 0u;

// Slant/clock-drift correction: incremental least squares over sync leading
// edges, measured position m ~= a + b*n with n = half-line (Robot) or line
// (Martin) sequence number. O(1) memory; double accumulators keep the
// products exact enough over a frame (n < 500, m < 2M).
double s_lsq_count = 0.0;
double s_lsq_sx = 0.0;
double s_lsq_sxx = 0.0;
double s_lsq_sy = 0.0;
double s_lsq_sxy = 0.0;

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

uint8_t freqToLevel(float mean)
{
    return static_cast<uint8_t>(clampf((mean - 1500.0f) * 255.0f / 800.0f, 0.0f, 255.0f) + 0.5f);
}

int8_t freqToChroma(float mean)
{
    const float v = clampf((mean - 1900.0f) * 256.0f / 800.0f, -128.0f, 127.0f);
    return static_cast<int8_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

uint16_t rgb565(int r, int g, int b)
{
    r = static_cast<int>(clampf(static_cast<float>(r), 0.0f, 255.0f));
    g = static_cast<int>(clampf(static_cast<float>(g), 0.0f, 255.0f));
    b = static_cast<int>(clampf(static_cast<float>(b), 0.0f, 255.0f));
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void emitRow(uint16_t y, const uint8_t *r, const uint8_t *g, const uint8_t *b)
{
    for (uint16_t x = 0u; x < kWidth; ++x) {
        s_row[x] = rgb565(r[x], g[x], b[x]);
    }
    if (s_on_line != nullptr) {
        s_on_line(y, s_row, s_user);
    }
}

// Robot group complete: YUV 4:2:0 -> two RGB rows (shared 2x2 chroma).
void emitRobotGroup(uint16_t group)
{
    uint8_t r_row[kWidth];
    uint8_t g_row[kWidth];
    uint8_t b_row[kWidth];
    for (uint16_t line = 0u; line < 2u; ++line) {
        for (uint16_t x = 0u; x < kWidth; ++x) {
            const float y = static_cast<float>(s_ybuf[line][x]);
            const float ry = static_cast<float>(s_chroma[0][x / 2u]);
            const float by = static_cast<float>(s_chroma[1][x / 2u]);
            const float r = y + ry;
            const float b = y + by;
            const float g = (y - 0.299f * r - 0.114f * b) / 0.587f;
            r_row[x] = static_cast<uint8_t>(clampf(r + 0.5f, 0.0f, 255.0f));
            g_row[x] = static_cast<uint8_t>(clampf(g + 0.5f, 0.0f, 255.0f));
            b_row[x] = static_cast<uint8_t>(clampf(b + 0.5f, 0.0f, 255.0f));
        }
        emitRow(static_cast<uint16_t>(group * 2u + line), r_row, g_row, b_row);
    }
}

// Nominal samples per sync sequence step (Robot: half line, Martin: line).
uint32_t seqPeriod()
{
    return (s_mode == SSTV_MODE_ROBOT36) ? s_timing->line_period / 2u
                                         : s_timing->line_period;
}

uint32_t seqNumber()
{
    return (s_mode == SSTV_MODE_ROBOT36) ? static_cast<uint32_t>(s_group) * 2u + s_half
                                         : s_group;
}

void lsqAdd(double n, double m)
{
    s_lsq_count += 1.0;
    s_lsq_sx += n;
    s_lsq_sxx += n * n;
    s_lsq_sy += m;
    s_lsq_sxy += n * m;
}

// Signal lost and reacquired: drop the fit and restart it from this point.
void lsqReset(double n, double m)
{
    s_lsq_count = 0.0;
    s_lsq_sx = 0.0;
    s_lsq_sxx = 0.0;
    s_lsq_sy = 0.0;
    s_lsq_sxy = 0.0;
    lsqAdd(n, m);
}

// Fitted sync position for sequence n; false while the fit is too young
// (<10 points) so the caller falls back to the raw per-line anchor.
bool lsqPredict(double n, double *out)
{
    if (s_lsq_count < 10.0) {
        return false;
    }
    const double denom = s_lsq_count * s_lsq_sxx - s_lsq_sx * s_lsq_sx;
    if (denom <= 0.0) {
        return false;
    }
    const double b = (s_lsq_count * s_lsq_sxy - s_lsq_sx * s_lsq_sy) / denom;
    const double a = (s_lsq_sy - b * s_lsq_sx) / s_lsq_count;
    *out = a + b * n;
    return true;
}

void finishLineSet()
{
    if (s_mode == SSTV_MODE_ROBOT36) {
        if (s_half == 1u) {
            // A sync lost mid-group leaves one half unsampled: repeat the
            // sibling luminance with neutral chroma instead of shifting the
            // whole frame by one line.
            if (!s_half_valid[0]) {
                memcpy(s_ybuf[0], s_ybuf[1], kWidth);
                memset(s_chroma[0], 0, 160u);
            }
            if (!s_half_valid[1]) {
                memcpy(s_ybuf[1], s_ybuf[0], kWidth);
                memset(s_chroma[1], 0, 160u);
            }
            s_half_valid[0] = false;
            s_half_valid[1] = false;
            emitRobotGroup(s_group);
            s_lines_received = static_cast<uint16_t>(s_lines_received + 2u);
            s_half = 0u;
            ++s_group;
            if (s_group >= s_timing->groups) {
                s_state = SSTV_RX_DONE;
                if (s_on_done != nullptr) {
                    s_on_done(s_user);
                }
            }
        } else {
            s_half = 1u;
        }
    } else {
        emitRow(s_group, s_rgb[2], s_rgb[0], s_rgb[1]); // R, G, B order
        s_lines_received = static_cast<uint16_t>(s_lines_received + 1u);
        ++s_group;
        if (s_group >= s_timing->groups) {
            s_state = SSTV_RX_DONE;
            if (s_on_done != nullptr) {
                s_on_done(s_user);
            }
        }
    }
}

void storePixel(uint8_t dest, uint16_t px)
{
    const float mean = s_px_count > 0u ? s_px_acc / s_px_count : kCenterHz;
    if (s_mode == SSTV_MODE_ROBOT36) {
        if (dest == 0u) {
            if (px < kWidth) {
                s_ystage[px] = freqToLevel(mean); // committed after the separator check
            }
        } else if (px < 160u) {
            s_chroma[s_half][px] = freqToChroma(mean);
        }
    } else if (px < kWidth) {
        s_rgb[dest][px] = freqToLevel(mean);
    }
}

// The Y scan of a Robot half-line just ended and the separator was measured:
// 1500 Hz = even half (R-Y follows), 2300 Hz = odd half (B-Y follows). Only a
// confident reading overrides the expected half; on a flip the Y staging is
// committed to the corrected half and a skipped group boundary is caught up.
void commitRobotHalf()
{
    if (s_sep_count > 0u) {
        const float sep = s_sep_acc / s_sep_count;
        uint8_t actual = s_half;
        if (sep < 1750.0f) {
            actual = 0u;
        } else if (sep > 2050.0f) {
            actual = 1u;
        }
        if (actual != s_half) {
            if (actual == 0u) {
                // Expected the odd half of group g but this is the even half
                // of g+1: a whole group boundary was missed. Emit g first
                // when its even half arrived (odd half gets the sibling fill)
                // so the missed sync costs nothing but one copied line.
                if (s_half_valid[0]) {
                    memcpy(s_ybuf[1], s_ybuf[0], kWidth);
                    memset(s_chroma[1], 0, 160u);
                    s_half_valid[0] = false;
                    s_half_valid[1] = false;
                    emitRobotGroup(s_group);
                    s_lines_received = static_cast<uint16_t>(s_lines_received + 2u);
                }
                if (s_group + 1u < s_timing->groups) {
                    ++s_group;
                }
            }
            s_half = actual;
        }
    }
    memcpy(s_ybuf[s_half], s_ystage, kWidth);
    s_half_valid[s_half] = true;
    s_sep_acc = 0.0f;
    s_sep_count = 0u;
}

void startLines(SSTV_Mode mode)
{
    s_mode = mode;
    s_timing = (mode == SSTV_MODE_ROBOT36) ? &kTimingRobot : &kTimingMartin;
    s_state = SSTV_RX_LINES;
    s_lines_received = 0u;
    s_group = 0u;
    s_half = 0u;
    // The first image sync starts exactly one VIS header (10 x 480 samples)
    // after slot 0, so the first line can be planned deterministically; its
    // sync tone is usually still merged with the leader/stop tone in the
    // Goertzel and could not anchor cleanly anyway. Later lines re-anchor on
    // their own syncs. This synthetic point also seeds the slant fit.
    lsqReset(0.0, static_cast<double>(s_leader_end + 4800u));
    s_half_valid[0] = false;
    s_half_valid[1] = false;
    s_sampling = true;
    s_plan_pos = 0u;
    s_last_sync = s_leader_end + 4800u;
    s_seg_begin = s_last_sync + s_timing->sync_len + s_timing->porch_len;
    s_sep_win = s_seg_begin + s_timing->scan_len;
    s_px = 0u;
    s_px_acc = 0.0f;
    s_px_count = 0u;
    s_sep_acc = 0.0f;
    s_sep_count = 0u;
    s_commit_pending = false;
    if (s_on_vis != nullptr) {
        s_on_vis(mode, s_user);
    }
}

// Decide the VIS slot that just closed; returns false to abort to HUNT.
bool visSlotDone()
{
    if (s_slot_count == 0u) {
        return false;
    }
    const float mean = s_slot_acc / s_slot_count;
    if (s_vis_slot == 0u) {
        return mean > 1200.0f; // start bit is 1300 Hz
    }
    if (s_vis_slot <= 7u) {
        const uint8_t bit = mean < 1200.0f ? 1u : 0u;
        s_vis_code = static_cast<uint8_t>(s_vis_code | (bit << (s_vis_slot - 1u)));
        s_vis_ones = static_cast<uint8_t>(s_vis_ones + bit);
        return true;
    }
    if (s_vis_slot == 8u) {
        const uint8_t parity = mean < 1200.0f ? 1u : 0u;
        return parity == (s_vis_ones & 1u);
    }
    // Stop bit ~1200 Hz, then the code must be a mode we speak.
    if (mean < 1100.0f || mean > 1300.0f) {
        return false;
    }
    if (s_vis_code == kVisRobot36) {
        startLines(SSTV_MODE_ROBOT36);
    } else if (s_vis_code == kVisMartinM1) {
        startLines(SSTV_MODE_MARTIN_M1);
    } else {
        return false; // known-but-unsupported or garbage: keep listening
    }
    return true;
}

void processSample(const int16_t x)
{
    // --- quadrature discriminator ---
    s_nco += static_cast<uint32_t>(((static_cast<uint64_t>(1900u) << 32u) / kSampleRate));
    const uint16_t idx = static_cast<uint16_t>(s_nco >> (32u - kSinBits));
    const float scale = 1.0f / 7000.0f; // table amplitude
    const float mix_i = static_cast<float>(x) *
                        static_cast<float>(s_sin[(idx + kSinEntries / 4u) % kSinEntries]) * scale;
    const float mix_q = static_cast<float>(x) * static_cast<float>(s_sin[idx]) * scale;
    s_i += kIqAlpha * (mix_i - s_i);
    s_q += kIqAlpha * (mix_q - s_q);
    const float num = s_i * s_q_prev - s_q * s_i_prev;
    const float mag2 = s_i * s_i + s_q * s_q + 1.0f;
    const float inst = kCenterHz + num / mag2 * (static_cast<float>(kSampleRate) / 6.283185307179586f);
    s_freq += kFreqBeta * (inst - s_freq);
    s_i_prev = s_i;
    s_q_prev = s_q;

    // --- sliding Goertzel at 1200 Hz ---
    constexpr float kGCoef = 2.0f * 0.8910065242f; // 2*cos(2*pi*1200/16000)
    const float oldest = static_cast<float>(s_ring[s_ring_pos]);
    s_ring[s_ring_pos] = x;
    s_ring_pos = static_cast<uint16_t>((s_ring_pos + 1u) % kGoertzelN);
    const float g = static_cast<float>(x) - oldest + kGCoef * s_g1 - s_g2;
    s_g2 = s_g1;
    s_g1 = g;
    const float power = s_g1 * s_g1 + s_g2 * s_g2 - kGCoef * s_g1 * s_g2;
    const float xf = static_cast<float>(x);
    s_eng += (xf * xf - s_eng) * (2.0f / kGoertzelN);
    s_ratio = power / (s_eng * (static_cast<float>(kGoertzelN) * kGoertzelN / 2.0f) + 1.0f);

    // --- sync tone Schmitt ---
    const bool on = s_tone ? s_ratio > kToneOffRatio : s_ratio > kToneOnRatio;
    if (on && !s_tone) {
        s_tone = true;
        s_tone_begin = s_index >= static_cast<uint32_t>(kSyncLeadBack)
                           ? s_index - static_cast<uint32_t>(kSyncLeadBack)
                           : 0u;
    } else if (!on && s_tone) {
        s_tone = false;
    }

    // --- line/header state machine ---
    if (s_state == SSTV_RX_HUNT || s_state == SSTV_RX_DONE) {
        // Leader: a continuous 1200 Hz tone >= 220 ms can only be the 300 ms
        // VIS leader. The VIS slots then start exactly one leader length
        // (4800 samples) after the tone began -- the 1100/1300 Hz VIS bits
        // are too close to 1200 Hz for the Goertzel to gate them, so the
        // slot anchor derives from the tone START, not its end.
        if (s_tone && s_index - s_tone_begin >= kLeaderMinSamples &&
            s_index - s_tone_begin < kLeaderMinSamples + 4800u) {
            s_leader_end = s_tone_begin + 4800u;
            s_vis_slot = 0u;
            s_vis_code = 0u;
            s_vis_ones = 0u;
            s_slot_acc = 0.0f;
            s_slot_count = 0u;
            s_state = SSTV_RX_VIS;
        }
    } else if (s_state == SSTV_RX_VIS) {
        const uint32_t slot_start = s_leader_end + static_cast<uint32_t>(s_vis_slot) * kVisSlotSamples;
        if (s_index >= slot_start + kVisMargin &&
            s_index < slot_start + kVisSlotSamples - kVisMargin) {
            s_slot_acc += s_freq;
            ++s_slot_count;
        }
        if (s_index + 1u >= slot_start + kVisSlotSamples) {
            if (!visSlotDone()) {
                s_state = SSTV_RX_HUNT;
            } else if (s_state == SSTV_RX_VIS) {
                ++s_vis_slot;
            }
            s_slot_acc = 0.0f;
            s_slot_count = 0u;
        }
    } else { // SSTV_RX_LINES
        if (!s_sampling) {
            if (s_index - s_last_sync > 2u * s_timing->line_period) {
                s_state = SSTV_RX_HUNT; // starved or signal lost
            } else if (s_tone &&
                       s_index - s_tone_begin >= (s_timing->sync_len * 2u) / 3u &&
                       s_index - s_tone_begin < s_timing->line_period) {
                // Anchored. With a mature slant fit (>=10 syncs) the fitted
                // position wins while the residual is small; a huge residual
                // means the signal was lost and reacquired, so the fit
                // restarts there.
                const double n = static_cast<double>(seqNumber());
                const double m = static_cast<double>(s_tone_begin);
                uint32_t anchor = s_tone_begin;
                double pred = 0.0;
                if (lsqPredict(n, &pred)) {
                    const double resid = m - pred;
                    if (resid > -40.0 && resid < 40.0) {
                        anchor = static_cast<uint32_t>(pred + 0.5);
                    } else if (resid > seqPeriod() / 4.0 || resid < -(seqPeriod() / 4.0)) {
                        lsqReset(n, m);
                    }
                }
                lsqAdd(n, m);
                s_sampling = true;
                s_plan_pos = 0u;
                s_seg_begin = anchor + s_timing->sync_len + s_timing->porch_len;
                s_sep_win = s_seg_begin + s_timing->scan_len;
                s_px = 0u;
                s_px_acc = 0.0f;
                s_px_count = 0u;
                s_sep_acc = 0.0f;
                s_sep_count = 0u;
                s_last_sync = s_tone_begin;
            }
        } else {
            const bool robot = s_mode == SSTV_MODE_ROBOT36;
            if (robot && s_commit_pending && s_index >= s_seg_begin) {
                commitRobotHalf(); // separator read; before the first chroma pixel
                s_commit_pending = false;
            }
            if (robot && s_index >= s_sep_win && s_index < s_sep_win + 72u) {
                // Separator between the Y scan and the chroma scan: 72 samples
                // of 1500 Hz (even half) or 2300 Hz (odd half).
                s_sep_acc += s_freq;
                ++s_sep_count;
            }
            const uint32_t scan_len = (robot && s_plan_pos == 1u) ? s_timing->chroma_len
                                                                  : s_timing->scan_len;
            const uint16_t npx = (robot && s_plan_pos == 1u) ? 160u : kWidth;
            if (s_index >= s_seg_begin && s_index < s_seg_begin + scan_len) {
                const uint32_t pos = s_index - s_seg_begin;
                const uint16_t px = static_cast<uint16_t>(pos * npx / scan_len);
                if (px != s_px) {
                    storePixel(s_plan_pos, s_px);
                    s_px = px;
                    s_px_acc = 0.0f;
                    s_px_count = 0u;
                }
                s_px_acc += s_freq;
                ++s_px_count;
            }
            if (s_index + 1u >= s_seg_begin + scan_len) {
                storePixel(s_plan_pos, s_px);
                const uint8_t plan_count = robot ? 2u : 3u;
                if (s_plan_pos + 1u < plan_count) {
                    if (robot && s_plan_pos == 0u) {
                        s_commit_pending = true; // judge the separator at chroma start
                    }
                    ++s_plan_pos;
                    s_seg_begin += scan_len + s_timing->skip_len;
                    s_px = 0u;
                    s_px_acc = 0.0f;
                    s_px_count = 0u;
                } else {
                    s_sampling = false;
                    finishLineSet();
                }
            }
        }
    }
    ++s_index;
}

} // namespace

void SSTV_RX_Init(SstvRxVisCallback on_vis, SstvRxLineCallback on_line,
                  SstvRxDoneCallback on_done, void *user)
{
    if (!s_sin_ready) {
        for (size_t i = 0u; i < kSinEntries; ++i) {
            s_sin[i] = static_cast<int16_t>(
                sinf(2.0f * 3.14159265358979323846f * static_cast<float>(i) /
                     static_cast<float>(kSinEntries)) * 7000.0f);
        }
        s_sin_ready = true;
    }
    s_on_vis = on_vis;
    s_on_line = on_line;
    s_on_done = on_done;
    s_user = user;
    SSTV_RX_Reset();
}

void SSTV_RX_Reset(void)
{
    s_state = SSTV_RX_HUNT;
    s_lines_received = 0u;
    s_group = 0u;
    s_half = 0u;
    s_sampling = false;
    s_tone = false;
    s_vis_slot = 0u;
    s_vis_code = 0u;
    s_vis_ones = 0u;
    s_slot_acc = 0.0f;
    s_slot_count = 0u;
    s_px = 0u;
    s_px_acc = 0.0f;
    s_px_count = 0u;
    s_sep_acc = 0.0f;
    s_sep_count = 0u;
    s_commit_pending = false;
    s_half_valid[0] = false;
    s_half_valid[1] = false;
    s_lsq_count = 0.0;
    s_lsq_sx = 0.0;
    s_lsq_sxx = 0.0;
    s_lsq_sy = 0.0;
    s_lsq_sxy = 0.0;
    s_last_sync = s_index;
}

void SSTV_RX_Feed(const int16_t *samples, size_t count)
{
    if (samples == nullptr) {
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        processSample(samples[i]);
    }
}

SstvRxState SSTV_RX_GetState(void)
{
    return s_state;
}

SSTV_Mode SSTV_RX_GetMode(void)
{
    return s_mode;
}

uint16_t SSTV_RX_LinesReceived(void)
{
    return s_lines_received;
}

uint16_t SSTV_RX_LinesTotal(void)
{
    return s_timing->lines;
}

uint8_t SSTV_RX_SignalQuality(void)
{
    const float q = clampf(s_ratio * 100.0f, 0.0f, 100.0f);
    return static_cast<uint8_t>(q);
}
