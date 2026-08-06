#include "audio/audio_router.h"

#include <esp_log.h>

#include <string.h>

static const char *TAG = "AROUTER";

namespace {

// Scratch size for one rate-conversion chunk. Chosen to hold a full 10 ms
// 16 kHz frame (160 samples) so the common mic push converts in one pass;
// larger inputs (e.g. 500-byte downlink chunks) loop.
constexpr size_t kConvertChunkSamples = 160;

struct SinkSlot {
    AudioRouterSinkWrite_t write;
    void *user_data;
    uint32_t sample_rate_hz;
    bool registered;
};

static SinkSlot s_sinks[AUDIO_SINK_COUNT] = {};
static volatile bool s_route_enabled[AUDIO_SRC_COUNT][AUDIO_SINK_COUNT] = {};
static volatile uint16_t s_route_gain_q8[AUDIO_SRC_COUNT][AUDIO_SINK_COUNT] = {};
static bool s_rate_warned = false;

static inline int16_t saturate16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return static_cast<int16_t>(value);
}

static inline void apply_gain(int16_t *samples, const size_t count, const uint16_t gain_q8)
{
    for (size_t i = 0; i < count; ++i) {
        samples[i] = saturate16((static_cast<int32_t>(samples[i]) * gain_q8) >> 8);
    }
}

// 16k -> 8k: average each sample pair. Identical to the pre-router
// downsample16kTo8kForG711 so the mic->NRL uplink stays bit-exact.
static size_t downsample_2to1(const int16_t *src, const size_t src_count,
                              int16_t *dst, const size_t dst_capacity)
{
    const size_t pairs = src_count / 2u;
    const size_t out = (pairs < dst_capacity) ? pairs : dst_capacity;
    for (size_t i = 0; i < out; ++i) {
        const int32_t a = src[i * 2u];
        const int32_t b = src[i * 2u + 1u];
        dst[i] = static_cast<int16_t>((a + b) / 2);
    }
    return out;
}

// 8k -> 16k: linear interpolation with cross-chunk continuity. `last` carries
// the final sample of the previous chunk so the first interpolated pair of
// the next chunk bridges the boundary smoothly instead of repeating a value
// (which created a 10 ms "flat spot" that broke MDC1200 four-point decode).
static size_t upsample_1to2_cont(const int16_t *src, const size_t src_count,
                                 int16_t *dst, const size_t dst_capacity,
                                 int16_t &last)
{
    const size_t out_pairs = (src_count * 2u < dst_capacity) ? src_count : dst_capacity / 2u;
    for (size_t i = 0; i < out_pairs; ++i) {
        const int16_t current = src[i];
        dst[i * 2u] = static_cast<int16_t>(
            (static_cast<int32_t>(last) + static_cast<int32_t>(current)) / 2);
        dst[i * 2u + 1u] = current;
        last = current;
    }
    return out_pairs * 2u;
}

static void deliver_converted(const uint8_t source_id,
                              const uint8_t sink_id,
                              const SinkSlot &sink,
                              const uint32_t src_rate,
                              const uint16_t gain_q8,
                              const int16_t *samples,
                              const size_t sample_count)
{
    int16_t scratch[kConvertChunkSamples];

    if (src_rate == 16000u && sink.sample_rate_hz == 8000u) {
        size_t offset = 0;
        while (offset < sample_count) {
            const size_t take = ((sample_count - offset) < kConvertChunkSamples * 2u)
                                    ? (sample_count - offset)
                                    : kConvertChunkSamples * 2u;
            const size_t out = downsample_2to1(samples + offset, take, scratch, kConvertChunkSamples);
            if (out == 0u) {
                break;
            }
            if (gain_q8 != AUDIO_ROUTER_GAIN_UNITY) {
                apply_gain(scratch, out, gain_q8);
            }
            sink.write(source_id, scratch, out, sink.user_data);
            offset += take;
        }
        return;
    }

    if (src_rate == 8000u && sink.sample_rate_hz == 16000u) {
        // Per-sink state: carry the last 8 kHz sample across PushFrame calls
        // so the interpolated stream has no boundary discontinuities.
        static int16_t s_upsample_last[AUDIO_SINK_COUNT] = {};
        int16_t &last = s_upsample_last[sink_id < AUDIO_SINK_COUNT ? sink_id : 0];
        size_t offset = 0;
        while (offset < sample_count) {
            const size_t take = ((sample_count - offset) < kConvertChunkSamples / 2u)
                                    ? (sample_count - offset)
                                    : kConvertChunkSamples / 2u;
            const size_t out = upsample_1to2_cont(samples + offset, take, scratch,
                                                  kConvertChunkSamples, last);
            if (out == 0u) {
                break;
            }
            if (gain_q8 != AUDIO_ROUTER_GAIN_UNITY) {
                apply_gain(scratch, out, gain_q8);
            }
            sink.write(source_id, scratch, out, sink.user_data);
            offset += take;
        }
        return;
    }

    if (!s_rate_warned) {
        s_rate_warned = true;
        ESP_LOGW(TAG, "unsupported rate conversion %lu -> %lu Hz, frames dropped",
                 static_cast<unsigned long>(src_rate),
                 static_cast<unsigned long>(sink.sample_rate_hz));
    }
}

} // namespace

extern "C" bool AudioRouter_RegisterSink(const uint8_t sink_id,
                                         const uint32_t sample_rate_hz,
                                         const AudioRouterSinkWrite_t write,
                                         void *user_data)
{
    if (sink_id >= AUDIO_SINK_COUNT || write == nullptr || sample_rate_hz == 0u) {
        return false;
    }
    s_sinks[sink_id].write = write;
    s_sinks[sink_id].user_data = user_data;
    s_sinks[sink_id].sample_rate_hz = sample_rate_hz;
    s_sinks[sink_id].registered = true;
    return true;
}

extern "C" bool AudioRouter_SetRoute(const uint8_t source_id, const uint8_t sink_id, const bool enabled)
{
    if (source_id >= AUDIO_SRC_COUNT || sink_id >= AUDIO_SINK_COUNT) {
        return false;
    }
    if (s_route_gain_q8[source_id][sink_id] == 0u) {
        s_route_gain_q8[source_id][sink_id] = AUDIO_ROUTER_GAIN_UNITY;
    }
    s_route_enabled[source_id][sink_id] = enabled;
    return true;
}

extern "C" bool AudioRouter_SetRouteGain(const uint8_t source_id, const uint8_t sink_id, const uint16_t gain_q8)
{
    if (source_id >= AUDIO_SRC_COUNT || sink_id >= AUDIO_SINK_COUNT) {
        return false;
    }
    s_route_gain_q8[source_id][sink_id] = gain_q8;
    return true;
}

extern "C" void AudioRouter_PushFrame(const uint8_t source_id,
                                      const uint32_t sample_rate_hz,
                                      const int16_t *samples,
                                      const size_t sample_count)
{
    if (source_id >= AUDIO_SRC_COUNT || samples == nullptr || sample_count == 0u) {
        return;
    }

    for (uint8_t sink_id = 0; sink_id < AUDIO_SINK_COUNT; ++sink_id) {
        if (!s_route_enabled[source_id][sink_id]) {
            continue;
        }
        const SinkSlot &sink = s_sinks[sink_id];
        if (!sink.registered) {
            continue;
        }

        const uint16_t gain_q8 = s_route_gain_q8[source_id][sink_id];
        if (sink.sample_rate_hz == sample_rate_hz) {
            if (gain_q8 != AUDIO_ROUTER_GAIN_UNITY) {
                int16_t scratch[kConvertChunkSamples];
                size_t offset = 0;
                while (offset < sample_count) {
                    const size_t take = ((sample_count - offset) < kConvertChunkSamples)
                                            ? (sample_count - offset)
                                            : kConvertChunkSamples;
                    memcpy(scratch, samples + offset, take * sizeof(int16_t));
                    apply_gain(scratch, take, gain_q8);
                    sink.write(source_id, scratch, take, sink.user_data);
                    offset += take;
                }
            } else {
                sink.write(source_id, samples, sample_count, sink.user_data);
            }
            continue;
        }

        deliver_converted(source_id, sink_id, sink, sample_rate_hz, gain_q8, samples, sample_count);
    }
}
