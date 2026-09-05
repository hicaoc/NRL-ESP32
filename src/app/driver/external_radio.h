#ifndef DRIVER_EXTERNAL_RADIO_H
#define DRIVER_EXTERNAL_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void EXTERNAL_RADIO_Init(void);
uint8_t EXTERNAL_RADIO_GetChannel(void);
bool EXTERNAL_RADIO_SetChannel(uint8_t channel, bool persist);
bool EXTERNAL_RADIO_SaveConfig(void);
bool EXTERNAL_RADIO_ResetNetworkConfig(void);

#ifdef __cplusplus
}

struct SciSerialConfig {
    uint32_t baud;
    uint8_t data_bits;
    char parity;
    uint8_t stop_bits;
};

constexpr size_t EXTERNAL_RADIO_MAX_WIFI_PROFILES = 5U;

struct ExternalWifiProfile {
    char ssid[33];
    char password[65];
};

enum ExternalRadioAecReferenceSource : uint8_t {
    EXTERNAL_RADIO_AEC_REF_NETWORK = 0,
    EXTERNAL_RADIO_AEC_REF_MIC = 1,
};

struct ExternalRadioConfig {
    uint8_t channel;
    uint16_t server_port;
    uint16_t local_port;
    uint8_t callsign_ssid;
    uint8_t device_mode;
    uint8_t mic_volume;
    // Software gain applied to captured PCM, in 1/1000 units (1000 = 1.0x).
    // Valid range: 100..5000 (0.1x..5.0x).
    uint16_t mic_pcm_gain_milli;
    uint8_t line_out_volume;
    bool hp_drive_enabled;
    bool drc_enabled;
    uint8_t drc_winsize;
    uint8_t drc_maxlevel;
    uint8_t drc_minlevel;
    uint8_t dac_ramprate;
    bool dac_eq_bypass;
    uint32_t daceq_b0;
    uint32_t daceq_b1;
    uint32_t daceq_a1;
    bool adc_dmic_enabled;
    bool adc_linsel;
    uint8_t adc_pga_gain;
    uint8_t adc_ramprate;
    bool adc_dmic_sense;
    bool adc_sync;
    bool adc_inv;
    bool adc_ramclr;
    uint8_t adc_scale;
    bool alc_enabled;
    bool adc_automute_enabled;
    uint8_t alc_winsize;
    uint8_t alc_maxlevel;
    uint8_t alc_minlevel;
    uint8_t adc_automute_winsize;
    uint8_t adc_automute_noise_gate;
    uint8_t adc_automute_volume;
    uint8_t adc_hpfs1;
    bool adc_eq_bypass;
    bool adc_hpf;
    uint8_t adc_hpfs2;
    uint32_t adceq_b0;
    uint32_t adceq_a1;
    uint32_t adceq_a2;
    uint32_t adceq_b1;
    uint32_t adceq_b2;
    uint32_t wifi_ip;
    uint32_t wifi_netmask;
    uint32_t wifi_gateway;
    uint32_t wifi_dns;
    bool wifi_dhcp_enabled;
    bool aec_enabled;
    bool ai_noise_enabled;
    uint8_t aec_reference_source;
    // Software high-pass filter on captured mic audio (1-pole IIR,
    // ~200 Hz cutoff at 8 kHz). Strips DC offset and low-frequency rumble
    // before AEC / network uplink. See AUDIO_SetMicHpfEnabled().
    bool mic_hpf_enabled;
    // Route the project's voice through a Bluetooth headset (HFP). S31 only.
    bool bt_enabled;
    // Master Wi-Fi enable. Off = free the shared 2.4 GHz radio for Bluetooth
    // (so A2DP can stream music to the headset) at the cost of the network
    // radio-voice link. Defaults ON. S31 only.
    bool wifi_enabled;
    uint16_t ptt_timeout_s;
    // Battery-voltage calibration factor, in units of 1/1000 (1000 = 1.000x).
    // The raw ADC reading is multiplied by this divided by 1000 to compensate
    // for divider-resistor tolerance. Valid range: 500..2000.
    uint16_t battery_cal_milli;
    // G.711 A-law payload size, in bytes, of each outbound voice packet.
    // Valid range: 160..500 (20..62.5 ms of audio at 8 kHz). The actual UDP
    // packet length is this plus the 48-byte NRL header.
    uint16_t voice_payload_bytes;
    // Tail-audio suppression window, in milliseconds. After the device finishes
    // playing a network voice stream out to the radio, captured radio audio is
    // dropped (not forwarded to the network) for this long. This breaks the
    // echo loop a repeater's response would otherwise create between two or more
    // networked devices. 0 disables suppression; valid range 0..5000.
    uint16_t tail_suppress_ms;
    // Software VOX (voice-operated transmit): keys the soft PTT when the mic
    // frame level stays at or above vox_open_db (dBFS) for vox_attack_ms and
    // releases it vox_hang_ms after the level drops below vox_close_db.
    // Distinct from the SR-110U module's hardware VOX (AT+RADIO_VOX).
    bool vox_enabled;
    int8_t vox_open_db;
    int8_t vox_close_db;
    uint16_t vox_attack_ms;
    uint16_t vox_hang_ms;
    SciSerialConfig sci;
    // Saved Wi-Fi networks in connection-priority order. Entry 0 mirrors the
    // legacy wifi_ssid/wifi_password fields so existing AT/BLE/LCD callers
    // remain source-compatible.
    ExternalWifiProfile wifi_profiles[EXTERNAL_RADIO_MAX_WIFI_PROFILES];
    char wifi_ssid[33];
    char wifi_password[65];
    char server_host[65];
    char callsign[7];
};

const ExternalRadioConfig *EXTERNAL_RADIO_GetConfig(void);
bool EXTERNAL_RADIO_SetWifiSsid(const char *value, bool persist);
bool EXTERNAL_RADIO_SetWifiPassword(const char *value, bool persist);
size_t EXTERNAL_RADIO_GetWifiProfileCount(void);
bool EXTERNAL_RADIO_AddWifiProfile(const char *ssid, const char *password, bool persist);
bool EXTERNAL_RADIO_RemoveWifiProfile(size_t index, bool persist);
bool EXTERNAL_RADIO_MoveWifiProfile(size_t index, int direction, bool persist);
bool EXTERNAL_RADIO_SetWifiIp(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiNetmask(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiGateway(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiDns(uint32_t value, bool persist);
bool EXTERNAL_RADIO_SetWifiDhcpEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetServerHost(const char *value, bool persist);
bool EXTERNAL_RADIO_SetServerPort(uint16_t value, bool persist);
bool EXTERNAL_RADIO_SetLocalPort(uint16_t value, bool persist);
bool EXTERNAL_RADIO_SetCallsign(const char *value, bool persist);
bool EXTERNAL_RADIO_SetCallsignSsid(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetMicVolume(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetMicPcmGain(uint16_t gain_milli, bool persist);
bool EXTERNAL_RADIO_SetLineOutVolume(uint8_t value, bool persist);
bool EXTERNAL_RADIO_ResetAudioConfig(bool persist);
bool EXTERNAL_RADIO_SetHpDriveEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetDrcEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetDrcWinsize(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDrcMaxlevel(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDrcMinlevel(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDacRamprate(uint8_t value, bool persist);
bool EXTERNAL_RADIO_SetDacEqBypass(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetDacEqCoefficients(uint32_t b0, uint32_t b1, uint32_t a1, bool persist);
bool EXTERNAL_RADIO_SetAdcSystemConfig(bool dmic_enabled, bool linsel, uint8_t pga_gain, bool persist);
bool EXTERNAL_RADIO_SetAdcRampConfig(uint8_t ramprate, bool dmic_sense, bool persist);
bool EXTERNAL_RADIO_SetAdcScaleConfig(bool sync, bool inv, bool ramclr, uint8_t scale, bool persist);
bool EXTERNAL_RADIO_SetAlcConfig(bool enabled, bool automute_enabled, uint8_t winsize, uint8_t maxlevel, uint8_t minlevel, bool persist);
bool EXTERNAL_RADIO_SetAdcAutomuteConfig(uint8_t winsize, uint8_t noise_gate, uint8_t volume, bool persist);
bool EXTERNAL_RADIO_SetAdcHpfConfig(uint8_t hpfs1, bool eq_bypass, bool hpf, uint8_t hpfs2, bool persist);
bool EXTERNAL_RADIO_SetAdcEqCoefficients(uint32_t b0, uint32_t a1, uint32_t a2, uint32_t b1, uint32_t b2, bool persist);
bool EXTERNAL_RADIO_SetSciConfig(uint32_t baud, uint8_t data_bits, char parity, uint8_t stop_bits, bool persist);
// Acoustic echo cancellation (Gezipai/esp-sr AEC). When AFE is resident, this
// toggles the processed path at runtime and can also persist the preference.
bool EXTERNAL_RADIO_SetAecEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetAecReferenceSource(uint8_t source, bool persist);
// esp-sr speech enhancement / AI noise reduction. When AFE is resident, this
// toggles the processed path at runtime and can also persist the preference.
bool EXTERNAL_RADIO_SetAiNoiseEnabled(bool enabled, bool persist);
// Software microphone high-pass filter (~200 Hz cutoff). Takes effect
// immediately on the running passthrough task; no codec restart needed.
bool EXTERNAL_RADIO_SetMicHpfEnabled(bool enabled, bool persist);
// Enable/disable the Bluetooth-headset (HFP) voice link. Persists the choice and
// brings the BT stack up/down immediately. No-op on non-S31 boards.
bool EXTERNAL_RADIO_SetBtEnabled(bool enabled, bool persist);
bool EXTERNAL_RADIO_SetWifiEnabled(bool enabled, bool persist);
// PTT button auto-off / maximum continuous transmit time, in seconds. A short
// press latches transmit on; it is forced off again after this many seconds.
bool EXTERNAL_RADIO_SetPttTimeout(uint16_t value, bool persist);
// Battery-voltage calibration. `scale_milli` is the multiplier in units of
// 1/1000 (1000 = no correction). Accepts 500..2000.
bool EXTERNAL_RADIO_SetBatteryCalibration(uint16_t scale_milli, bool persist);
// G.711 voice payload size per outbound NRL packet. Accepts 160..500 bytes.
bool EXTERNAL_RADIO_SetVoicePayloadBytes(uint16_t value, bool persist);
// Tail-audio suppression window in milliseconds (0 disables). Accepts 0..5000.
bool EXTERNAL_RADIO_SetTailSuppressMs(uint16_t value, bool persist);
// Software VOX (voice-operated transmit) on the mic uplink. open_db accepts
// -80..-10, close_db -90..-15 and must be <= open_db - 2, attack_ms 0..500,
// hang_ms 100..3000. Takes effect immediately on the running capture task.
bool EXTERNAL_RADIO_SetVoxConfig(bool enabled, int open_db, int close_db, uint16_t attack_ms, uint16_t hang_ms, bool persist);
#endif

#endif // DRIVER_EXTERNAL_RADIO_H
