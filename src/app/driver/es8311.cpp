#include "driver/es8311.h"
#include "lib/nrl_psram.h"

#include "driver/audio_passthrough.h"
#include "driver/board_pins.h"
#include "driver/es7210.h"
#include "driver/i2c1.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "ES8311";

namespace {

constexpr uint8_t kEs8311Addr = 0x18; // 7-bit I2C address

constexpr int kPinPaEn = NRL_PIN_PA_EN;
#ifndef NRL_PIN_PA_EN_ACTIVE_LEVEL
#define NRL_PIN_PA_EN_ACTIVE_LEVEL 1
#endif
constexpr int kPinPaEnActiveLevel = NRL_PIN_PA_EN_ACTIVE_LEVEL;

// ES8311 registers (subset)
enum : uint8_t {
    ES8311_REG00_RESET = 0x00,
    ES8311_REG01_CLK_MANAGER = 0x01,
    ES8311_REG02_CLK_MANAGER = 0x02,
    ES8311_REG03_CLK_MANAGER = 0x03,
    ES8311_REG04_CLK_MANAGER = 0x04,
    ES8311_REG05_CLK_MANAGER = 0x05,
    ES8311_REG06_CLK_MANAGER = 0x06,
    ES8311_REG07_CLK_MANAGER = 0x07,
    ES8311_REG08_CLK_MANAGER = 0x08,
    ES8311_REG09_SDPIN = 0x09,
    ES8311_REG0A_SDPOUT = 0x0A,
    ES8311_REG0B_SYSTEM = 0x0B,
    ES8311_REG0C_SYSTEM = 0x0C,
    ES8311_REG0D_SYSTEM = 0x0D,
    ES8311_REG0E_SYSTEM = 0x0E,
    ES8311_REG10_SYSTEM = 0x10,
    ES8311_REG11_SYSTEM = 0x11,
    ES8311_REG12_SYSTEM = 0x12,
    ES8311_REG13_SYSTEM = 0x13,
    ES8311_REG14_SYSTEM = 0x14,
    ES8311_REG15_ADC = 0x15,
    ES8311_REG16_ADC = 0x16,
    ES8311_REG17_ADC = 0x17,
    ES8311_REG18_ADC = 0x18,
    ES8311_REG19_ADC = 0x19,
    ES8311_REG1A_ADC = 0x1A,
    ES8311_REG1B_ADC = 0x1B,
    ES8311_REG1C_ADC = 0x1C,
    ES8311_REG1D_ADCEQ = 0x1D,
    ES8311_REG31_DAC = 0x31,
    ES8311_REG32_DAC = 0x32,
    ES8311_REG33_DAC = 0x33,
    ES8311_REG34_DAC = 0x34,
    ES8311_REG35_DAC = 0x35,
    ES8311_REG36_DAC = 0x36,
    ES8311_REG37_DAC = 0x37,
    ES8311_REG38_DACEQ = 0x38,
    ES8311_REG39_DACEQ = 0x39,
    ES8311_REG3A_DACEQ = 0x3A,
    ES8311_REG3B_DACEQ = 0x3B,
    ES8311_REG3C_DACEQ = 0x3C,
    ES8311_REG3D_DACEQ = 0x3D,
    ES8311_REG3E_DACEQ = 0x3E,
    ES8311_REG3F_DACEQ = 0x3F,
    ES8311_REG40_DACEQ = 0x40,
    ES8311_REG41_DACEQ = 0x41,
    ES8311_REG42_DACEQ = 0x42,
    ES8311_REG43_DACEQ = 0x43,
    ES8311_REG44_GPIO = 0x44,
    ES8311_REG45_GP = 0x45,
};

constexpr uint8_t kEs8311ClockEnableAll = 0x3F;
constexpr uint8_t kDefaultAdcRamprate = 0x04;
// ESP-ADF es8311_start() writes REG17 = 0xBF here, but on some ES8311
// chips values above 0xBA make the ADC output digital zero (observed
// 0xBB-0xC0 dead, 0xC1 erratic). Start below that cliff so even the
// pre-config boot window has a live mic.
constexpr uint8_t kEs8311AdcVolumeDefault = 0xBA;
constexpr uint8_t kEs8311AdcVolumeMax = 0xBA;
constexpr uint8_t kEs8311DacVolumeDefault = 180U;
// REG0D bits: PDN_ANA(7) PDN_IBIASGEN(6) PDN_ADCBIASGEN(5) PDN_ADCVERFGEN(4)
//             PDN_DACVREFGEN(3) PDN_VREF(2) VMIDSEL(1:0)
// NOTE: PDN_VREF (bit 2) has REVERSE polarity vs other PDN bits!
//   0 = disable internal reference (BAD), 1 = enable reference
// VMIDSEL: 0=down, 1=startup normal charge, 2=normal operation, 3=startup fast.
constexpr uint8_t kEs8311PowerUpAnalog = 0x06;
constexpr uint8_t kEs8311PowerUpPgaAdc = 0x02;
constexpr uint8_t kEs8311PowerUpDac = 0x00;
constexpr uint8_t kDefaultDrcWinsize = 0x00;
constexpr uint8_t kDefaultDrcLevel = 0x00;
constexpr uint8_t kDefaultDacRamprate = 0x00;
constexpr uint32_t kDacEqCoefficientMask = 0x3FFFFFFFUL;
constexpr uint32_t kAdcEqCoefficientMask = 0x3FFFFFFFUL;
constexpr uint8_t kEs8311DacUnmute = 0x00;
constexpr uint8_t kEs8311DacMuteMask = 0x60;
constexpr uint32_t kVoiceSampleRate = 16000u;

struct Es8311ClockConfig {
    uint8_t pre_div;
    uint8_t pre_mult;
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

static bool s_es8311_ready = false;
// Set for the entire acquire/reconfigure/play/release interval. Receive-mode
// requests arriving from NRL voice during this interval must not restart the
// 16 kHz passthrough underneath the media task.
static volatile bool s_hifi_active = false;
constexpr size_t kHifiMixSamples = 64u * 1024u;
NRL_PSRAM_BSS static int16_t s_hifi_mix_storage[kHifiMixSamples];
static int16_t *s_hifi_mix_buffer = s_hifi_mix_storage;
static size_t s_hifi_mix_capacity = sizeof(s_hifi_mix_storage);
static uint8_t s_mic_volume = kEs8311AdcVolumeDefault;
static uint8_t s_line_out_volume = kEs8311DacVolumeDefault;
static bool s_hp_drive_enabled = false;
static bool s_drc_enabled = false;
static uint8_t s_drc_winsize = kDefaultDrcWinsize;
static uint8_t s_drc_maxlevel = kDefaultDrcLevel;
static uint8_t s_drc_minlevel = kDefaultDrcLevel;
static uint8_t s_dac_ramprate = kDefaultDacRamprate;
static bool s_dac_eq_bypass = true;
static uint32_t s_daceq_b0 = 0U;
static uint32_t s_daceq_b1 = 0U;
static uint32_t s_daceq_a1 = 0U;
static bool s_adc_dmic_enabled = false;
static bool s_adc_linsel = true;
static uint8_t s_adc_pga_gain = 10U;
static uint8_t s_adc_ramprate = kDefaultAdcRamprate;
static bool s_adc_dmic_sense = false;
static bool s_adc_sync = true;
static bool s_adc_inv = false;
static bool s_adc_ramclr = false;
static uint8_t s_adc_scale = 7U;
static bool s_alc_enabled = false;
static bool s_adc_automute_enabled = false;
static uint8_t s_alc_winsize = 0U;
static uint8_t s_alc_maxlevel = 0U;
static uint8_t s_alc_minlevel = 0U;
static uint8_t s_adc_automute_winsize = 0U;
static uint8_t s_adc_automute_noise_gate = 0U;
static uint8_t s_adc_automute_volume = 0U;
static uint8_t s_adc_hpfs1 = 10U;
static bool s_adc_eq_bypass = true;
static bool s_adc_hpf = true;
static uint8_t s_adc_hpfs2 = 10U;
static uint32_t s_adceq_b0 = 0U;
static uint32_t s_adceq_a1 = 0U;
static uint32_t s_adceq_a2 = 0U;
static uint32_t s_adceq_b1 = 0U;
static uint32_t s_adceq_b2 = 0U;

static bool es8311_write_reg(uint8_t reg, uint8_t value) {
    const uint8_t data[] = {reg, value};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (I2C_MasterTransmit(kEs8311Addr, data, sizeof(data), 100)) {
            return true;
        }
        ESP_LOGI(TAG, "I2C write failed: reg=0x%02X value=0x%02X attempt=%d",
                 static_cast<unsigned>(reg),
                 static_cast<unsigned>(value),
                 attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static bool es8311_read_reg(const uint8_t reg, uint8_t *value) {
    if (value == nullptr) {
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (I2C_MasterTransmitReceive(kEs8311Addr, &reg, 1u, value, 1u, 100)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return false;
}

static uint8_t es8311_resolution_value(const int bits) {
    switch (bits) {
        case 16: return (3u << 2);
        case 18: return (2u << 2);
        case 20: return (1u << 2);
        case 24: return (0u << 2);
        case 32: return (4u << 2);
        default: return (3u << 2);
    }
}

static uint8_t es8311_pre_mult_code(const uint8_t pre_mult) {
    switch (pre_mult) {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        default: return 0;
    }
}

static bool es8311_config_clock(const Es8311ClockConfig &cfg) {
    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, kEs8311ClockEnableAll)) {
        return false;
    }

    uint8_t reg02 = 0x00;
    reg02 |= static_cast<uint8_t>((cfg.pre_div - 1u) << 5);
    reg02 |= static_cast<uint8_t>(es8311_pre_mult_code(cfg.pre_mult) << 3);
    if (!es8311_write_reg(ES8311_REG02_CLK_MANAGER, reg02)) {
        return false;
    }

    if (!es8311_write_reg(ES8311_REG03_CLK_MANAGER, static_cast<uint8_t>((cfg.fs_mode << 6) | cfg.adc_osr))) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG04_CLK_MANAGER, cfg.dac_osr)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG05_CLK_MANAGER,
                          static_cast<uint8_t>(((cfg.adc_div - 1u) << 4) | (cfg.dac_div - 1u)))) {
        return false;
    }

    uint8_t reg06 = 0x00;
    if (cfg.bclk_div < 19u) {
        reg06 |= static_cast<uint8_t>(cfg.bclk_div - 1u);
    } else {
        reg06 |= cfg.bclk_div;
    }

    if (!es8311_write_reg(ES8311_REG06_CLK_MANAGER, reg06)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG07_CLK_MANAGER, cfg.lrck_h)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG08_CLK_MANAGER, cfg.lrck_l)) {
        return false;
    }
    return true;
}

static bool es8311_config_i2s_format(const int bits) {
    const uint8_t reg_value = es8311_resolution_value(bits);
    if (!es8311_write_reg(ES8311_REG09_SDPIN, reg_value)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0A_SDPOUT, reg_value)) {
        return false;
    }
    return true;
}

static bool es8311_clock_config_for_rate(const uint32_t sample_rate_hz,
                                         Es8311ClockConfig *cfg) {
    if (cfg == nullptr) {
        return false;
    }

    // AUDIO_ReconfigureOutput supplies MCLK=256*Fs. These values are the
    // matching rows from Espressif's ES8311 coefficient table. For the
    // common 8..64 kHz rows the divider topology is identical; only the DAC
    // oversampling ratio changes above 16 kHz.
    switch (sample_rate_hz) {
        case 8000u:
        case 11025u:
        case 12000u:
        case 16000u:
            *cfg = {
                .pre_div = 0x01,
                .pre_mult = 0x01,
                .adc_div = 0x01,
                .dac_div = 0x01,
                .fs_mode = 0x00,
                .lrck_h = 0x00,
                .lrck_l = 0xFF,
                .bclk_div = 0x04,
                .adc_osr = 0x10,
                .dac_osr = 0x20,
            };
            return true;

        case 22050u:
        case 24000u:
        case 32000u:
        case 44100u:
        case 48000u:
        case 64000u:
            *cfg = {
                .pre_div = 0x01,
                .pre_mult = 0x01,
                .adc_div = 0x01,
                .dac_div = 0x01,
                .fs_mode = 0x00,
                .lrck_h = 0x00,
                .lrck_l = 0xFF,
                .bclk_div = 0x04,
                .adc_osr = 0x10,
                .dac_osr = 0x10,
            };
            return true;

        case 96000u:
            *cfg = {
                .pre_div = 0x02,
                .pre_mult = 0x02,
                .adc_div = 0x01,
                .dac_div = 0x01,
                .fs_mode = 0x00,
                .lrck_h = 0x00,
                .lrck_l = 0xFF,
                .bclk_div = 0x04,
                .adc_osr = 0x10,
                .dac_osr = 0x10,
            };
            return true;

        default:
            return false;
    }
}

static bool es8311_apply_sample_format(const uint32_t sample_rate_hz,
                                       const uint8_t bits_per_sample) {
    Es8311ClockConfig cfg = {};
    return bits_per_sample == 16u &&
           es8311_clock_config_for_rate(sample_rate_hz, &cfg) &&
           es8311_config_clock(cfg) &&
           es8311_config_i2s_format(bits_per_sample);
}

static bool es8311_set_dac_mute(const bool muted) {
    uint8_t reg31 = 0u;
    if (!es8311_read_reg(ES8311_REG31_DAC, &reg31)) {
        return false;
    }
    reg31 = muted ? static_cast<uint8_t>(reg31 | kEs8311DacMuteMask)
                  : static_cast<uint8_t>(reg31 & ~kEs8311DacMuteMask);
    return es8311_write_reg(ES8311_REG31_DAC, reg31);
}

static uint8_t es8311_output_drive_reg(void) {
    // HP mode (0x10, HPSW=1): OUTN virtual ground, OUTP single-ended.
    // LINE mode (0x00): pure differential OUTP/OUTN, symmetric DC bias.
    if (s_hp_drive_enabled) {
        return 0x10;
    }
    return 0x00;
}

static uint8_t es8311_drc_reg34(void) {
    return static_cast<uint8_t>((s_drc_enabled ? 0x80u : 0x00u) |
                                (s_drc_winsize & 0x0Fu));
}

static uint8_t es8311_drc_reg35(void) {
    return static_cast<uint8_t>(((s_drc_maxlevel & 0x0Fu) << 4) |
                                (s_drc_minlevel & 0x0Fu));
}

static uint8_t es8311_dac_reg37(void) {
    return static_cast<uint8_t>(((s_dac_ramprate & 0x0Fu) << 4) |
                                (s_dac_eq_bypass ? 0x08u : 0x00u));
}

static bool es8311_apply_drc_config(void) {
    return es8311_write_reg(ES8311_REG34_DAC, es8311_drc_reg34()) &&
           es8311_write_reg(ES8311_REG35_DAC, es8311_drc_reg35()) &&
           es8311_write_reg(ES8311_REG37_DAC, es8311_dac_reg37());
}

static bool es8311_write_daceq_coeff(const uint8_t first_reg, const uint32_t value) {
    const uint32_t coeff = value & kDacEqCoefficientMask;
    return es8311_write_reg(first_reg, static_cast<uint8_t>((coeff >> 24) & 0x3Fu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 1U), static_cast<uint8_t>((coeff >> 16) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 2U), static_cast<uint8_t>((coeff >> 8) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 3U), static_cast<uint8_t>(coeff & 0xFFu));
}

static bool es8311_apply_daceq_coefficients(void) {
    return es8311_write_daceq_coeff(ES8311_REG38_DACEQ, s_daceq_b0) &&
           es8311_write_daceq_coeff(ES8311_REG3C_DACEQ, s_daceq_b1) &&
           es8311_write_daceq_coeff(ES8311_REG40_DACEQ, s_daceq_a1);
}

static uint8_t es8311_adc_reg14(void) {
    return static_cast<uint8_t>((s_adc_dmic_enabled ? 0x40u : 0x00u) |
                                (s_adc_linsel ? 0x10u : 0x00u) |
                                (s_adc_pga_gain & 0x0Fu));
}

static uint8_t es8311_adc_reg15(void) {
    return static_cast<uint8_t>(((s_adc_ramprate & 0x0Fu) << 4) |
                                (s_adc_dmic_sense ? 0x01u : 0x00u));
}

static uint8_t es8311_adc_reg16(void) {
    return static_cast<uint8_t>((s_adc_sync ? 0x20u : 0x00u) |
                                (s_adc_inv ? 0x10u : 0x00u) |
                                (s_adc_ramclr ? 0x08u : 0x00u) |
                                (s_adc_scale & 0x07u));
}

static uint8_t es8311_adc_reg18(void) {
    return static_cast<uint8_t>((s_alc_enabled ? 0x80u : 0x00u) |
                                (s_adc_automute_enabled ? 0x40u : 0x00u) |
                                (s_alc_winsize & 0x0Fu));
}

static uint8_t es8311_adc_reg19(void) {
    return static_cast<uint8_t>(((s_alc_maxlevel & 0x0Fu) << 4) |
                                (s_alc_minlevel & 0x0Fu));
}

static uint8_t es8311_adc_reg1a(void) {
    return static_cast<uint8_t>(((s_adc_automute_winsize & 0x0Fu) << 4) |
                                (s_adc_automute_noise_gate & 0x0Fu));
}

static uint8_t es8311_adc_reg1b(void) {
    return static_cast<uint8_t>(((s_adc_automute_volume & 0x07u) << 5) |
                                (s_adc_hpfs1 & 0x1Fu));
}

static uint8_t es8311_adc_reg1c(void) {
    return static_cast<uint8_t>((s_adc_eq_bypass ? 0x40u : 0x00u) |
                                (s_adc_hpf ? 0x20u : 0x00u) |
                                (s_adc_hpfs2 & 0x1Fu));
}

static bool es8311_write_adceq_coeff(const uint8_t first_reg, const uint32_t value) {
    const uint32_t coeff = value & kAdcEqCoefficientMask;
    return es8311_write_reg(first_reg, static_cast<uint8_t>((coeff >> 24) & 0x3Fu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 1U), static_cast<uint8_t>((coeff >> 16) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 2U), static_cast<uint8_t>((coeff >> 8) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 3U), static_cast<uint8_t>(coeff & 0xFFu));
}

static bool es8311_apply_adc_config(void) {
    return es8311_write_reg(ES8311_REG14_SYSTEM, es8311_adc_reg14()) &&
           es8311_write_reg(ES8311_REG15_ADC, es8311_adc_reg15()) &&
           es8311_write_reg(ES8311_REG16_ADC, es8311_adc_reg16()) &&
           es8311_write_reg(ES8311_REG18_ADC, es8311_adc_reg18()) &&
           es8311_write_reg(ES8311_REG19_ADC, es8311_adc_reg19()) &&
           es8311_write_reg(ES8311_REG1A_ADC, es8311_adc_reg1a()) &&
           es8311_write_reg(ES8311_REG1B_ADC, es8311_adc_reg1b()) &&
           es8311_write_reg(ES8311_REG1C_ADC, es8311_adc_reg1c()) &&
           es8311_write_adceq_coeff(ES8311_REG1D_ADCEQ, s_adceq_b0) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 4U), s_adceq_a1) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 8U), s_adceq_a2) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 12U), s_adceq_b1) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 16U), s_adceq_b2);
}

static bool es8311_config_dac_output_path(const uint8_t reg12,
                                          const uint8_t reg13,
                                          const uint8_t reg31,
                                          const uint8_t reg32) {
    if (!es8311_write_reg(ES8311_REG12_SYSTEM, reg12)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG13_SYSTEM, reg13)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG31_DAC, reg31)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG32_DAC, reg32)) {
        return false;
    }
    return true;
}

// Init sequence aligned to the ESP-ADF esp_codec_dev ES8311 implementation:
// es8311_open(), es8311_set_fs(), then es8311_enable(true).
static bool es8311_configure_codec(void) {
    uint8_t regv = 0u;

    if (es8311_read_reg(ES8311_REG0D_SYSTEM, &regv) && regv != 0xFA) {
        if (!es8311_write_reg(ES8311_REG0D_SYSTEM, 0xFA)) {
            ESP_LOGI(TAG, "REG0D pre-power failed");
            return false;
        }
    }

    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        ESP_LOGI(TAG, "REG44 (1) failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        ESP_LOGI(TAG, "REG44 (2) failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x30)) {
        ESP_LOGI(TAG, "REG01 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG02_CLK_MANAGER, 0x00)) {
        ESP_LOGI(TAG, "REG02 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG03_CLK_MANAGER, 0x10)) {
        ESP_LOGI(TAG, "REG03 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG16_ADC, es8311_adc_reg16())) {
        ESP_LOGI(TAG, "REG16 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG04_CLK_MANAGER, 0x10)) {
        ESP_LOGI(TAG, "REG04 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG05_CLK_MANAGER, 0x00)) {
        ESP_LOGI(TAG, "REG05 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0B_SYSTEM, 0x00)) {
        ESP_LOGI(TAG, "REG0B failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0C_SYSTEM, 0x00)) {
        ESP_LOGI(TAG, "REG0C failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG10_SYSTEM, 0x1F)) {
        ESP_LOGI(TAG, "REG10 bias config failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG11_SYSTEM, 0x7F)) {
        ESP_LOGI(TAG, "REG11 bias config failed");
        return false;
    }
    // Let the bias generators settle before powering up the rest of the
    // analog block. Without this delay the ADC sees transient bias and
    // produces an audible "settling" hiss for the first second.
    vTaskDelay(pdMS_TO_TICKS(40));

    if (!es8311_write_reg(ES8311_REG00_RESET, 0x80)) {
        ESP_LOGI(TAG, "REG00 reset/slave failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x3F)) {
        ESP_LOGI(TAG, "REG01 use_mclk failed");
        return false;
    }

    if (es8311_read_reg(ES8311_REG06_CLK_MANAGER, &regv)) {
        regv = static_cast<uint8_t>(regv & ~0x20u);
        if (!es8311_write_reg(ES8311_REG06_CLK_MANAGER, regv)) {
            ESP_LOGI(TAG, "REG06 sclk polarity failed");
            return false;
        }
    }

    if (!es8311_apply_sample_format(kVoiceSampleRate, 16u)) {
        ESP_LOGI(TAG, "voice sample format configuration failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG13_SYSTEM, es8311_output_drive_reg())) {
        ESP_LOGI(TAG, "REG13 failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG1B_ADC, es8311_adc_reg1b())) {
        ESP_LOGI(TAG, "REG1B (ADC HPF) failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG1C_ADC, es8311_adc_reg1c())) {
        ESP_LOGI(TAG, "REG1C (ADC EQ) failed");
        return false;
    }
    // REG44 ADCDAT_SEL (bits 6:4): 0 = "ADC + ADC" duplicates the mic on
    // both I2S slots (default). Keep bit3 (I2C_WL) set.
    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        ESP_LOGI(TAG, "REG44 final failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG00_RESET, 0x80)) {
        ESP_LOGI(TAG, "REG00 start failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x3F)) {
        ESP_LOGI(TAG, "REG01 start clock failed");
        return false;
    }

    uint8_t dac_iface = 0u;
    uint8_t adc_iface = 0u;
    if (!es8311_read_reg(ES8311_REG09_SDPIN, &dac_iface) ||
        !es8311_read_reg(ES8311_REG0A_SDPOUT, &adc_iface)) {
        ESP_LOGI(TAG, "interface readback failed");
        return false;
    }
    dac_iface = static_cast<uint8_t>(dac_iface & ~0x40u);
    adc_iface = static_cast<uint8_t>(adc_iface & ~0x40u);
    if (!es8311_write_reg(ES8311_REG09_SDPIN, dac_iface) ||
        !es8311_write_reg(ES8311_REG0A_SDPOUT, adc_iface)) {
        ESP_LOGI(TAG, "interface enable failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG17_ADC, s_mic_volume)) {
        ESP_LOGI(TAG, "REG17 ADC volume failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0E_SYSTEM, kEs8311PowerUpPgaAdc)) {
        ESP_LOGI(TAG, "REG0E power-up PGA/ADC failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG12_SYSTEM, kEs8311PowerUpDac)) {
        ESP_LOGI(TAG, "REG12 enable DAC failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG14_SYSTEM, es8311_adc_reg14())) {
        ESP_LOGI(TAG, "REG14 analog mic PGA failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0D_SYSTEM, kEs8311PowerUpAnalog)) {
        ESP_LOGI(TAG, "REG0D power-up analog failed");
        return false;
    }
    // VMID needs ~tens of ms to charge up after enabling the analog block.
    vTaskDelay(pdMS_TO_TICKS(40));
    if (!es8311_apply_adc_config()) {
        ESP_LOGI(TAG, "REG15 ADC ramp rate failed");
        return false;
    }
    if (!es8311_apply_drc_config() || !es8311_apply_daceq_coefficients()) {
        ESP_LOGI(TAG, "DAC DRC/EQ config failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG45_GP, 0x00)) {
        ESP_LOGI(TAG, "REG45 GP control failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG31_DAC, kEs8311DacUnmute)) {
        ESP_LOGI(TAG, "REG31 unmute failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG32_DAC, s_line_out_volume)) {
        ESP_LOGI(TAG, "REG32 DAC volume failed");
        return false;
    }

    return true;
}

} // namespace

extern "C" bool ES8311_Init(void) {
    if (s_es8311_ready) {
        return s_hifi_active || AUDIO_StartPassthrough();
    }

    AUDIO_SetMode(AUDIO_MODE_RECEIVE);

    if (kPinPaEn >= 0) {
        gpio_reset_pin((gpio_num_t)kPinPaEn);
        gpio_set_direction((gpio_num_t)kPinPaEn, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)kPinPaEn, kPinPaEnActiveLevel);
        ESP_LOGI(TAG, "PA enable pin %d set %s", kPinPaEn,
                 kPinPaEnActiveLevel ? "HIGH" : "LOW");
    }

    i2c_master_bus_handle_t i2c_bus = nullptr;
    if (!I2C_MasterGetBus(&i2c_bus)) {
        ESP_LOGE(TAG, "I2C master bus unavailable");
        return false;
    }

    // I2S/MCLK must be running BEFORE codec register config.
    // ES8311 internal DAC bias circuits require MCLK to establish
    // proper VMID and output common-mode voltage.
    if (!AUDIO_SetupI2S()) {
        ESP_LOGI(TAG, "initialization failed during I2S setup");
        return false;
    }

    if (!es8311_configure_codec()) {
        ESP_LOGI(TAG, "initialization failed during codec configuration");
        return false;
    }

    if (!AUDIO_StartPassthrough()) {
        ESP_LOGI(TAG, "initialization failed starting passthrough task");
        return false;
    }

    s_es8311_ready = true;
    ESP_LOGI(TAG, "ready: i2c=0x%02X", static_cast<unsigned>(kEs8311Addr));
    return true;
}

extern "C" bool ES8311_IsReady(void) {
    return s_es8311_ready;
}

extern "C" bool ES8311_ReadReg(const uint8_t reg, uint8_t *value) {
    return es8311_read_reg(reg, value);
}

extern "C" bool ES8311_WriteReg(const uint8_t reg, const uint8_t value) {
    return es8311_write_reg(reg, value);
}

extern "C" bool ES8311_SetAudioMode(const AUDIO_Mode_t mode) {
    if (!ES8311_Init()) {
        return false;
    }

    // NRL voice has already asked the music player to stop. Keep its frames
    // queued until the media task restores 16 kHz and restarts passthrough.
    if (s_hifi_active) {
        AUDIO_SetMode(mode);
        return true;
    }

    if (!es8311_config_dac_output_path(kEs8311PowerUpDac,
                                       es8311_output_drive_reg(),
                                       kEs8311DacUnmute,
                                       s_line_out_volume)) {
        ESP_LOGI(TAG, "failed to configure DAC output path");
        return false;
    }

    AUDIO_SetMode(mode);
    return true;
}

extern "C" bool ES8311_SetReceiveMode(void) {
    return ES8311_SetAudioMode(AUDIO_MODE_RECEIVE);
}

extern "C" bool ES8311_HifiAcquire(const uint32_t sample_rate_hz,
                                    const uint8_t bits_per_sample,
                                    const uint8_t channels) {
    Es8311ClockConfig unused = {};
    if (!s_es8311_ready || s_hifi_active || bits_per_sample != 16u ||
        channels != 2u ||
        !es8311_clock_config_for_rate(sample_rate_hz, &unused)) {
        return false;
    }

    // Claim the path before stopping the task so an NRL voice callback cannot
    // restart passthrough in the middle of the clock transition.
    s_hifi_active = true;
    AUDIO_StopPassthrough();
    AUDIO_ClearOutputQueue();
    (void)es8311_set_dac_mute(true);

    const bool configured =
        AUDIO_ReconfigureOutput(sample_rate_hz, bits_per_sample) &&
        es8311_apply_sample_format(sample_rate_hz, bits_per_sample);
    if (!configured) {
        ESP_LOGE(TAG, "hifi: configure %luHz/%ubit/%uch failed",
                 static_cast<unsigned long>(sample_rate_hz),
                 static_cast<unsigned>(bits_per_sample),
                 static_cast<unsigned>(channels));
        (void)AUDIO_ReconfigureOutput(kVoiceSampleRate, 16u);
        (void)es8311_apply_sample_format(kVoiceSampleRate, 16u);
        (void)es8311_set_dac_mute(false);
        s_hifi_active = false;
        (void)AUDIO_StartPassthrough();
        return false;
    }

    (void)es8311_set_dac_mute(false);
    ESP_LOGI(TAG, "hifi: acquired %luHz/%ubit/%uch (mono DAC downmix)",
             static_cast<unsigned long>(sample_rate_hz),
             static_cast<unsigned>(bits_per_sample),
             static_cast<unsigned>(channels));
    return true;
}

extern "C" bool ES8311_HifiWrite(const void *pcm, const size_t bytes) {
    if (!s_hifi_active || pcm == nullptr || bytes == 0u ||
        (bytes % (2u * sizeof(int16_t))) != 0u) {
        return false;
    }

    const int16_t *stereo = static_cast<const int16_t *>(pcm);
    const size_t frames = bytes / (2u * sizeof(int16_t));
    const size_t chunk_frames = s_hifi_mix_capacity / (2u * sizeof(int16_t));
    for (size_t offset = 0u; offset < frames;) {
        const size_t take = (frames - offset < chunk_frames) ? frames - offset : chunk_frames;
        for (size_t i = 0u; i < take; ++i) {
            const size_t source = (offset + i) * 2u;
            const int32_t mixed =
                (static_cast<int32_t>(stereo[source]) +
                 static_cast<int32_t>(stereo[source + 1u])) / 2;
            s_hifi_mix_buffer[i * 2u] = static_cast<int16_t>(mixed);
            s_hifi_mix_buffer[i * 2u + 1u] = static_cast<int16_t>(mixed);
        }
        const size_t chunk_bytes = take * 2u * sizeof(int16_t);
        if (!AUDIO_WriteOutput(s_hifi_mix_buffer, chunk_bytes)) return false;
        offset += take;
    }
    return true;
}

extern "C" bool ES8311_HifiRelease(void) {
    if (!s_hifi_active) {
        return true;
    }

    (void)es8311_set_dac_mute(true);
    const bool output_restored = AUDIO_ReconfigureOutput(kVoiceSampleRate, 16u);
    const bool codec_restored = es8311_apply_sample_format(kVoiceSampleRate, 16u);
    (void)es8311_set_dac_mute(false);
    s_hifi_active = false;
    const bool restarted = AUDIO_StartPassthrough();

    ESP_LOGI(TAG, "hifi: released (voice path %s)",
             (output_restored && codec_restored && restarted)
                 ? "restored" : "RESTORE FAILED");
    return output_restored && codec_restored && restarted;
}

extern "C" bool ES8311_HifiActive(void) {
    return s_hifi_active;
}

extern "C" bool ES8311_ApplyAudioConfig(const uint8_t mic_volume,
                                        const uint8_t line_out_volume,
                                        const bool hp_drive_enabled,
                                        const bool drc_enabled,
                                        const uint8_t drc_winsize,
                                        const uint8_t drc_maxlevel,
                                        const uint8_t drc_minlevel,
                                        const uint8_t dac_ramprate,
                                        const bool dac_eq_bypass,
                                        const uint32_t daceq_b0,
                                        const uint32_t daceq_b1,
                                        const uint32_t daceq_a1,
                                        const bool adc_dmic_enabled,
                                        const bool adc_linsel,
                                        const uint8_t adc_pga_gain,
                                        const uint8_t adc_ramprate,
                                        const bool adc_dmic_sense,
                                        const bool adc_sync,
                                        const bool adc_inv,
                                        const bool adc_ramclr,
                                        const uint8_t adc_scale,
                                        const bool alc_enabled,
                                        const bool adc_automute_enabled,
                                        const uint8_t alc_winsize,
                                        const uint8_t alc_maxlevel,
                                        const uint8_t alc_minlevel,
                                        const uint8_t adc_automute_winsize,
                                        const uint8_t adc_automute_noise_gate,
                                        const uint8_t adc_automute_volume,
                                        const uint8_t adc_hpfs1,
                                        const bool adc_eq_bypass,
                                        const bool adc_hpf,
                                        const uint8_t adc_hpfs2,
                                        const uint32_t adceq_b0,
                                        const uint32_t adceq_a1,
                                        const uint32_t adceq_a2,
                                        const uint32_t adceq_b1,
                                        const uint32_t adceq_b2) {
    s_mic_volume = mic_volume > kEs8311AdcVolumeMax ? kEs8311AdcVolumeMax : mic_volume;
    s_line_out_volume = line_out_volume;
    s_hp_drive_enabled = hp_drive_enabled;

    // On boards with a separate ES7210 mic ADC, the microphone is captured
    // by the ES7210, not the ES8311 -- route the mic volume there too. The
    // ES8311 REG17 write below is then harmless (its ADC path is unused).
    // On boards without an ES7210 this call is a no-op.
    ES7210_SetMicVolume(mic_volume);
    s_drc_enabled = drc_enabled;
    s_drc_winsize = drc_winsize & 0x0Fu;
    s_drc_maxlevel = drc_maxlevel & 0x0Fu;
    s_drc_minlevel = drc_minlevel & 0x0Fu;
    s_dac_ramprate = dac_ramprate & 0x0Fu;
    s_dac_eq_bypass = dac_eq_bypass;
    s_daceq_b0 = daceq_b0 & kDacEqCoefficientMask;
    s_daceq_b1 = daceq_b1 & kDacEqCoefficientMask;
    s_daceq_a1 = daceq_a1 & kDacEqCoefficientMask;
    s_adc_dmic_enabled = adc_dmic_enabled;
    s_adc_linsel = adc_linsel;
    s_adc_pga_gain = adc_pga_gain & 0x0Fu;
    s_adc_ramprate = adc_ramprate & 0x0Fu;
    s_adc_dmic_sense = adc_dmic_sense;
    s_adc_sync = adc_sync;
    s_adc_inv = adc_inv;
    s_adc_ramclr = adc_ramclr;
    s_adc_scale = adc_scale & 0x07u;
    s_alc_enabled = alc_enabled;
    s_adc_automute_enabled = adc_automute_enabled;
    s_alc_winsize = alc_winsize & 0x0Fu;
    s_alc_maxlevel = alc_maxlevel & 0x0Fu;
    s_alc_minlevel = alc_minlevel & 0x0Fu;
    s_adc_automute_winsize = adc_automute_winsize & 0x0Fu;
    s_adc_automute_noise_gate = adc_automute_noise_gate & 0x0Fu;
    s_adc_automute_volume = adc_automute_volume & 0x07u;
    s_adc_hpfs1 = adc_hpfs1 & 0x1Fu;
    s_adc_eq_bypass = adc_eq_bypass;
    s_adc_hpf = adc_hpf;
    s_adc_hpfs2 = adc_hpfs2 & 0x1Fu;
    s_adceq_b0 = adceq_b0 & kAdcEqCoefficientMask;
    s_adceq_a1 = adceq_a1 & kAdcEqCoefficientMask;
    s_adceq_a2 = adceq_a2 & kAdcEqCoefficientMask;
    s_adceq_b1 = adceq_b1 & kAdcEqCoefficientMask;
    s_adceq_b2 = adceq_b2 & kAdcEqCoefficientMask;

    if (!s_es8311_ready) {
        ESP_LOGI(TAG, "audio config pending: mic=0x%02X line_out=0x%02X hp_drive=%u",
                 static_cast<unsigned>(s_mic_volume),
                 static_cast<unsigned>(s_line_out_volume),
                 s_hp_drive_enabled ? 1u : 0u);
        return true;
    }

    const bool ok =
        es8311_write_reg(ES8311_REG17_ADC, s_mic_volume) &&
        es8311_config_dac_output_path(kEs8311PowerUpDac,
                                      es8311_output_drive_reg(),
                                      kEs8311DacUnmute,
                                      s_line_out_volume) &&
        es8311_apply_drc_config() &&
        es8311_apply_daceq_coefficients() &&
        es8311_apply_adc_config();
    if (!ok) {
        ESP_LOGI(TAG, "audio config apply failed");
    }
    return ok;
}
