#include "vox.h"

#include "status_io.h"

#include <math.h>
#include <stdint.h>

namespace {

constexpr int kMinOpenDb = -80;
constexpr int kMaxOpenDb = -10;
constexpr int kMinCloseDb = -90;
constexpr int kMaxCloseDb = -15;
constexpr uint16_t kMaxAttackMs = 500U;
constexpr uint16_t kMinHangMs = 100U;
constexpr uint16_t kMaxHangMs = 3000U;
constexpr unsigned kFrameMs = 10U;
constexpr float kSilenceDb = -100.0f;

bool s_enabled = false;
int s_open_db = -40;
int s_close_db = -48;
unsigned s_attack_frames = 3U;
unsigned s_hang_frames = 60U;
unsigned s_open_run = 0U;
unsigned s_quiet_run = 0U;
bool s_tx_active = false;
float s_level_db = kSilenceDb;

int clampInt(const int value, const int lo, const int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

} // namespace

extern "C" void VOX_Configure(const bool enabled, const int open_db, const int close_db,
                              const uint16_t attack_ms, const uint16_t hang_ms)
{
    s_enabled = enabled;
    s_open_db = clampInt(open_db, kMinOpenDb, kMaxOpenDb);
    s_close_db = clampInt(close_db, kMinCloseDb, kMaxCloseDb);
    if (s_close_db > s_open_db - 2) {
        s_close_db = s_open_db - 2;
    }
    s_attack_frames = clampInt(attack_ms, 0, kMaxAttackMs) / kFrameMs;
    s_hang_frames = clampInt(hang_ms, kMinHangMs, kMaxHangMs) / kFrameMs;
    s_open_run = 0U;
    s_quiet_run = 0U;
}

extern "C" void VOX_ProcessFrame(const int16_t *samples, const size_t count)
{
    if (samples == nullptr || count == 0U) {
        return;
    }

    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t sample = samples[i];
        sum_sq += sample * sample;
    }
    const double rms = sqrt(static_cast<double>(sum_sq) / static_cast<double>(count));
    s_level_db = (rms > 0.0)
                     ? 20.0f * log10f(static_cast<float>(rms / 32768.0))
                     : kSilenceDb;

    if (!s_enabled) {
        s_open_run = 0U;
        s_quiet_run = 0U;
        if (s_tx_active) {
            s_tx_active = false;
            STATUS_IO_SetSoftPtt(false);
        }
        return;
    }

    if (!s_tx_active) {
        if (s_level_db >= static_cast<float>(s_open_db)) {
            ++s_open_run;
            if (s_open_run >= s_attack_frames) {
                s_tx_active = true;
                s_open_run = 0U;
                s_quiet_run = 0U;
                STATUS_IO_SetSoftPtt(true);
            }
        } else {
            s_open_run = 0U;
        }
    } else {
        if (s_level_db < static_cast<float>(s_close_db)) {
            ++s_quiet_run;
            if (s_quiet_run >= s_hang_frames) {
                s_tx_active = false;
                s_quiet_run = 0U;
                STATUS_IO_SetSoftPtt(false);
            }
        } else {
            s_quiet_run = 0U;
        }
    }
}

extern "C" bool VOX_IsActive(void)
{
    return s_tx_active;
}

extern "C" float VOX_CurrentLevelDb(void)
{
    return s_level_db;
}
