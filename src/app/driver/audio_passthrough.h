#ifndef DRIVER_AUDIO_PASSTHROUGH_H
#define DRIVER_AUDIO_PASSTHROUGH_H

// Board-neutral audio capture/playback pipeline that owns the I2S bus, the
// downlink output queue, the optional AEC reference buffer, the software mic
// high-pass filter, and the FreeRTOS passthrough task that pulls mic frames
// from I2S, hands them to AEC / the audio router (AUDIO_SRC_MIC), and writes
// DAC samples back to I2S. The playback queue is exposed to the router as
// AUDIO_SINK_SPEAKER; see src/audio/audio_router.h.
//
// The ADC behind I2S DIN differs per board:
//   bh4tdv  -> ES8311 (codec + ADC + DAC in one chip)
//   gezipai -> ES7210 captures the mic, ES8311 only drives the DAC
// Either way the data flow above is identical, so it lives in its own
// translation unit instead of being tangled with ES8311 register code.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <driver/i2s_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_MODE_RECEIVE = 0,   // Full-duplex bridge: ADC captures MIC, DAC plays NRL downlink.
} AUDIO_Mode_t;

// Pipeline lifecycle. The codec init path needs I2S running (for MCLK) before
// it can talk to the codec, so the codec driver calls AUDIO_SetupI2S() first
// and then AUDIO_StartPassthrough() once register config has completed.
bool AUDIO_SetupI2S(void);
bool AUDIO_StartPassthrough(void);
void AUDIO_StopPassthrough(void);
bool AUDIO_IsPassthroughRunning(void);
bool AUDIO_GetI2SHandles(i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle);

// Temporarily retime the shared I2S TX channel for native-rate media output.
// The caller must stop the passthrough task first and must restore 16 kHz
// before restarting it. RX stays allocated but is not consumed while media
// owns the bus, matching the ES8389 codec-dev playback lifecycle.
bool AUDIO_ReconfigureOutput(uint32_t sample_rate_hz, uint8_t bits_per_sample);
// Blocking raw write to the currently configured stereo I2S TX channel.
bool AUDIO_WriteOutput(const void *pcm, size_t bytes);

// The codec driver tracks the active audio mode by calling AUDIO_SetMode()
// whenever it switches the DAC power path; the passthrough task reads it
// back to forward to the frame hook.
void AUDIO_SetMode(AUDIO_Mode_t mode);
AUDIO_Mode_t AUDIO_GetMode(void);

// Queue PCM16 mono samples (16 kHz voice domain) for DAC playback. This is
// the backend of the router's AUDIO_SINK_SPEAKER (registered at 16 kHz;
// 8 kHz sources are upsampled by the router at delivery); producers should
// route through the audio router rather than calling it directly.
size_t AUDIO_QueueOutputSamples(const int16_t *samples, size_t sample_count);
void AUDIO_ClearOutputQueue(void);

// AEC reference source: 0 = delayed network playback, 1 = second I2S input
// channel. Takes effect immediately while the resident AFE keeps running.
void AUDIO_SetAecReferenceSource(uint8_t source);

// Software high-pass filter on captured mic audio (4th-order Butterworth IIR,
// ~200 Hz cutoff at 16 kHz). Strips DC offset and low-frequency rumble
// before AEC / network uplink. Stateful: toggling resets the filter memory
// to avoid a transient on the first frame.
void AUDIO_SetMicHpfEnabled(bool enabled);
// Debug self-test: run synthetic DC and 8 kHz square-wave frames through the
// mic HPF and report output RMS. Filter state is saved/restored.
void AUDIO_MicHpfSelfTest(float *dc_out_rms, float *square_out_rms);
bool AUDIO_GetMicHpfEnabled(void);

// Saturating software gain on PCM16 immediately after ADC/I2S capture.
// `gain_milli` is 100..5000, where 1000 means 1.0x.
void AUDIO_SetMicPcmGain(uint16_t gain_milli);
uint16_t AUDIO_GetMicPcmGain(void);

// Audio parameters mirrored to callers that need to size buffers.
int AUDIO_GetSampleRate(void);
size_t AUDIO_GetFrameSamples(void);

// Test/diagnostic utilities. These stop the passthrough task while running.
bool AUDIO_PlayTestTone(uint32_t durationMs);
bool AUDIO_RecordMicAndPlayback(uint32_t durationMs);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_AUDIO_PASSTHROUGH_H
