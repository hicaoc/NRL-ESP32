#include "external_radio.h"

#include "board_pins.h"
#include "audio_passthrough.h"
#include "es7210.h"
#include "es8311.h"
#include "es8389.h"
#include "../../lib/nrl_audio_config.h"
#include "../../lib/nrl_bt_hfp.h"
#include "../../services/config_notify.h"
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
#include "aec/aec_processor.h"
#endif

#ifdef ENABLE_OPENCV
#include "../opencv/Arduino.hpp"
#endif

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <nvs.h>

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "IO";

namespace {

constexpr const char *kConfigNvsNamespace = "device_config";
constexpr const char *kConfigNvsKey = "config";
constexpr uint32_t kConfigMagic = 0x58455655U;
// BI4UMD needs the tuned speaker/mic defaults; bump the stored config version
// so older 7.x configs get re-seeded once on first boot.
#if NRL_BOARD == NRL_BOARD_BI4UMD
constexpr uint8_t kConfigVersion = 8U;
constexpr uint8_t kDefaultMicVolume = 0xFFU;
constexpr uint8_t kDefaultLineOutVolume = 0xBFU;
constexpr uint8_t kDefaultDrcWinsize = 2U;
constexpr uint8_t kDefaultDrcMaxlevel = 12U;
constexpr uint8_t kDefaultDrcMinlevel = 4U;
constexpr uint8_t kDefaultDacRamprate = 4U;
constexpr uint8_t kDefaultAdcPgaGain = 4U;
constexpr uint8_t kDefaultAdcScale = 0U;
constexpr bool kDefaultAlcEnabled = true;
constexpr uint8_t kDefaultAlcWinsize = 2U;
constexpr uint8_t kDefaultAlcMaxlevel = 12U;
constexpr uint8_t kDefaultAlcMinlevel = 4U;
constexpr bool kDefaultAdcHpf = true;
#else
constexpr uint8_t kConfigVersion = 7U;
constexpr uint8_t kDefaultMicVolume = 180U;
constexpr uint8_t kDefaultLineOutVolume = 180U;
constexpr uint8_t kDefaultDrcWinsize = 0U;
constexpr uint8_t kDefaultDrcMaxlevel = 0U;
constexpr uint8_t kDefaultDrcMinlevel = 0U;
constexpr uint8_t kDefaultDacRamprate = 0U;
constexpr uint8_t kDefaultAdcPgaGain = 10U;
constexpr uint8_t kDefaultAdcScale = 4U;
constexpr bool kDefaultAlcEnabled = false;
constexpr uint8_t kDefaultAlcWinsize = 0U;
constexpr uint8_t kDefaultAlcMaxlevel = 0U;
constexpr uint8_t kDefaultAlcMinlevel = 0U;
constexpr bool kDefaultAdcHpf = false;
#endif
constexpr uint8_t kLegacyConfigVersion1 = 1U;
constexpr uint8_t kLegacyConfigVersion2 = 2U;
constexpr uint8_t kLegacyConfigVersion3 = 3U;
constexpr uint8_t kLegacyConfigVersion4 = 4U;
constexpr uint8_t kLegacyConfigVersion5 = 5U;
constexpr uint8_t kLegacyConfigVersion6 = 6U;
constexpr uint8_t kLegacyConfigVersion7 = 7U;
constexpr uint8_t kMinChannel = 0U;
constexpr uint8_t kMaxChannel = 7U;
constexpr uint16_t kDefaultMicPcmGainMilli = 1000U;
constexpr uint16_t kMinMicPcmGainMilli = 100U;
constexpr uint16_t kMaxMicPcmGainMilli = 5000U;
constexpr uint8_t kPersistedHpDriveOff = 1U;
constexpr uint8_t kPersistedHpDriveOn = 2U;
constexpr uint8_t kPersistedFlagOff = 1U;
constexpr uint8_t kPersistedFlagOn = 2U;
constexpr uint8_t kPersistedAecRefNetwork = 1U;
constexpr uint8_t kPersistedAecRefMic = 2U;
constexpr uint32_t kDacEqCoefficientMask = 0x3FFFFFFFUL;
constexpr uint32_t kAdcEqCoefficientMask = 0x3FFFFFFFUL;
constexpr uint32_t kDefaultSciBaud = 9600U;
constexpr uint8_t kDefaultSciDataBits = 8U;
constexpr char kDefaultSciParity = 'N';
constexpr uint8_t kDefaultSciStopBits = 1U;
constexpr uint16_t kDefaultPttTimeoutS = 300U;  // 5 minutes
constexpr uint16_t kMinPttTimeoutS = 5U;
constexpr uint16_t kMaxPttTimeoutS = 3600U;
constexpr uint16_t kDefaultBatteryCalMilli = 1000U;  // 1.000x: no correction
constexpr uint16_t kMinBatteryCalMilli = 500U;       // 0.5x
constexpr uint16_t kMaxBatteryCalMilli = 2000U;      // 2.0x
constexpr uint16_t kDefaultVoicePayloadBytes = 160U; // 20 ms G.711 A-law
constexpr uint16_t kMinVoicePayloadBytes = 160U;
constexpr uint16_t kMaxVoicePayloadBytes = 500U;
constexpr uint16_t kDefaultTailSuppressMs = 0U;       // disabled
constexpr uint16_t kMaxTailSuppressMs = 5000U;

struct PersistedExternalRadioConfig {
    uint32_t magic;
    uint8_t version;
    uint8_t channel;
    uint16_t server_port;
    uint16_t local_port;
    uint8_t callsign_ssid;
    uint8_t device_mode;
    uint8_t mic_volume;
    uint8_t line_out_volume;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_host[65];
    char callsign[7];
    uint8_t reserved[8];
    uint8_t drc_enabled;
    uint8_t drc_winsize;
    uint8_t drc_maxlevel;
    uint8_t drc_minlevel;
    uint8_t dac_ramprate;
    uint8_t dac_eq_bypass;
    uint8_t reserved2[6];
    uint32_t daceq_b0;
    uint32_t daceq_b1;
    uint32_t daceq_a1;
    uint32_t wifi_ip;
    uint32_t wifi_netmask;
    uint32_t wifi_gateway;
    uint32_t wifi_dns;
    uint8_t wifi_dhcp_enabled;
    uint8_t reserved3[7];
    uint8_t adc_reg14;
    uint8_t adc_reg15;
    uint8_t adc_reg16;
    uint8_t adc_reg18;
    uint8_t adc_reg19;
    uint8_t adc_reg1a;
    uint8_t adc_reg1b;
    uint8_t adc_reg1c;
    uint32_t adceq_b0;
    uint32_t adceq_a1;
    uint32_t adceq_a2;
    uint32_t adceq_b1;
    uint32_t adceq_b2;
    uint8_t reserved4[4];
    ExternalWifiProfile wifi_extra[EXTERNAL_RADIO_MAX_WIFI_PROFILES - 1U];
    uint32_t crc32;
} __attribute__((packed));

static_assert(sizeof(PersistedExternalRadioConfig) <= 1024U,
              "device configuration must remain a small NVS blob");

ExternalRadioConfig s_config = {};
bool s_loaded = false;
bool s_audio_config_migrated = false;
#if NRL_BOARD == NRL_BOARD_BH4TDV
uint8_t s_last_channel_encoded = 0xFFu;
#endif

static uint8_t defaultAecReferenceSource(void)
{
#if NRL_HAS_ES7210
    return EXTERNAL_RADIO_AEC_REF_MIC;
#else
    return EXTERNAL_RADIO_AEC_REF_NETWORK;
#endif
}

static uint8_t normalizeAecReferenceSource(const uint8_t source)
{
#if NRL_HAS_ES7210
    if (source == EXTERNAL_RADIO_AEC_REF_MIC) {
        return EXTERNAL_RADIO_AEC_REF_MIC;
    }
#endif
    return EXTERNAL_RADIO_AEC_REF_NETWORK;
}

static void copyBounded(char *dst, const size_t dst_size, const char *src)
{
    if (dst == nullptr || dst_size == 0U) {
        return;
    }

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    // snprintf, not strncpy: always NUL-terminates and keeps gcc's -O2
    // stringop-truncation analysis quiet about the intentional truncation.
    snprintf(dst, dst_size, "%s", src);
}

static void trimTrailingWhitespace(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t len = strlen(text);
    while (len > 0U) {
        const char ch = text[len - 1U];
        if (ch != '\r' && ch != '\n' && ch != ' ' && ch != '\t') {
            break;
        }
        text[--len] = '\0';
    }
}

static bool isPrintableAscii(const char ch)
{
    const unsigned char value = static_cast<unsigned char>(ch);
    return value >= 32U && value <= 126U;
}

static void sanitizeString(char *text)
{
    if (text == nullptr) {
        return;
    }

    trimTrailingWhitespace(text);
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (!isPrintableAscii(text[i])) {
            text[i] = '_';
        }
    }
}

static void sanitizeCallsign(char *callsign)
{
    if (callsign == nullptr) {
        return;
    }

    sanitizeString(callsign);

    char compact[7] = {};
    size_t out = 0U;
    for (size_t i = 0; callsign[i] != '\0' && out < 6U; ++i) {
        const unsigned char ch = static_cast<unsigned char>(callsign[i]);
        if (isspace(ch)) {
            continue;
        }
        compact[out++] = static_cast<char>(toupper(ch));
    }
    compact[out] = '\0';
    copyBounded(callsign, 7U, compact);

    if (callsign[0] == '\0') {
        copyBounded(callsign, 7U, NRL_AUDIO_CALLSIGN);
    }
}

static uint8_t clampChannel(const uint8_t channel)
{
    if (channel < kMinChannel) {
        return kMinChannel;
    }
    if (channel > kMaxChannel) {
        return kMaxChannel;
    }
    return channel;
}

static char normalizeParity(const char parity)
{
    const char upper = static_cast<char>(toupper(static_cast<unsigned char>(parity)));
    if (upper == 'N' || upper == 'E' || upper == 'O') {
        return upper;
    }
    return '\0';
}

static bool isValidSciConfig(const uint32_t baud,
                             const uint8_t data_bits,
                             const char parity,
                             const uint8_t stop_bits)
{
    return baud >= 300U &&
           baud <= 921600U &&
           data_bits >= 5U &&
           data_bits <= 8U &&
           normalizeParity(parity) != '\0' &&
           (stop_bits == 1U || stop_bits == 2U);
}

static void applyDefaultSciConfig(void)
{
    s_config.sci.baud = kDefaultSciBaud;
    s_config.sci.data_bits = kDefaultSciDataBits;
    s_config.sci.parity = kDefaultSciParity;
    s_config.sci.stop_bits = kDefaultSciStopBits;
}

static void applyDefaultDrcConfig(void)
{
    s_config.drc_enabled = false;
    s_config.drc_winsize = kDefaultDrcWinsize;
    s_config.drc_maxlevel = kDefaultDrcMaxlevel;
    s_config.drc_minlevel = kDefaultDrcMinlevel;
    s_config.dac_ramprate = kDefaultDacRamprate;
    s_config.dac_eq_bypass = true;
    s_config.daceq_b0 = 0U;
    s_config.daceq_b1 = 0U;
    s_config.daceq_a1 = 0U;
}

static uint8_t adcReg14(void)
{
    return static_cast<uint8_t>((s_config.adc_dmic_enabled ? 0x40U : 0x00U) |
                                (s_config.adc_linsel ? 0x10U : 0x00U) |
                                (s_config.adc_pga_gain & 0x0FU));
}

static uint8_t adcReg15(void)
{
    return static_cast<uint8_t>(((s_config.adc_ramprate & 0x0FU) << 4) |
                                (s_config.adc_dmic_sense ? 0x01U : 0x00U));
}

static uint8_t adcReg16(void)
{
    return static_cast<uint8_t>((s_config.adc_sync ? 0x20U : 0x00U) |
                                (s_config.adc_inv ? 0x10U : 0x00U) |
                                (s_config.adc_ramclr ? 0x08U : 0x00U) |
                                (s_config.adc_scale & 0x07U));
}

static uint8_t adcReg18(void)
{
    return static_cast<uint8_t>((s_config.alc_enabled ? 0x80U : 0x00U) |
                                (s_config.adc_automute_enabled ? 0x40U : 0x00U) |
                                (s_config.alc_winsize & 0x0FU));
}

static uint8_t adcReg19(void)
{
    return static_cast<uint8_t>(((s_config.alc_maxlevel & 0x0FU) << 4) |
                                (s_config.alc_minlevel & 0x0FU));
}

static uint8_t adcReg1a(void)
{
    return static_cast<uint8_t>(((s_config.adc_automute_winsize & 0x0FU) << 4) |
                                (s_config.adc_automute_noise_gate & 0x0FU));
}

static uint8_t adcReg1b(void)
{
    return static_cast<uint8_t>(((s_config.adc_automute_volume & 0x07U) << 5) |
                                (s_config.adc_hpfs1 & 0x1FU));
}

static uint8_t adcReg1c(void)
{
    return static_cast<uint8_t>((s_config.adc_eq_bypass ? 0x40U : 0x00U) |
                                (s_config.adc_hpf ? 0x20U : 0x00U) |
                                (s_config.adc_hpfs2 & 0x1FU));
}

static void applyDefaultAdcConfig(void)
{
    s_config.adc_dmic_enabled = false;
    s_config.adc_linsel = true;
    s_config.adc_pga_gain = kDefaultAdcPgaGain;
    s_config.adc_ramprate = 4U;
    s_config.adc_dmic_sense = false;
    s_config.adc_sync = true;
    s_config.adc_inv = false;
    s_config.adc_ramclr = false;
    s_config.adc_scale = kDefaultAdcScale;
    s_config.alc_enabled = kDefaultAlcEnabled;
    s_config.adc_automute_enabled = false;
    s_config.alc_winsize = kDefaultAlcWinsize;
    s_config.alc_maxlevel = kDefaultAlcMaxlevel;
    s_config.alc_minlevel = kDefaultAlcMinlevel;
    s_config.adc_automute_winsize = 0U;
    s_config.adc_automute_noise_gate = 0U;
    s_config.adc_automute_volume = 0U;
    s_config.adc_hpfs1 = 10U;
    s_config.adc_eq_bypass = true;
    // Enable the codec-side dynamic HPF (DC blocker) on BI4UMD. Older
    // defaults left it off, which let mic DC offset eat into ALC headroom.
    s_config.adc_hpf = kDefaultAdcHpf;
    s_config.adc_hpfs2 = 10U;
    s_config.adceq_b0 = 0U;
    s_config.adceq_a1 = 0U;
    s_config.adceq_a2 = 0U;
    s_config.adceq_b1 = 0U;
    s_config.adceq_b2 = 0U;
}

static void applyDefaultAudioConfig(void)
{
    s_config.mic_volume = kDefaultMicVolume;
    s_config.mic_pcm_gain_milli = kDefaultMicPcmGainMilli;
    s_config.line_out_volume = kDefaultLineOutVolume;
    s_config.hp_drive_enabled = false;
    applyDefaultDrcConfig();
    applyDefaultAdcConfig();
    s_config.aec_enabled = false;
    s_config.aec_reference_source = defaultAecReferenceSource();
    s_config.ai_noise_enabled = false;
    s_config.mic_hpf_enabled = true;
    s_config.bt_enabled = false;
    s_config.wifi_enabled = true;
}

static void loadAdcRegisters(const uint8_t reg14,
                             const uint8_t reg15,
                             const uint8_t reg16,
                             const uint8_t reg18,
                             const uint8_t reg19,
                             const uint8_t reg1a,
                             const uint8_t reg1b,
                             const uint8_t reg1c)
{
    s_config.adc_dmic_enabled = (reg14 & 0x40U) != 0U;
    s_config.adc_linsel = (reg14 & 0x10U) != 0U;
    s_config.adc_pga_gain = reg14 & 0x0FU;
    s_config.adc_ramprate = (reg15 >> 4) & 0x0FU;
    s_config.adc_dmic_sense = (reg15 & 0x01U) != 0U;
    s_config.adc_sync = (reg16 & 0x20U) != 0U;
    s_config.adc_inv = (reg16 & 0x10U) != 0U;
    s_config.adc_ramclr = (reg16 & 0x08U) != 0U;
    s_config.adc_scale = reg16 & 0x07U;
    s_config.alc_enabled = (reg18 & 0x80U) != 0U;
    s_config.adc_automute_enabled = (reg18 & 0x40U) != 0U;
    s_config.alc_winsize = reg18 & 0x0FU;
    s_config.alc_maxlevel = (reg19 >> 4) & 0x0FU;
    s_config.alc_minlevel = reg19 & 0x0FU;
    s_config.adc_automute_winsize = (reg1a >> 4) & 0x0FU;
    s_config.adc_automute_noise_gate = reg1a & 0x0FU;
    s_config.adc_automute_volume = (reg1b >> 5) & 0x07U;
    s_config.adc_hpfs1 = reg1b & 0x1FU;
    s_config.adc_eq_bypass = (reg1c & 0x40U) != 0U;
    s_config.adc_hpf = (reg1c & 0x20U) != 0U;
    s_config.adc_hpfs2 = reg1c & 0x1FU;
}

static void applyAudioConfigToCodec(void)
{
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    ES8311_ApplyAudioConfig(s_config.mic_volume,
                            s_config.line_out_volume,
                            s_config.hp_drive_enabled,
                            s_config.drc_enabled,
                            s_config.drc_winsize,
                            s_config.drc_maxlevel,
                            s_config.drc_minlevel,
                            s_config.dac_ramprate,
                            s_config.dac_eq_bypass,
                            s_config.daceq_b0,
                            s_config.daceq_b1,
                            s_config.daceq_a1,
                            s_config.adc_dmic_enabled,
                            s_config.adc_linsel,
                            s_config.adc_pga_gain,
                            s_config.adc_ramprate,
                            s_config.adc_dmic_sense,
                            s_config.adc_sync,
                            s_config.adc_inv,
                            s_config.adc_ramclr,
                            s_config.adc_scale,
                            s_config.alc_enabled,
                            s_config.adc_automute_enabled,
                            s_config.alc_winsize,
                            s_config.alc_maxlevel,
                            s_config.alc_minlevel,
                            s_config.adc_automute_winsize,
                            s_config.adc_automute_noise_gate,
                            s_config.adc_automute_volume,
                            s_config.adc_hpfs1,
                            s_config.adc_eq_bypass,
                            s_config.adc_hpf,
                            s_config.adc_hpfs2,
                            s_config.adceq_b0,
                            s_config.adceq_a1,
                            s_config.adceq_a2,
                            s_config.adceq_b1,
                            s_config.adceq_b2);
#endif
#if defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    ES8389_SetOutputVolume(s_config.line_out_volume);
    ES8389_SetInputGain(s_config.mic_volume);
#endif
    ES7210_SetMicVolume(s_config.mic_volume);
    // The mic HPF is a pure-software filter (no I2C registers involved), so
    // it is pushed to the codec driver separately from ES8311_ApplyAudioConfig.
    AUDIO_SetMicHpfEnabled(s_config.mic_hpf_enabled);
    AUDIO_SetMicPcmGain(s_config.mic_pcm_gain_milli);
}

// Drives the BH4TDV radio's channel-select GPIOs. 格子派 has no channel-
// select hardware, so this is compiled out there (40/39/38 are its I2S/I2C).
static void applyChannelOutputs(void)
{
#if NRL_BOARD == NRL_BOARD_BH4TDV
    const uint8_t encoded = clampChannel(s_config.channel);
    gpio_set_level((gpio_num_t)NRL_PIN_CHANNEL_BIT0, (encoded & 0x01U) ? 1 : 0);
    gpio_set_level((gpio_num_t)NRL_PIN_CHANNEL_BIT1, (encoded & 0x02U) ? 1 : 0);
    gpio_set_level((gpio_num_t)NRL_PIN_CHANNEL_BIT2, (encoded & 0x04U) ? 1 : 0);

    if (encoded != s_last_channel_encoded) {
        ESP_LOGI(TAG, "channel=%u bits ch0=%u ch1=%u ch2=%u",
                 static_cast<unsigned>(s_config.channel),
                 static_cast<unsigned>((encoded & 0x01U) ? 1u : 0u),
                 static_cast<unsigned>((encoded & 0x02U) ? 1u : 0u),
                 static_cast<unsigned>((encoded & 0x04U) ? 1u : 0u));
        s_last_channel_encoded = encoded;
    }
#endif
}

static void applyDefaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    s_config.channel = 0U;
    s_config.server_port = static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    s_config.local_port = s_config.server_port;
    s_config.callsign_ssid = static_cast<uint8_t>(NRL_AUDIO_CALLSIGN_SSID);
    s_config.device_mode = static_cast<uint8_t>(NRL_AUDIO_DEVICE_MODE);
    applyDefaultAudioConfig();
    applyDefaultSciConfig();
    s_config.wifi_ssid[0] = '\0';
    s_config.wifi_password[0] = '\0';
    s_config.wifi_ip = 0U;
    s_config.wifi_netmask = 0U;
    s_config.wifi_gateway = 0U;
    s_config.wifi_dns = 0U;
    s_config.wifi_dhcp_enabled = true;
    s_config.ptt_timeout_s = kDefaultPttTimeoutS;
    s_config.battery_cal_milli = kDefaultBatteryCalMilli;
    s_config.voice_payload_bytes = kDefaultVoicePayloadBytes;
    s_config.tail_suppress_ms = kDefaultTailSuppressMs;
    copyBounded(s_config.server_host, sizeof(s_config.server_host), NRL_AUDIO_SERVER_HOST);
    copyBounded(s_config.callsign, sizeof(s_config.callsign), NRL_AUDIO_CALLSIGN);
    sanitizeCallsign(s_config.callsign);
}

static void applyNetworkDefaults(void)
{
    memset(s_config.wifi_profiles, 0, sizeof(s_config.wifi_profiles));
    s_config.wifi_ssid[0] = '\0';
    s_config.wifi_password[0] = '\0';
    s_config.wifi_ip = 0U;
    s_config.wifi_netmask = 0U;
    s_config.wifi_gateway = 0U;
    s_config.wifi_dns = 0U;
    s_config.wifi_dhcp_enabled = true;
    copyBounded(s_config.server_host, sizeof(s_config.server_host), NRL_AUDIO_SERVER_HOST);
    s_config.server_port = static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    s_config.local_port = s_config.server_port;
}

static void normalizeConfig(void)
{
    s_config.channel = clampChannel(s_config.channel);
    sanitizeString(s_config.wifi_ssid);
    sanitizeString(s_config.wifi_password);
    for (size_t i = 0; i < EXTERNAL_RADIO_MAX_WIFI_PROFILES; ++i) {
        sanitizeString(s_config.wifi_profiles[i].ssid);
        sanitizeString(s_config.wifi_profiles[i].password);
    }
    // Keep entry 0 as the legacy primary credentials, then compact and
    // de-duplicate the remaining profiles while preserving priority order.
    copyBounded(s_config.wifi_profiles[0].ssid,
                sizeof(s_config.wifi_profiles[0].ssid), s_config.wifi_ssid);
    copyBounded(s_config.wifi_profiles[0].password,
                sizeof(s_config.wifi_profiles[0].password), s_config.wifi_password);
    ExternalWifiProfile compact[EXTERNAL_RADIO_MAX_WIFI_PROFILES] = {};
    size_t used = 0U;
    for (size_t i = 0; i < EXTERNAL_RADIO_MAX_WIFI_PROFILES; ++i) {
        const ExternalWifiProfile &candidate = s_config.wifi_profiles[i];
        if (candidate.ssid[0] == '\0') continue;
        bool duplicate = false;
        for (size_t j = 0; j < used; ++j) {
            if (strcmp(compact[j].ssid, candidate.ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) compact[used++] = candidate;
    }
    memcpy(s_config.wifi_profiles, compact, sizeof(compact));
    if (used > 0U) {
        copyBounded(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), compact[0].ssid);
        copyBounded(s_config.wifi_password, sizeof(s_config.wifi_password), compact[0].password);
    } else {
        s_config.wifi_ssid[0] = '\0';
        s_config.wifi_password[0] = '\0';
    }
    sanitizeString(s_config.server_host);
    sanitizeCallsign(s_config.callsign);
    s_config.device_mode = static_cast<uint8_t>(NRL_AUDIO_DEVICE_MODE);

    if (s_config.server_port == 0U) {
        s_config.server_port = static_cast<uint16_t>(NRL_AUDIO_SERVER_PORT);
    }
    if (s_config.local_port == 0U) {
        s_config.local_port = s_config.server_port;
    }
    s_config.sci.parity = normalizeParity(s_config.sci.parity);
    if (!isValidSciConfig(s_config.sci.baud,
                          s_config.sci.data_bits,
                          s_config.sci.parity,
                          s_config.sci.stop_bits)) {
        applyDefaultSciConfig();
    }
    s_config.drc_winsize &= 0x0FU;
    s_config.drc_maxlevel &= 0x0FU;
    s_config.drc_minlevel &= 0x0FU;
    s_config.dac_ramprate &= 0x0FU;
    s_config.daceq_b0 &= kDacEqCoefficientMask;
    s_config.daceq_b1 &= kDacEqCoefficientMask;
    s_config.daceq_a1 &= kDacEqCoefficientMask;
    if (s_config.adc_pga_gain > 10U) {
        s_config.adc_pga_gain = 10U;
    }
    s_config.adc_ramprate &= 0x0FU;
    s_config.adc_scale &= 0x07U;
    s_config.alc_winsize &= 0x0FU;
    s_config.alc_maxlevel &= 0x0FU;
    s_config.alc_minlevel &= 0x0FU;
    s_config.adc_automute_winsize &= 0x0FU;
    s_config.adc_automute_noise_gate &= 0x0FU;
    s_config.adc_automute_volume &= 0x07U;
    s_config.adc_hpfs1 &= 0x1FU;
    s_config.adc_hpfs2 &= 0x1FU;
    s_config.adceq_b0 &= kAdcEqCoefficientMask;
    s_config.adceq_a1 &= kAdcEqCoefficientMask;
    s_config.adceq_a2 &= kAdcEqCoefficientMask;
    s_config.adceq_b1 &= kAdcEqCoefficientMask;
    s_config.adceq_b2 &= kAdcEqCoefficientMask;
    if (s_config.ptt_timeout_s < kMinPttTimeoutS || s_config.ptt_timeout_s > kMaxPttTimeoutS) {
        s_config.ptt_timeout_s = kDefaultPttTimeoutS;
    }
    if (s_config.battery_cal_milli < kMinBatteryCalMilli ||
        s_config.battery_cal_milli > kMaxBatteryCalMilli) {
        s_config.battery_cal_milli = kDefaultBatteryCalMilli;
    }
    if (s_config.mic_pcm_gain_milli < kMinMicPcmGainMilli ||
        s_config.mic_pcm_gain_milli > kMaxMicPcmGainMilli) {
        s_config.mic_pcm_gain_milli = kDefaultMicPcmGainMilli;
    }
    if (s_config.voice_payload_bytes < kMinVoicePayloadBytes ||
        s_config.voice_payload_bytes > kMaxVoicePayloadBytes) {
        s_config.voice_payload_bytes = kDefaultVoicePayloadBytes;
    }
    // 0 is a valid value (suppression off), so only the upper bound is clamped.
    if (s_config.tail_suppress_ms > kMaxTailSuppressMs) {
        s_config.tail_suppress_ms = kMaxTailSuppressMs;
    }
#if !defined(NRL_ENABLE_AEC) || !NRL_ENABLE_AEC
    s_config.aec_enabled = false;
#endif
    s_config.aec_reference_source = normalizeAecReferenceSource(s_config.aec_reference_source);
#if !defined(NRL_ENABLE_AUDIO_AFE) || !NRL_ENABLE_AUDIO_AFE
    s_config.ai_noise_enabled = false;
#endif
}

static bool loadPersistedConfig(void)
{
    PersistedExternalRadioConfig persisted = {};
    nvs_handle_t nvs = 0;
    if (nvs_open(kConfigNvsNamespace, NVS_READONLY, &nvs) != ESP_OK)
        return false;
    size_t size = sizeof(persisted);
    const esp_err_t read_error = nvs_get_blob(
        nvs, kConfigNvsKey, &persisted, &size);
    nvs_close(nvs);
    const uint32_t expected_crc = esp_rom_crc32_le(
        0u, reinterpret_cast<const uint8_t *>(&persisted),
        offsetof(PersistedExternalRadioConfig, crc32));
    if (read_error != ESP_OK || size != sizeof(persisted) ||
        persisted.magic != kConfigMagic || persisted.crc32 != expected_crc) {
        return false;
    }
    const bool version_ok =
        persisted.version == kConfigVersion ||
        persisted.version == kLegacyConfigVersion1 ||
        persisted.version == kLegacyConfigVersion2 ||
        persisted.version == kLegacyConfigVersion3 ||
        persisted.version == kLegacyConfigVersion4 ||
        persisted.version == kLegacyConfigVersion5 ||
        persisted.version == kLegacyConfigVersion6 ||
        persisted.version == kLegacyConfigVersion7;
    if (!version_ok) {
        return false;
    }
    const bool current_version = persisted.version == kConfigVersion;

    s_config.channel = persisted.channel;
    s_config.server_port = persisted.server_port;
    s_config.local_port = persisted.local_port;
    s_config.callsign_ssid = persisted.callsign_ssid;
    s_config.device_mode = persisted.device_mode;
    s_config.mic_volume = persisted.mic_volume;
    s_config.line_out_volume = persisted.line_out_volume;
    // BI4UMD v8 reseeds the speaker/mic defaults from the desktop speaker-fix
    // path. Older layouts kept a quieter DAC volume and the old 30 dB PGA +
    // 24 dB scale chain that clipped loud talkers. Override those two volumes
    // here so users get the fix without wiping Wi-Fi / callsign settings.
    if (!current_version) {
        s_config.mic_volume = kDefaultMicVolume;
        s_config.line_out_volume = kDefaultLineOutVolume;
        s_audio_config_migrated = true;
    }
    s_config.hp_drive_enabled = persisted.reserved[0] == kPersistedHpDriveOn;
    s_config.sci.data_bits = persisted.reserved[1];
    s_config.sci.parity = static_cast<char>(persisted.reserved[2]);
    s_config.sci.stop_bits = persisted.reserved[3];
    s_config.sci.baud = static_cast<uint32_t>(persisted.reserved[4]) |
                        (static_cast<uint32_t>(persisted.reserved[5]) << 8) |
                        (static_cast<uint32_t>(persisted.reserved[6]) << 16) |
                        (static_cast<uint32_t>(persisted.reserved[7]) << 24);
    copyBounded(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), persisted.wifi_ssid);
    copyBounded(s_config.wifi_password, sizeof(s_config.wifi_password), persisted.wifi_password);
    copyBounded(s_config.wifi_profiles[0].ssid, sizeof(s_config.wifi_profiles[0].ssid),
                persisted.wifi_ssid);
    copyBounded(s_config.wifi_profiles[0].password, sizeof(s_config.wifi_profiles[0].password),
                persisted.wifi_password);
    if (persisted.version >= kLegacyConfigVersion6) {
        for (size_t i = 1; i < EXTERNAL_RADIO_MAX_WIFI_PROFILES; ++i) {
            copyBounded(s_config.wifi_profiles[i].ssid,
                        sizeof(s_config.wifi_profiles[i].ssid), persisted.wifi_extra[i - 1U].ssid);
            copyBounded(s_config.wifi_profiles[i].password,
                        sizeof(s_config.wifi_profiles[i].password), persisted.wifi_extra[i - 1U].password);
        }
    }
    copyBounded(s_config.server_host, sizeof(s_config.server_host), persisted.server_host);
    copyBounded(s_config.callsign, sizeof(s_config.callsign), persisted.callsign);
    if (current_version) {
        s_config.drc_enabled = persisted.drc_enabled == kPersistedFlagOn;
        s_config.drc_winsize = persisted.drc_winsize;
        s_config.drc_maxlevel = persisted.drc_maxlevel;
        s_config.drc_minlevel = persisted.drc_minlevel;
        s_config.dac_ramprate = persisted.dac_ramprate;
        s_config.dac_eq_bypass = persisted.dac_eq_bypass != kPersistedFlagOff;
    } else {
        applyDefaultDrcConfig();
        s_audio_config_migrated = true;
    }
    if (persisted.version >= kLegacyConfigVersion5) {
        s_config.daceq_b0 = persisted.daceq_b0;
        s_config.daceq_b1 = persisted.daceq_b1;
        s_config.daceq_a1 = persisted.daceq_a1;
    } else {
        s_config.daceq_b0 = 0U;
        s_config.daceq_b1 = 0U;
        s_config.daceq_a1 = 0U;
    }
    if (persisted.version >= kLegacyConfigVersion4) {
        s_config.wifi_ip = persisted.wifi_ip;
        s_config.wifi_netmask = persisted.wifi_netmask;
        s_config.wifi_gateway = persisted.wifi_gateway;
        s_config.wifi_dns = persisted.wifi_dns;
        s_config.wifi_dhcp_enabled = persisted.wifi_dhcp_enabled != kPersistedFlagOff;
        // reserved3[0] holds the AEC flag; 0 (configs written before the flag
        // existed) reads as "default on".
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
        s_config.aec_enabled = persisted.reserved3[0] != kPersistedFlagOff;
#else
        s_config.aec_enabled = false;
#endif
        // reserved3[2] holds the esp-sr speech-enhancement / AI denoise flag.
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
        s_config.ai_noise_enabled = persisted.reserved3[2] != kPersistedFlagOff;
#else
        s_config.ai_noise_enabled = false;
#endif
        // reserved3[1] holds the software mic HPF flag (~200 Hz). Same
        // "0 == not written yet, treat as default" pattern as AEC above.
        s_config.mic_hpf_enabled = persisted.reserved3[1] != kPersistedFlagOff;
        // reserved3[4] holds the Bluetooth-headset enable flag. Defaults OFF, so
        // (unlike the flags above) an unwritten 0 must read as false -- match the
        // explicit "on" sentinel instead of "not off".
        s_config.bt_enabled = persisted.reserved3[4] == kPersistedFlagOn;
        // reserved3[5] holds the master Wi-Fi enable flag. Defaults ON, so an
        // unwritten 0 (old configs) must read as true -- match the "off" sentinel.
        s_config.wifi_enabled = persisted.reserved3[5] != kPersistedFlagOff;
        if (persisted.reserved3[3] == kPersistedAecRefMic) {
            s_config.aec_reference_source = EXTERNAL_RADIO_AEC_REF_MIC;
        } else if (persisted.reserved3[3] == kPersistedAecRefNetwork) {
            s_config.aec_reference_source = EXTERNAL_RADIO_AEC_REF_NETWORK;
        } else {
            s_config.aec_reference_source = defaultAecReferenceSource();
        }
    } else {
        s_config.wifi_ip = 0U;
        s_config.wifi_netmask = 0U;
        s_config.wifi_gateway = 0U;
        s_config.wifi_dns = 0U;
        s_config.wifi_dhcp_enabled = true;
        s_config.aec_enabled = false;
        s_config.aec_reference_source = defaultAecReferenceSource();
        s_config.ai_noise_enabled = false;
        s_config.mic_hpf_enabled = true;
        s_config.bt_enabled = false;
    }
    if (current_version) {
        loadAdcRegisters(persisted.adc_reg14,
                         persisted.adc_reg15,
                         persisted.adc_reg16,
                         persisted.adc_reg18,
                         persisted.adc_reg19,
                         persisted.adc_reg1a,
                         persisted.adc_reg1b,
                         persisted.adc_reg1c);
        s_config.adceq_b0 = persisted.adceq_b0;
        s_config.adceq_a1 = persisted.adceq_a1;
        s_config.adceq_a2 = persisted.adceq_a2;
        s_config.adceq_b1 = persisted.adceq_b1;
        s_config.adceq_b2 = persisted.adceq_b2;
    } else {
        applyDefaultAdcConfig();
        s_audio_config_migrated = true;
    }
    // PTT timeout lives in reserved4[0..1] and battery calibration in
    // reserved4[2..3] (both added without a config version bump). Configs
    // written before these features have the bytes as 0, which normalizeConfig()
    // maps back to the default.
    if (persisted.version >= kLegacyConfigVersion5) {
        s_config.ptt_timeout_s = static_cast<uint16_t>(persisted.reserved4[0]) |
                                 (static_cast<uint16_t>(persisted.reserved4[1]) << 8);
        s_config.battery_cal_milli = static_cast<uint16_t>(persisted.reserved4[2]) |
                                     (static_cast<uint16_t>(persisted.reserved4[3]) << 8);
        // voice_payload_bytes shares the version-5 layout: reserved2[0..1] were
        // unused before this field was added, so old saves read 0 here and
        // normalizeConfig() promotes that to kDefaultVoicePayloadBytes.
        s_config.voice_payload_bytes = static_cast<uint16_t>(persisted.reserved2[0]) |
                                       (static_cast<uint16_t>(persisted.reserved2[1]) << 8);
        // tail_suppress_ms shares the version-5 layout in reserved2[2..3]. Old
        // saves read 0 here, which is the default (suppression disabled).
        s_config.tail_suppress_ms = static_cast<uint16_t>(persisted.reserved2[2]) |
                                    (static_cast<uint16_t>(persisted.reserved2[3]) << 8);
        // Software mic PCM gain uses the final two version-5 reserved bytes.
        // An older saved config reads zero and is normalized to 1.0x.
        s_config.mic_pcm_gain_milli = static_cast<uint16_t>(persisted.reserved2[4]) |
                                      (static_cast<uint16_t>(persisted.reserved2[5]) << 8);
    } else {
        s_config.ptt_timeout_s = 0U;
        s_config.battery_cal_milli = 0U;
        s_config.voice_payload_bytes = 0U;
        s_config.tail_suppress_ms = 0U;
        s_config.mic_pcm_gain_milli = 0U;
    }
    normalizeConfig();
    return true;
}

static bool savePersistedConfig(void)
{
    normalizeConfig();
    PersistedExternalRadioConfig persisted = {};
    persisted.magic = kConfigMagic;
    persisted.version = kConfigVersion;
    persisted.channel = s_config.channel;
    persisted.server_port = s_config.server_port;
    persisted.local_port = s_config.local_port;
    persisted.callsign_ssid = s_config.callsign_ssid;
    persisted.device_mode = s_config.device_mode;
    persisted.mic_volume = s_config.mic_volume;
    persisted.line_out_volume = s_config.line_out_volume;
    persisted.reserved[0] = s_config.hp_drive_enabled ? kPersistedHpDriveOn : kPersistedHpDriveOff;
    persisted.reserved[1] = s_config.sci.data_bits;
    persisted.reserved[2] = static_cast<uint8_t>(s_config.sci.parity);
    persisted.reserved[3] = s_config.sci.stop_bits;
    persisted.reserved[4] = static_cast<uint8_t>(s_config.sci.baud & 0xFFU);
    persisted.reserved[5] = static_cast<uint8_t>((s_config.sci.baud >> 8) & 0xFFU);
    persisted.reserved[6] = static_cast<uint8_t>((s_config.sci.baud >> 16) & 0xFFU);
    persisted.reserved[7] = static_cast<uint8_t>((s_config.sci.baud >> 24) & 0xFFU);
    persisted.drc_enabled = s_config.drc_enabled ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.drc_winsize = s_config.drc_winsize & 0x0FU;
    persisted.drc_maxlevel = s_config.drc_maxlevel & 0x0FU;
    persisted.drc_minlevel = s_config.drc_minlevel & 0x0FU;
    persisted.dac_ramprate = s_config.dac_ramprate & 0x0FU;
    persisted.dac_eq_bypass = s_config.dac_eq_bypass ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.daceq_b0 = s_config.daceq_b0 & kDacEqCoefficientMask;
    persisted.daceq_b1 = s_config.daceq_b1 & kDacEqCoefficientMask;
    persisted.daceq_a1 = s_config.daceq_a1 & kDacEqCoefficientMask;
    persisted.wifi_ip = s_config.wifi_ip;
    persisted.wifi_netmask = s_config.wifi_netmask;
    persisted.wifi_gateway = s_config.wifi_gateway;
    persisted.wifi_dns = s_config.wifi_dns;
    persisted.wifi_dhcp_enabled = s_config.wifi_dhcp_enabled ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.reserved3[0] = s_config.aec_enabled ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.reserved3[1] = s_config.mic_hpf_enabled ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.reserved3[2] = s_config.ai_noise_enabled ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.reserved3[4] = s_config.bt_enabled ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.reserved3[5] = s_config.wifi_enabled ? kPersistedFlagOn : kPersistedFlagOff;
    persisted.reserved3[3] = (s_config.aec_reference_source == EXTERNAL_RADIO_AEC_REF_MIC)
                                 ? kPersistedAecRefMic
                                 : kPersistedAecRefNetwork;
    persisted.adc_reg14 = adcReg14();
    persisted.adc_reg15 = adcReg15();
    persisted.adc_reg16 = adcReg16();
    persisted.adc_reg18 = adcReg18();
    persisted.adc_reg19 = adcReg19();
    persisted.adc_reg1a = adcReg1a();
    persisted.adc_reg1b = adcReg1b();
    persisted.adc_reg1c = adcReg1c();
    persisted.adceq_b0 = s_config.adceq_b0 & kAdcEqCoefficientMask;
    persisted.adceq_a1 = s_config.adceq_a1 & kAdcEqCoefficientMask;
    persisted.adceq_a2 = s_config.adceq_a2 & kAdcEqCoefficientMask;
    persisted.adceq_b1 = s_config.adceq_b1 & kAdcEqCoefficientMask;
    persisted.adceq_b2 = s_config.adceq_b2 & kAdcEqCoefficientMask;
    persisted.reserved4[0] = static_cast<uint8_t>(s_config.ptt_timeout_s & 0xFFU);
    persisted.reserved4[1] = static_cast<uint8_t>((s_config.ptt_timeout_s >> 8) & 0xFFU);
    persisted.reserved4[2] = static_cast<uint8_t>(s_config.battery_cal_milli & 0xFFU);
    persisted.reserved4[3] = static_cast<uint8_t>((s_config.battery_cal_milli >> 8) & 0xFFU);
    persisted.reserved2[0] = static_cast<uint8_t>(s_config.voice_payload_bytes & 0xFFU);
    persisted.reserved2[1] = static_cast<uint8_t>((s_config.voice_payload_bytes >> 8) & 0xFFU);
    persisted.reserved2[2] = static_cast<uint8_t>(s_config.tail_suppress_ms & 0xFFU);
    persisted.reserved2[3] = static_cast<uint8_t>((s_config.tail_suppress_ms >> 8) & 0xFFU);
    persisted.reserved2[4] = static_cast<uint8_t>(s_config.mic_pcm_gain_milli & 0xFFU);
    persisted.reserved2[5] = static_cast<uint8_t>((s_config.mic_pcm_gain_milli >> 8) & 0xFFU);
    copyBounded(persisted.wifi_ssid, sizeof(persisted.wifi_ssid), s_config.wifi_ssid);
    copyBounded(persisted.wifi_password, sizeof(persisted.wifi_password), s_config.wifi_password);
    for (size_t i = 1; i < EXTERNAL_RADIO_MAX_WIFI_PROFILES; ++i) {
        copyBounded(persisted.wifi_extra[i - 1U].ssid,
                    sizeof(persisted.wifi_extra[i - 1U].ssid), s_config.wifi_profiles[i].ssid);
        copyBounded(persisted.wifi_extra[i - 1U].password,
                    sizeof(persisted.wifi_extra[i - 1U].password), s_config.wifi_profiles[i].password);
    }
    copyBounded(persisted.server_host, sizeof(persisted.server_host), s_config.server_host);
    copyBounded(persisted.callsign, sizeof(persisted.callsign), s_config.callsign);
    persisted.crc32 = esp_rom_crc32_le(
        0u, reinterpret_cast<const uint8_t *>(&persisted),
        offsetof(PersistedExternalRadioConfig, crc32));

    nvs_handle_t nvs = 0;
    if (nvs_open(kConfigNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "device config NVS open failed");
        return false;
    }
    const esp_err_t write_error = nvs_set_blob(
        nvs, kConfigNvsKey, &persisted, sizeof(persisted));
    const esp_err_t commit_error = write_error == ESP_OK ? nvs_commit(nvs)
                                                         : write_error;
    nvs_close(nvs);
    if (commit_error != ESP_OK) {
        ESP_LOGE(TAG, "device config NVS write failed: %s",
                 esp_err_to_name(commit_error));
        return false;
    }
    CONFIG_NOTIFY_Bump(); // let the LCD UI refresh pages showing this config
    return true;
}

static bool updateStringField(char *field, const size_t field_size, const char *value, const bool persist)
{
    if (field == nullptr || field_size == 0U || value == nullptr) {
        return false;
    }

    copyBounded(field, field_size, value);
    sanitizeString(field);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

} // namespace

extern "C" void EXTERNAL_RADIO_Init(void)
{
    if (s_loaded) {
        return;
    }

#if NRL_BOARD == NRL_BOARD_BH4TDV
    gpio_reset_pin((gpio_num_t)NRL_PIN_CHANNEL_BIT0);
    gpio_set_direction((gpio_num_t)NRL_PIN_CHANNEL_BIT0, GPIO_MODE_OUTPUT);
    gpio_reset_pin((gpio_num_t)NRL_PIN_CHANNEL_BIT1);
    gpio_set_direction((gpio_num_t)NRL_PIN_CHANNEL_BIT1, GPIO_MODE_OUTPUT);
    gpio_reset_pin((gpio_num_t)NRL_PIN_CHANNEL_BIT2);
    gpio_set_direction((gpio_num_t)NRL_PIN_CHANNEL_BIT2, GPIO_MODE_OUTPUT);
#endif

    applyDefaults();
    if (!loadPersistedConfig()) {
        normalizeConfig();
        savePersistedConfig();
    } else if (s_audio_config_migrated) {
        normalizeConfig();
        savePersistedConfig();
    }

    applyChannelOutputs();
    AUDIO_SetMicPcmGain(s_config.mic_pcm_gain_milli);
    s_loaded = true;
}

extern "C" uint8_t EXTERNAL_RADIO_GetChannel(void)
{
    EXTERNAL_RADIO_Init();
    return s_config.channel;
}

extern "C" bool EXTERNAL_RADIO_SetChannel(const uint8_t channel, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (channel > kMaxChannel) {
        return false;
    }
    s_config.channel = channel;
    applyChannelOutputs();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

extern "C" bool EXTERNAL_RADIO_SaveConfig(void)
{
    EXTERNAL_RADIO_Init();
    normalizeConfig();
    return savePersistedConfig();
}

extern "C" bool EXTERNAL_RADIO_ResetNetworkConfig(void)
{
    EXTERNAL_RADIO_Init();
    applyNetworkDefaults();
    normalizeConfig();
    return savePersistedConfig();
}

const ExternalRadioConfig *EXTERNAL_RADIO_GetConfig(void)
{
    EXTERNAL_RADIO_Init();
    return &s_config;
}

bool EXTERNAL_RADIO_SetWifiSsid(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (!updateStringField(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), value, false) ||
        s_config.wifi_ssid[0] == '\0') {
        return false;
    }
    copyBounded(s_config.wifi_profiles[0].ssid, sizeof(s_config.wifi_profiles[0].ssid),
                s_config.wifi_ssid);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetWifiPassword(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (!updateStringField(s_config.wifi_password, sizeof(s_config.wifi_password), value, false)) {
        return false;
    }
    copyBounded(s_config.wifi_profiles[0].password,
                sizeof(s_config.wifi_profiles[0].password), s_config.wifi_password);
    return !persist || savePersistedConfig();
}

size_t EXTERNAL_RADIO_GetWifiProfileCount(void)
{
    EXTERNAL_RADIO_Init();
    size_t count = 0U;
    while (count < EXTERNAL_RADIO_MAX_WIFI_PROFILES &&
           s_config.wifi_profiles[count].ssid[0] != '\0') {
        ++count;
    }
    return count;
}

bool EXTERNAL_RADIO_AddWifiProfile(const char *ssid, const char *password, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (ssid == nullptr || ssid[0] == '\0' || strlen(ssid) > 32U ||
        password == nullptr || strlen(password) > 64U) {
        return false;
    }
    char clean_ssid[33] = {};
    char clean_password[65] = {};
    copyBounded(clean_ssid, sizeof(clean_ssid), ssid);
    copyBounded(clean_password, sizeof(clean_password), password);
    sanitizeString(clean_ssid);
    sanitizeString(clean_password);
    if (clean_ssid[0] == '\0') return false;

    size_t index = EXTERNAL_RADIO_MAX_WIFI_PROFILES;
    for (size_t i = 0; i < EXTERNAL_RADIO_MAX_WIFI_PROFILES; ++i) {
        if (strcmp(s_config.wifi_profiles[i].ssid, clean_ssid) == 0) {
            index = i;
            break;
        }
        if (index == EXTERNAL_RADIO_MAX_WIFI_PROFILES &&
            s_config.wifi_profiles[i].ssid[0] == '\0') {
            index = i;
        }
    }
    if (index >= EXTERNAL_RADIO_MAX_WIFI_PROFILES) return false;
    copyBounded(s_config.wifi_profiles[index].ssid,
                sizeof(s_config.wifi_profiles[index].ssid), clean_ssid);
    copyBounded(s_config.wifi_profiles[index].password,
                sizeof(s_config.wifi_profiles[index].password), clean_password);
    if (index == 0U) {
        copyBounded(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), clean_ssid);
        copyBounded(s_config.wifi_password, sizeof(s_config.wifi_password), clean_password);
    }
    return !persist || savePersistedConfig();
}

bool EXTERNAL_RADIO_RemoveWifiProfile(const size_t index, const bool persist)
{
    EXTERNAL_RADIO_Init();
    const size_t count = EXTERNAL_RADIO_GetWifiProfileCount();
    if (index >= count) return false;
    for (size_t i = index; i + 1U < EXTERNAL_RADIO_MAX_WIFI_PROFILES; ++i) {
        s_config.wifi_profiles[i] = s_config.wifi_profiles[i + 1U];
    }
    memset(&s_config.wifi_profiles[EXTERNAL_RADIO_MAX_WIFI_PROFILES - 1U], 0,
           sizeof(s_config.wifi_profiles[0]));
    copyBounded(s_config.wifi_ssid, sizeof(s_config.wifi_ssid),
                s_config.wifi_profiles[0].ssid);
    copyBounded(s_config.wifi_password, sizeof(s_config.wifi_password),
                s_config.wifi_profiles[0].password);
    return !persist || savePersistedConfig();
}

bool EXTERNAL_RADIO_MoveWifiProfile(const size_t index, const int direction, const bool persist)
{
    EXTERNAL_RADIO_Init();
    const size_t count = EXTERNAL_RADIO_GetWifiProfileCount();
    if (index >= count || (direction != -1 && direction != 1)) return false;
    if ((direction < 0 && index == 0U) || (direction > 0 && index + 1U >= count)) {
        return false;
    }
    const size_t other = direction < 0 ? index - 1U : index + 1U;
    const ExternalWifiProfile tmp = s_config.wifi_profiles[index];
    s_config.wifi_profiles[index] = s_config.wifi_profiles[other];
    s_config.wifi_profiles[other] = tmp;
    copyBounded(s_config.wifi_ssid, sizeof(s_config.wifi_ssid),
                s_config.wifi_profiles[0].ssid);
    copyBounded(s_config.wifi_password, sizeof(s_config.wifi_password),
                s_config.wifi_profiles[0].password);
    return !persist || savePersistedConfig();
}

bool EXTERNAL_RADIO_SetWifiIp(const uint32_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.wifi_ip = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetWifiNetmask(const uint32_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.wifi_netmask = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetWifiGateway(const uint32_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.wifi_gateway = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetWifiDns(const uint32_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.wifi_dns = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetWifiDhcpEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.wifi_dhcp_enabled = enabled;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetServerHost(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (!updateStringField(s_config.server_host, sizeof(s_config.server_host), value, false) ||
        s_config.server_host[0] == '\0') {
        return false;
    }
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetServerPort(const uint16_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value == 0U) {
        return false;
    }
    s_config.server_port = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetLocalPort(const uint16_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value == 0U) {
        return false;
    }
    s_config.local_port = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetCallsign(const char *value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value == nullptr) {
        return false;
    }

    copyBounded(s_config.callsign, sizeof(s_config.callsign), value);
    sanitizeCallsign(s_config.callsign);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetCallsignSsid(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.callsign_ssid = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetMicVolume(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.mic_volume = value;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetMicPcmGain(const uint16_t gain_milli, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (gain_milli < kMinMicPcmGainMilli || gain_milli > kMaxMicPcmGainMilli) {
        return false;
    }
    s_config.mic_pcm_gain_milli = gain_milli;
    AUDIO_SetMicPcmGain(gain_milli);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetLineOutVolume(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.line_out_volume = value;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_ResetAudioConfig(const bool persist)
{
    EXTERNAL_RADIO_Init();
    applyDefaultAudioConfig();
    normalizeConfig();
    applyAudioConfigToCodec();
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    AEC_Reconfigure(s_config.aec_enabled, s_config.ai_noise_enabled);
#endif
    AUDIO_SetAecReferenceSource(s_config.aec_reference_source);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetHpDriveEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.hp_drive_enabled = enabled;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDrcEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.drc_enabled = enabled;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDrcWinsize(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value > 15U) {
        return false;
    }
    s_config.drc_winsize = value;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDrcMaxlevel(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value > 15U) {
        return false;
    }
    s_config.drc_maxlevel = value;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDrcMinlevel(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value > 15U) {
        return false;
    }
    s_config.drc_minlevel = value;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDacRamprate(const uint8_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value > 15U) {
        return false;
    }
    s_config.dac_ramprate = value;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDacEqBypass(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.dac_eq_bypass = enabled;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetDacEqCoefficients(const uint32_t b0,
                                         const uint32_t b1,
                                         const uint32_t a1,
                                         const bool persist)
{
    EXTERNAL_RADIO_Init();
    if ((b0 & ~kDacEqCoefficientMask) != 0U ||
        (b1 & ~kDacEqCoefficientMask) != 0U ||
        (a1 & ~kDacEqCoefficientMask) != 0U) {
        return false;
    }
    s_config.daceq_b0 = b0;
    s_config.daceq_b1 = b1;
    s_config.daceq_a1 = a1;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAdcSystemConfig(const bool dmic_enabled,
                                       const bool linsel,
                                       const uint8_t pga_gain,
                                       const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (pga_gain > 10U) {
        return false;
    }
    s_config.adc_dmic_enabled = dmic_enabled;
    s_config.adc_linsel = linsel;
    s_config.adc_pga_gain = pga_gain;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAdcRampConfig(const uint8_t ramprate,
                                     const bool dmic_sense,
                                     const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (ramprate > 15U) {
        return false;
    }
    s_config.adc_ramprate = ramprate;
    s_config.adc_dmic_sense = dmic_sense;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAdcScaleConfig(const bool sync,
                                      const bool inv,
                                      const bool ramclr,
                                      const uint8_t scale,
                                      const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (scale > 7U) {
        return false;
    }
    s_config.adc_sync = sync;
    s_config.adc_inv = inv;
    s_config.adc_ramclr = ramclr;
    s_config.adc_scale = scale;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAlcConfig(const bool enabled,
                                 const bool automute_enabled,
                                 const uint8_t winsize,
                                 const uint8_t maxlevel,
                                 const uint8_t minlevel,
                                 const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (winsize > 15U || maxlevel > 15U || minlevel > 15U) {
        return false;
    }
    s_config.alc_enabled = enabled;
    s_config.adc_automute_enabled = automute_enabled;
    s_config.alc_winsize = winsize;
    s_config.alc_maxlevel = maxlevel;
    s_config.alc_minlevel = minlevel;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAdcAutomuteConfig(const uint8_t winsize,
                                         const uint8_t noise_gate,
                                         const uint8_t volume,
                                         const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (winsize > 15U || noise_gate > 15U || volume > 7U) {
        return false;
    }
    s_config.adc_automute_winsize = winsize;
    s_config.adc_automute_noise_gate = noise_gate;
    s_config.adc_automute_volume = volume;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAdcHpfConfig(const uint8_t hpfs1,
                                    const bool eq_bypass,
                                    const bool hpf,
                                    const uint8_t hpfs2,
                                    const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (hpfs1 > 31U || hpfs2 > 31U) {
        return false;
    }
    s_config.adc_hpfs1 = hpfs1;
    s_config.adc_eq_bypass = eq_bypass;
    s_config.adc_hpf = hpf;
    s_config.adc_hpfs2 = hpfs2;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAdcEqCoefficients(const uint32_t b0,
                                         const uint32_t a1,
                                         const uint32_t a2,
                                         const uint32_t b1,
                                         const uint32_t b2,
                                         const bool persist)
{
    EXTERNAL_RADIO_Init();
    if ((b0 & ~kAdcEqCoefficientMask) != 0U ||
        (a1 & ~kAdcEqCoefficientMask) != 0U ||
        (a2 & ~kAdcEqCoefficientMask) != 0U ||
        (b1 & ~kAdcEqCoefficientMask) != 0U ||
        (b2 & ~kAdcEqCoefficientMask) != 0U) {
        return false;
    }
    s_config.adceq_b0 = b0;
    s_config.adceq_a1 = a1;
    s_config.adceq_a2 = a2;
    s_config.adceq_b1 = b1;
    s_config.adceq_b2 = b2;
    applyAudioConfigToCodec();
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetSciConfig(const uint32_t baud,
                                 const uint8_t data_bits,
                                 const char parity,
                                 const uint8_t stop_bits,
                                 const bool persist)
{
    EXTERNAL_RADIO_Init();
    const char normalized_parity = normalizeParity(parity);
    if (!isValidSciConfig(baud, data_bits, normalized_parity, stop_bits)) {
        return false;
    }

    s_config.sci.baud = baud;
    s_config.sci.data_bits = data_bits;
    s_config.sci.parity = normalized_parity;
    s_config.sci.stop_bits = stop_bits;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAecEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.aec_enabled = enabled;
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    AEC_SetRuntimeEnabled(s_config.aec_enabled, s_config.ai_noise_enabled);
#endif
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAecReferenceSource(const uint8_t source, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.aec_reference_source = normalizeAecReferenceSource(source);
    AUDIO_SetAecReferenceSource(s_config.aec_reference_source);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetAiNoiseEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    const bool changed = (s_config.ai_noise_enabled != enabled);
    s_config.ai_noise_enabled = enabled;
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    // afe_ns_mode (NSNET2 vs NONE) is baked at AFE create time, so a runtime
    // routing flag alone cannot stop the RNN. Re-create the AFE so the
    // toggle has real effect; expect ~100 ms of audio dropout while it
    // happens.
    if (changed) {
        AEC_Reconfigure(s_config.aec_enabled, s_config.ai_noise_enabled);
    } else {
        AEC_SetRuntimeEnabled(s_config.aec_enabled, s_config.ai_noise_enabled);
    }
#endif
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetMicHpfEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.mic_hpf_enabled = enabled;
    AUDIO_SetMicHpfEnabled(enabled);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetBtEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.bt_enabled = enabled;
    NRL_BtHfp_SetEnabled(enabled);
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetWifiEnabled(const bool enabled, const bool persist)
{
    EXTERNAL_RADIO_Init();
    s_config.wifi_enabled = enabled;
    // The audio-bridge task owns the Wi-Fi state machine; it polls this flag in
    // ensureWifiAndUdp() and brings Wi-Fi down (freeing the shared radio for
    // Bluetooth/A2DP) or back up accordingly, so nothing else is done here.
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetPttTimeout(const uint16_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value < kMinPttTimeoutS || value > kMaxPttTimeoutS) {
        return false;
    }
    s_config.ptt_timeout_s = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetBatteryCalibration(const uint16_t scale_milli, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (scale_milli < kMinBatteryCalMilli || scale_milli > kMaxBatteryCalMilli) {
        return false;
    }
    s_config.battery_cal_milli = scale_milli;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetVoicePayloadBytes(const uint16_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value < kMinVoicePayloadBytes || value > kMaxVoicePayloadBytes) {
        return false;
    }
    s_config.voice_payload_bytes = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}

bool EXTERNAL_RADIO_SetTailSuppressMs(const uint16_t value, const bool persist)
{
    EXTERNAL_RADIO_Init();
    if (value > kMaxTailSuppressMs) {
        return false;
    }
    s_config.tail_suppress_ms = value;
    if (persist) {
        return savePersistedConfig();
    }
    return true;
}
