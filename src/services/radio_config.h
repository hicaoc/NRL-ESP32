#ifndef SERVICES_RADIO_CONFIG_H
#define SERVICES_RADIO_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SR-110U RF module configuration (BH4TDV-RF board). Persisted in NVS and
// pushed to the module over its UART using the simplified command set
// (AT+DMOGRP / AT+DMOFUN / AT+DMOVOL / AT+DMOSAV / AT+DMOVOX per the SR-110U
// V303 datasheet). All mutating APIs validate ranges; nothing reaches flash
// or the module when validation fails.

enum RadioToneType : uint8_t {
    RADIO_TONE_NONE = 0, // sent to the module as raw bytes FF FF
    RADIO_TONE_CTCSS,    // value = frequency x10, e.g. 670 = 67.0 Hz
    RADIO_TONE_CDCSS_N,  // value = code number, e.g. 23 = D023N
    RADIO_TONE_CDCSS_I,  // value = code number, e.g. 251 = D251I
};

struct RadioTone {
    uint8_t type;   // RadioToneType
    uint16_t value;
};

struct RadioModuleConfig {
    bool enabled;        // module power (PD pin via PCA9555)
    uint32_t rx_freq_hz; // 400000000..480000000, multiple of 2500
    uint32_t tx_freq_hz;
    RadioTone rx_tone;
    RadioTone tx_tone;
    bool busy_lockout;   // DMOGRP Flag bit0: busy channel lockout (遇忙禁发)
    bool narrowband;     // DMOGRP Flag bit1: 0=wide 1=narrow
    bool low_power;      // DMOGRP Flag1 bit0 (UART-only; the RJ11 board has no H/L pin)
    uint8_t radio_type;  // 0 = other radios (P1.4 low), 1 = YAESU/MOTO (P1.4 high)
    uint8_t squelch;     // 0..8
    uint8_t mic_level;   // 0..8, module default 5
    uint8_t tot;         // 0..9 minutes, 0 = off
    uint8_t scramble;    // 0..7, 0 = off
    bool compander;
    uint8_t volume;      // 1..9, module default 6
    bool power_save;     // AT+DMOSAV receiver power saving
    uint8_t vox;         // 0..8, 0 = off
};

void RADIO_CONFIG_Init(void);
const RadioModuleConfig *RADIO_CONFIG_Get(void);
// Validates every field; on success replaces the runtime copy, persists when
// persist=true and bumps the config generation.
bool RADIO_CONFIG_Set(const RadioModuleConfig *config, bool persist);
// Pushes the current config to the module. Powers the module down when the
// config is disabled. On boards without the SR-110U this is a no-op success.
bool RADIO_CONFIG_ApplyToModule(void);
// True while some parameter groups still wait for a successful module
// apply (changed but not yet acknowledged by the module).
bool RADIO_CONFIG_PendingApply(void);

// Text helpers shared by the AT interface and the web portal. Frequency text
// is MHz with up to 5 decimals ("450.02500"); tone text is "OFF", a CTCSS
// frequency ("67.0") or a CDCSS code ("D023N" / "D251I").
bool RADIO_CONFIG_ParseFreqMHz(const char *text, uint32_t *out_hz);
void RADIO_CONFIG_FormatFreqMHz(uint32_t hz, char *out, size_t out_size);
bool RADIO_CONFIG_ParseTone(const char *text, RadioTone *out_tone);
void RADIO_CONFIG_FormatTone(const RadioTone *tone, char *out, size_t out_size);

#endif // SERVICES_RADIO_CONFIG_H
