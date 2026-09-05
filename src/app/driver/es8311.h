#ifndef DRIVER_ES8311_H
#define DRIVER_ES8311_H

#include "driver/audio_passthrough.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the ES8311 codec: configure I2S clocks, the codec via I2C, then
// start the audio passthrough task. On boards where the ES8311 also drives
// the DAC only (gezipai), the mic path is via ES7210 over the same I2S bus.
bool ES8311_Init(void);

// True if the codec initialization sequence already succeeded.
bool ES8311_IsReady(void);

// Debug/diagnostics: raw register read over the shared I2C bus. Returns false
// when the bus transaction fails (e.g. no codec at 0x18 on this board).
bool ES8311_ReadReg(uint8_t reg, uint8_t *value);
bool ES8311_WriteReg(uint8_t reg, uint8_t value);

// Switch the codec's external audio routing for a given audio mode.
bool ES8311_SetAudioMode(AUDIO_Mode_t mode);
bool ES8311_SetReceiveMode(void);

// Temporarily hand the shared I2S/DAC path to native-rate media playback.
// ES8311 has a mono DAC, so HifiWrite accepts stereo PCM16 and downmixes it
// to mono while retaining the two-slot I2S wire format used by every board.
bool ES8311_HifiAcquire(uint32_t sample_rate_hz, uint8_t bits_per_sample, uint8_t channels);
bool ES8311_HifiWrite(const void *pcm, size_t bytes);
bool ES8311_HifiRelease(void);
bool ES8311_HifiActive(void);

// Push the full per-codec audio configuration (volumes, DRC, ADC config, EQ
// coefficients) into the ES8311 registers. Mirrors the persisted config in
// ExternalRadioConfig.
bool ES8311_ApplyAudioConfig(uint8_t mic_volume,
                             uint8_t line_out_volume,
                             bool hp_drive_enabled,
                             bool drc_enabled,
                             uint8_t drc_winsize,
                             uint8_t drc_maxlevel,
                             uint8_t drc_minlevel,
                             uint8_t dac_ramprate,
                             bool dac_eq_bypass,
                             uint32_t daceq_b0,
                             uint32_t daceq_b1,
                             uint32_t daceq_a1,
                             bool adc_dmic_enabled,
                             bool adc_linsel,
                             uint8_t adc_pga_gain,
                             uint8_t adc_ramprate,
                             bool adc_dmic_sense,
                             bool adc_sync,
                             bool adc_inv,
                             bool adc_ramclr,
                             uint8_t adc_scale,
                             bool alc_enabled,
                             bool adc_automute_enabled,
                             uint8_t alc_winsize,
                             uint8_t alc_maxlevel,
                             uint8_t alc_minlevel,
                             uint8_t adc_automute_winsize,
                             uint8_t adc_automute_noise_gate,
                             uint8_t adc_automute_volume,
                             uint8_t adc_hpfs1,
                             bool adc_eq_bypass,
                             bool adc_hpf,
                             uint8_t adc_hpfs2,
                             uint32_t adceq_b0,
                             uint32_t adceq_a1,
                             uint32_t adceq_a2,
                             uint32_t adceq_b1,
                             uint32_t adceq_b2);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_ES8311_H
