#include "wifi_config_portal_view.h"

#include "nrl_net_compat.h"
#include "nrl_version.h"
#include "wifi_config_portal_page.generated.h"
#include "wifi_config_portal_sections.generated.h"
#include "wifi_update_portal_page.generated.h"
#include "../app/driver/board_pins.h"
#include "../app/driver/display.h"
#include "../app/driver/serial_port_config.h"
#include "../services/ai_assistant.h"
#include "../services/aprs_service.h"
#include "../services/espnow_link.h"
#include "../services/music_player.h"
#include "../services/nanny.h"
#include "../services/radio_favorites.h"
#include "../services/signaling_service.h"
#include "../services/storage_service.h"
#include "nrl_audio_bridge.h"

#include <stdio.h>
#include <string.h>

#include <string>

namespace {

constexpr unsigned long kDacEqCoefficientMax = 1073741823UL;

static std::string fromU32(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v));
    return std::string(buf);
}

static std::string fromI32(int32_t v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
    return std::string(buf);
}

static std::string formatHex32(const uint32_t value)
{
    char buffer[12];
    snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(value));
    return std::string(buffer);
}

static std::string htmlEscape(const char *text)
{
    std::string out;
    if (text == nullptr) {
        return out;
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        switch (text[i]) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += text[i];
                break;
        }
    }
    return out;
}

// JSON string escaping for values embedded in an inline <script> block:
// besides the usual JSON escapes, '<' is escaped so a stored URL/name can
// never smuggle a "</script>" into the page.
static std::string jsonScriptEscape(const char *text)
{
    std::string out;
    if (text == nullptr) {
        return out;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const char ch = text[i];
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '<':  out += "\\u003C"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

static const char kMusicBrowserSections[] = R"HTML(
<section class="panel" id="music-player-panel">
  <div class="section-head"><h2 data-i18n="musicLibrary">Music Playback</h2><span class="hint mono" id="music-player-status"></span></div>
  <div class="music-controls">
    <button class="secondary btn-small" type="button" onclick="musicLibraryAction('prev')" data-i18n="musicPrev">Previous</button>
    <button class="secondary btn-small" type="button" onclick="musicLibraryAction('stop')" data-i18n="musicStop">Stop</button>
    <button class="secondary btn-small" type="button" onclick="musicLibraryAction('next')" data-i18n="musicNext">Next</button>
    <button class="secondary btn-small" type="button" id="music-repeat-button" onclick="musicLibraryAction('repeat')" data-i18n="musicRepeatList">Repeat list</button>
  </div>
  <div class="music-browser-head">
    <button class="secondary btn-small" type="button" id="music-up-button" onclick="musicLibraryAction('up')" data-i18n="musicUp">Up</button>
    <span class="mono music-browser-path" id="music-browser-path">/</span>
    <button class="secondary btn-small" type="button" onclick="musicLibraryAction('refresh')" data-i18n="musicRefresh">Rescan</button>
  </div>
  <div id="music-library-list" class="music-library-list"><span class="hint" data-i18n="musicLoading">Loading...</span></div>
  <div class="music-pagination" id="music-pagination" hidden>
    <button class="secondary btn-small" type="button" onclick="musicLibraryPage(-1)" data-i18n="musicPagePrev">Previous page</button>
    <span class="hint" id="music-page-status"></span>
    <button class="secondary btn-small" type="button" onclick="musicLibraryPage(1)" data-i18n="musicPageNext">Next page</button>
  </div>
  <p class="hint" data-i18n="musicSourcesHint">Mounted SMB, TF/SD and USB sources appear here automatically. Tap a folder to browse and a track to play.</p>
</section>
)HTML";

// Replace every occurrence of `token` in `html` with `value`. Arduino's
// String::replace had identical semantics; we re-implement it on std::string
// because the standard library has no built-in "replace-all-substring".
static void replaceToken(std::string &html, const char *token, const std::string &value)
{
    const size_t token_len = strlen(token);
    if (token_len == 0u) {
        return;
    }
    size_t pos = 0u;
    while ((pos = html.find(token, pos)) != std::string::npos) {
        html.replace(pos, token_len, value);
        pos += value.size();
    }
}

static void replaceToken(std::string &html, const char *token, const char *value)
{
    replaceToken(html, token, std::string(value != nullptr ? value : ""));
}

static std::string buildPageWithFormSections(const std::string &form_sections)
{
    static const char kFormSectionsToken[] = "{{FORM_SECTIONS}}";
    const char *template_html = kWifiConfigPortalHtmlTemplate;
    const char *token = strstr(template_html, kFormSectionsToken);
    if (token == nullptr) {
        return std::string(template_html);
    }

    const size_t token_len = strlen(kFormSectionsToken);
    const size_t prefix_len = static_cast<size_t>(token - template_html);
    const size_t template_len = strlen(template_html);

    std::string html;
    html.reserve(template_len - token_len + form_sections.size() + 64);
    html.append(template_html, prefix_len);
    html += form_sections;
    html += token + token_len;
    return html;
}

static std::string checkedAttr(const bool checked)
{
    return checked ? std::string("checked") : std::string("");
}

static std::string boolValue(const bool enabled)
{
    return enabled ? std::string("1") : std::string("0");
}

static std::string ipToString(const uint32_t value)
{
    char buf[16] = {};
    nrlIpToString(value, buf, sizeof(buf));
    return std::string(buf);
}

static std::string wifiDisplayIp(const ExternalRadioConfig *config,
                                 const uint32_t configured_value,
                                 const uint32_t dhcp_value)
{
    if (config != nullptr && config->wifi_dhcp_enabled && nrlWifiStaConnected()) {
        return ipToString(dhcp_value);
    }
    return ipToString(configured_value);
}

static std::string buildDacEqSlider(const char *field_name,
                                    const char *label,
                                    const char *i18n_key,
                                    const uint32_t value)
{
    std::string html = std::string(kWifiConfigPortalEqSliderTemplate);
    std::string i18n_attr;
    if (i18n_key != nullptr && i18n_key[0] != '\0') {
        i18n_attr = std::string(" data-i18n=\"") + i18n_key + "\"";
    }
    replaceToken(html, "{{I18N_ATTR}}", i18n_attr);
    replaceToken(html, "{{LABEL}}", label);
    replaceToken(html, "{{FIELD}}", field_name);
    replaceToken(html, "{{MAX}}", fromU32(static_cast<uint32_t>(kDacEqCoefficientMax)));
    replaceToken(html, "{{VALUE}}", fromU32(value));
    replaceToken(html, "{{HEX_VALUE}}", formatHex32(value));
    return html;
}

static std::string buildAutoSubmitSlider(const char *field_name,
                                         const char *label,
                                         const char *i18n_key,
                                         const uint32_t min_value,
                                         const uint32_t max_value,
                                         const uint32_t value)
{
    std::string html = std::string(kWifiConfigPortalAutoSliderTemplate);
    std::string i18n_attr;
    if (i18n_key != nullptr && i18n_key[0] != '\0') {
        i18n_attr = std::string(" data-i18n=\"") + i18n_key + "\"";
    }
    replaceToken(html, "{{I18N_ATTR}}", i18n_attr);
    replaceToken(html, "{{LABEL}}", label);
    replaceToken(html, "{{FIELD}}", field_name);
    replaceToken(html, "{{MIN}}", fromU32(min_value));
    replaceToken(html, "{{MAX}}", fromU32(max_value));
    replaceToken(html, "{{VALUE}}", fromU32(value));
    return html;
}

static std::string buildAutoSubmitNumber(const char *field_name,
                                         const char *label,
                                         const char *i18n_key,
                                         const char *min_value,
                                         const char *max_value,
                                         const char *step,
                                         const char *value)
{
    std::string html = std::string(kWifiConfigPortalAutoNumberTemplate);
    std::string i18n_attr;
    if (i18n_key != nullptr && i18n_key[0] != '\0') {
        i18n_attr = std::string(" data-i18n=\"") + i18n_key + "\"";
    }
    replaceToken(html, "{{I18N_ATTR}}", i18n_attr);
    replaceToken(html, "{{LABEL}}", label);
    replaceToken(html, "{{FIELD}}", field_name);
    replaceToken(html, "{{MIN}}", min_value);
    replaceToken(html, "{{MAX}}", max_value);
    replaceToken(html, "{{STEP}}", step);
    replaceToken(html, "{{VALUE}}", value);
    return html;
}

static std::string formatMicPcmGain(const uint16_t gain_milli)
{
    char value[16];
    snprintf(value, sizeof(value), "%u.%03u",
             static_cast<unsigned>(gain_milli / 1000u),
             static_cast<unsigned>(gain_milli % 1000u));
    size_t len = strlen(value);
    while (len > 2u && value[len - 1u] == '0' && value[len - 2u] != '.') {
        value[--len] = '\0';
    }
    return std::string(value);
}

static std::string staIpOrNotConnected(uint32_t ip)
{
    return (ip != 0u) ? ipToString(ip) : std::string("not connected");
}

} // namespace

std::string WifiConfigPortalView_BuildNetworkSection(const ExternalRadioConfig *config,
                                                     const WifiConfigPortalScanEntry *scan_entries,
                                                     const size_t scan_count)
{
    std::string wifi_options;
    bool current_ssid_found = false;
    for (size_t i = 0; i < scan_count; ++i) {
        const std::string escaped = htmlEscape(scan_entries[i].ssid.c_str());
        std::string option = std::string(kWifiConfigPortalWifiOptionTemplate);
        replaceToken(option, "{{VALUE}}", escaped);
        if (strcmp(scan_entries[i].ssid.c_str(), config->wifi_ssid) == 0) {
            current_ssid_found = true;
            replaceToken(option, "{{SELECTED}}", " selected");
        }
        replaceToken(option, "{{SELECTED}}", "");
        replaceToken(option, "{{LABEL}}", escaped + " (" + fromI32(scan_entries[i].rssi) + " dBm)");
        wifi_options += option;
    }
    if (!current_ssid_found && config->wifi_ssid[0] != '\0') {
        const std::string escaped = htmlEscape(config->wifi_ssid);
        std::string option = std::string(kWifiConfigPortalWifiOptionTemplate);
        replaceToken(option, "{{VALUE}}", escaped);
        replaceToken(option, "{{SELECTED}}", " selected");
        replaceToken(option, "{{LABEL}}", escaped);
        wifi_options += option;
    }
    std::string html = std::string(kWifiConfigPortalNetworkSectionTemplate);
    std::string saved_profiles;
    const size_t profile_count = EXTERNAL_RADIO_GetWifiProfileCount();
    for (size_t i = 0; i < profile_count; ++i) {
        saved_profiles += "<form class=\"item-form\" method=\"post\" action=\"/save_wifi\" data-reload-on-save=\"1\">";
        saved_profiles += "<input type=\"hidden\" name=\"wifi_profile_index\" value=\"" + fromU32(i) + "\">";
        saved_profiles += "<label><span class=\"mono\">" + fromU32(i + 1U) + ". " +
                          htmlEscape(config->wifi_profiles[i].ssid) + "</span></label>";
        saved_profiles += "<div class=\"actions\">";
        if (i > 0U) saved_profiles += "<button class=\"btn-small secondary\" type=\"button\" onclick=\"submitFormAction(this,'wifi_move_up')\" data-i18n=\"wifiPriorityUp\">Up</button>";
        if (i + 1U < profile_count) saved_profiles += "<button class=\"btn-small secondary\" type=\"button\" onclick=\"submitFormAction(this,'wifi_move_down')\" data-i18n=\"wifiPriorityDown\">Down</button>";
        saved_profiles += "<button class=\"btn-small secondary\" type=\"button\" onclick=\"submitFormAction(this,'wifi_delete')\" data-i18n=\"wifiDelete\">Delete</button></div></form>";
    }
    if (saved_profiles.empty()) {
        saved_profiles = "<span class=\"hint\" data-i18n=\"wifiNoSaved\">No saved WiFi networks.</span>";
    }
    replaceToken(html, "{{WIFI_OPTIONS}}", wifi_options);
    replaceToken(html, "{{WIFI_SAVED_PROFILES}}", saved_profiles);
    replaceToken(html, "{{WIFI_PASSWORD}}", htmlEscape(config->wifi_password));
    replaceToken(html, "{{DHCP_CHECKED}}", checkedAttr(config->wifi_dhcp_enabled));
    replaceToken(html, "{{WIFI_IP}}", wifiDisplayIp(config, config->wifi_ip, nrlWifiStaIp()));
    replaceToken(html, "{{WIFI_MASK}}", wifiDisplayIp(config, config->wifi_netmask, nrlWifiStaNetmask()));
    replaceToken(html, "{{WIFI_GATEWAY}}", wifiDisplayIp(config, config->wifi_gateway, nrlWifiStaGateway()));
    replaceToken(html, "{{WIFI_DNS}}", wifiDisplayIp(config, config->wifi_dns, nrlWifiStaDns()));
    return html;
}

std::string WifiConfigPortalView_BuildDeviceSections(const ExternalRadioConfig *config)
{
    std::string html = std::string(kWifiConfigPortalDeviceSectionsTemplate);
    replaceToken(html, "{{SERVER_HOST}}", htmlEscape(config->server_host));
    replaceToken(html, "{{SERVER_PORT}}", fromU32(config->server_port));
    replaceToken(html, "{{CHANNEL}}", fromU32(config->channel));
    replaceToken(html, "{{CALLSIGN}}", htmlEscape(config->callsign));
    replaceToken(html, "{{CALLSIGN_SSID}}", fromU32(config->callsign_ssid));
    replaceToken(html, "{{PTT_TIMEOUT}}", fromU32(config->ptt_timeout_s));
    replaceToken(html, "{{VOICE_PAYLOAD_BYTES}}", fromU32(config->voice_payload_bytes));
    replaceToken(html, "{{TAIL_SUPPRESS_MS}}", fromU32(config->tail_suppress_ms));
    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    replaceToken(html, "{{UART1_ENABLED_CHECKED}}", checkedAttr(serial.uart1_enabled));
    replaceToken(html, "{{UART2_ENABLED_CHECKED}}", checkedAttr(serial.uart2_enabled));
    replaceToken(html, "{{UART1_RX_PIN}}", fromI32(serial.uart1_rx_pin));
    replaceToken(html, "{{UART1_TX_PIN}}", fromI32(serial.uart1_tx_pin));
    replaceToken(html, "{{UART1_BAUD}}", fromU32(config->sci.baud));
    replaceToken(html, "{{UART1_DATA_BITS}}", fromU32(config->sci.data_bits));
    replaceToken(html, "{{UART1_PARITY}}", std::string(1, config->sci.parity));
    replaceToken(html, "{{UART1_STOP_BITS}}", fromU32(config->sci.stop_bits));
    replaceToken(html, "{{UART2_RX_PIN}}", fromI32(serial.uart2_rx_pin));
    replaceToken(html, "{{UART2_TX_PIN}}", fromI32(serial.uart2_tx_pin));
    replaceToken(html, "{{UART2_BAUD}}", fromU32(serial.uart2_baud));
    replaceToken(html, "{{UART2_DATA_BITS}}", fromU32(serial.uart2_data_bits));
    replaceToken(html, "{{UART2_PARITY}}", std::string(1, serial.uart2_parity));
    replaceToken(html, "{{UART2_STOP_BITS}}", fromU32(serial.uart2_stop_bits));
    const uint8_t nrl_codec = NRLAudioBridge_GetVoiceCodec();
    replaceToken(html, "{{CODEC_G711_SELECTED}}", nrl_codec == 0u ? " selected" : "");
    replaceToken(html, "{{CODEC_OPUS_SELECTED}}", nrl_codec == 1u ? " selected" : "");
#if NRL_BOARD_IS_GEZIPAI_FAMILY
    std::string battery_section = std::string(kWifiConfigPortalBatterySectionTemplate);
    replaceToken(battery_section, "{{BATT_RAW_MV}}", fromI32(Display_GetBatteryRawMv()));
    replaceToken(battery_section, "{{BATT_CAL_MV}}", fromI32(Display_GetBatteryCalibratedMv()));
    replaceToken(battery_section, "{{BATT_CAL_MILLI}}", fromU32(config->battery_cal_milli));
    replaceToken(html, "{{BATTERY_SECTION}}", battery_section);
#else
    replaceToken(html, "{{BATTERY_SECTION}}", std::string(""));
#endif
    return html;
}

std::string WifiConfigPortalView_BuildAudioSections(const ExternalRadioConfig *config)
{
    std::string html = std::string(kWifiConfigPortalAudioSectionsTemplate);
    html.reserve(strlen(kWifiConfigPortalAudioSectionsTemplate) + 9000u);
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    std::string aec_section = std::string(kWifiConfigPortalAecSectionTemplate);
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    std::string aec_toggle = std::string(kWifiConfigPortalAecToggleTemplate);
    replaceToken(aec_toggle, "{{AEC_CHECKED}}", checkedAttr(config->aec_enabled));
    replaceToken(aec_toggle, "{{AEC_REF_NETWORK_SELECTED}}",
                 (config->aec_reference_source == EXTERNAL_RADIO_AEC_REF_NETWORK) ? " selected" : "");
    replaceToken(aec_toggle, "{{AEC_REF_MIC_SELECTED}}",
#if NRL_HAS_ES7210
                 (config->aec_reference_source == EXTERNAL_RADIO_AEC_REF_MIC) ? " selected" : ""
#else
                 " disabled"
#endif
    );
    replaceToken(aec_section, "{{AFE_GROUP_I18N}}", "aec");
    replaceToken(aec_section, "{{AFE_GROUP_LABEL}}", "AEC / AI");
#else
    std::string aec_toggle = "";
    replaceToken(aec_section, "{{AFE_GROUP_I18N}}", "aiNoiseLabel");
    replaceToken(aec_section, "{{AFE_GROUP_LABEL}}", "AI Noise Reduction");
#endif
    replaceToken(aec_section, "{{AEC_TOGGLE}}", aec_toggle);
    replaceToken(aec_section, "{{AI_NOISE_CHECKED}}", checkedAttr(config->ai_noise_enabled));
#endif
    replaceToken(html, "{{AEC_SECTION}}",
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
                 aec_section
#else
                 std::string("")
#endif
    );
    // ES8311 register-level expert panels only make sense on boards that
    // actually carry an ES8311 (the S31-Korvo uses an ES8389 with a totally
    // different register map). The ADC panel is further hidden when a
    // separate ES7210 captures the mic, since the ES8311 ADC path is unused.
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    replaceToken(html, "{{ES8311_ADC_SECTION}}",
#if NRL_HAS_ES7210
                 std::string("")
#else
                 std::string(kWifiConfigPortalEs8311AdcSectionTemplate)
#endif
    );
    replaceToken(html, "{{ES8311_DAC_SECTION}}",
                 std::string(kWifiConfigPortalEs8311DacSectionTemplate));
    replaceToken(html, "{{AUDIO_EXPERT_TOGGLE_HIDDEN}}", std::string(""));
#else
    replaceToken(html, "{{ES8311_ADC_SECTION}}", std::string(""));
    replaceToken(html, "{{ES8311_DAC_SECTION}}", std::string(""));
    // Without the expert panels the expert-mode switch would toggle nothing.
    replaceToken(html, "{{AUDIO_EXPERT_TOGGLE_HIDDEN}}", std::string(" hidden"));
#endif
    replaceToken(html, "{{HP_DRIVE_CHECKED}}", checkedAttr(config->hp_drive_enabled));
    replaceToken(html, "{{MIC_VOLUME_SLIDER}}", buildAutoSubmitSlider("mic_volume", "Mic Volume (0-255)", "micVolume", 0u, 255u, config->mic_volume));
    const std::string mic_pcm_gain = formatMicPcmGain(config->mic_pcm_gain_milli);
    replaceToken(html, "{{MIC_PCM_GAIN_INPUT}}",
                 buildAutoSubmitNumber("mic_pcm_gain", "Mic PCM Gain (0.1-5.0x)",
                                       "micPcmGain", "0.1", "5.0", "0.1",
                                       mic_pcm_gain.c_str()));
    replaceToken(html, "{{LINE_OUT_VOLUME_SLIDER}}", buildAutoSubmitSlider("line_out_volume", "Line Out Volume (0-255)", "lineOutVolume", 0u, 255u, config->line_out_volume));
    replaceToken(html, "{{ADC_DMIC_CHECKED}}", checkedAttr(config->adc_dmic_enabled));
    replaceToken(html, "{{ADC_DMIC_VALUE}}", boolValue(config->adc_dmic_enabled));
    replaceToken(html, "{{ADC_LINSEL_CHECKED}}", checkedAttr(config->adc_linsel));
    replaceToken(html, "{{ADC_LINSEL_VALUE}}", boolValue(config->adc_linsel));
    replaceToken(html, "{{ADC_PGA_GAIN}}", fromU32(config->adc_pga_gain));
    replaceToken(html, "{{ADC_RAMPRATE}}", fromU32(config->adc_ramprate));
    replaceToken(html, "{{ADC_SCALE}}", fromU32(config->adc_scale));
    replaceToken(html, "{{ADC_PGA_GAIN_SLIDER}}", buildAutoSubmitSlider("adc_pga_gain", "ADC PGA Gain (0-10)", "adcPgaGain", 0u, 10u, config->adc_pga_gain));
    replaceToken(html, "{{ADC_RAMPRATE_SLIDER}}", buildAutoSubmitSlider("adc_ramprate", "ADC VC Ramp Rate (0-15)", "adcRampRate", 0u, 15u, config->adc_ramprate));
    replaceToken(html, "{{ADC_SCALE_SLIDER}}", buildAutoSubmitSlider("adc_scale", "ADC Gain Scale (0-7)", "adcGainScale", 0u, 7u, config->adc_scale));
    replaceToken(html, "{{ADC_DMIC_SENSE_CHECKED}}", checkedAttr(config->adc_dmic_sense));
    replaceToken(html, "{{ADC_SYNC_CHECKED}}", checkedAttr(config->adc_sync));
    replaceToken(html, "{{ADC_SYNC_VALUE}}", boolValue(config->adc_sync));
    replaceToken(html, "{{ADC_INV_CHECKED}}", checkedAttr(config->adc_inv));
    replaceToken(html, "{{ADC_INV_VALUE}}", boolValue(config->adc_inv));
    replaceToken(html, "{{ADC_RAMCLR_CHECKED}}", checkedAttr(config->adc_ramclr));
    replaceToken(html, "{{ADC_RAMCLR_VALUE}}", boolValue(config->adc_ramclr));
    replaceToken(html, "{{ALC_CHECKED}}", checkedAttr(config->alc_enabled));
    replaceToken(html, "{{ALC_ENABLED_VALUE}}", boolValue(config->alc_enabled));
    replaceToken(html, "{{ADC_AUTOMUTE_CHECKED}}", checkedAttr(config->adc_automute_enabled));
    replaceToken(html, "{{ADC_AUTOMUTE_ENABLED_VALUE}}", boolValue(config->adc_automute_enabled));
    replaceToken(html, "{{ALC_WINSIZE_SLIDER}}", buildAutoSubmitSlider("alc_winsize", "ALC Window Size (0-15)", "alcWindowSize", 0u, 15u, config->alc_winsize));
    replaceToken(html, "{{ALC_MAXLEVEL_SLIDER}}", buildAutoSubmitSlider("alc_maxlevel", "ALC Max Level (0-15)", "alcMaxLevel", 0u, 15u, config->alc_maxlevel));
    replaceToken(html, "{{ALC_MINLEVEL_SLIDER}}", buildAutoSubmitSlider("alc_minlevel", "ALC Min Level (0-15)", "alcMinLevel", 0u, 15u, config->alc_minlevel));
    replaceToken(html, "{{ADC_AUTOMUTE_WINSIZE_SLIDER}}", buildAutoSubmitSlider("adc_automute_winsize", "ADC Automute Window (0-15)", "adcAutomuteWindow", 0u, 15u, config->adc_automute_winsize));
    replaceToken(html, "{{ADC_AUTOMUTE_NOISE_GATE_SLIDER}}", buildAutoSubmitSlider("adc_automute_noise_gate", "ADC Automute Noise Gate (0-15)", "adcAutomuteNoiseGate", 0u, 15u, config->adc_automute_noise_gate));
    replaceToken(html, "{{ADC_AUTOMUTE_VOLUME_SLIDER}}", buildAutoSubmitSlider("adc_automute_volume", "ADC Automute Volume (0-7)", "adcAutomuteVolume", 0u, 7u, config->adc_automute_volume));
    replaceToken(html, "{{ADC_HPFS1}}", fromU32(config->adc_hpfs1));
    replaceToken(html, "{{ADC_HPFS2}}", fromU32(config->adc_hpfs2));
    replaceToken(html, "{{ADC_HPFS1_SLIDER}}", buildAutoSubmitSlider("adc_hpfs1", "ADC HPF Stage 1 (0-31)", "adcHpfStage1", 0u, 31u, config->adc_hpfs1));
    replaceToken(html, "{{ADC_HPFS2_SLIDER}}", buildAutoSubmitSlider("adc_hpfs2", "ADC HPF Stage 2 (0-31)", "adcHpfStage2", 0u, 31u, config->adc_hpfs2));
    replaceToken(html, "{{ADC_EQ_BYPASS_CHECKED}}", checkedAttr(config->adc_eq_bypass));
    replaceToken(html, "{{ADC_EQ_BYPASS_VALUE}}", boolValue(config->adc_eq_bypass));
    replaceToken(html, "{{ADC_HPF_CHECKED}}", checkedAttr(config->adc_hpf));
    replaceToken(html, "{{ADC_HPF_VALUE}}", boolValue(config->adc_hpf));
    replaceToken(html, "{{MIC_HPF_CHECKED}}", checkedAttr(config->mic_hpf_enabled));
    replaceToken(html, "{{ADCEQ_B0_SLIDER}}", buildDacEqSlider("adceq_b0", "ADCEQ B0 (REG1D-20)", "adceqB0", config->adceq_b0));
    replaceToken(html, "{{ADCEQ_A1_SLIDER}}", buildDacEqSlider("adceq_a1", "ADCEQ A1 (REG21-24)", "adceqA1", config->adceq_a1));
    replaceToken(html, "{{ADCEQ_A2_SLIDER}}", buildDacEqSlider("adceq_a2", "ADCEQ A2 (REG25-28)", "adceqA2", config->adceq_a2));
    replaceToken(html, "{{ADCEQ_B1_SLIDER}}", buildDacEqSlider("adceq_b1", "ADCEQ B1 (REG29-2C)", "adceqB1", config->adceq_b1));
    replaceToken(html, "{{ADCEQ_B2_SLIDER}}", buildDacEqSlider("adceq_b2", "ADCEQ B2 (REG2D-30)", "adceqB2", config->adceq_b2));
    replaceToken(html, "{{DRC_CHECKED}}", checkedAttr(config->drc_enabled));
    replaceToken(html, "{{DRC_WINSIZE_SLIDER}}", buildAutoSubmitSlider("drc_winsize", "DRC Window Size (0-15)", "drcWindowSize", 0u, 15u, config->drc_winsize));
    replaceToken(html, "{{DRC_MAXLEVEL_SLIDER}}", buildAutoSubmitSlider("drc_maxlevel", "DRC Max Level (0-15)", "drcMaxLevel", 0u, 15u, config->drc_maxlevel));
    replaceToken(html, "{{DRC_MINLEVEL_SLIDER}}", buildAutoSubmitSlider("drc_minlevel", "DRC Min Level (0-15)", "drcMinLevel", 0u, 15u, config->drc_minlevel));
    replaceToken(html, "{{DAC_RAMPRATE_SLIDER}}", buildAutoSubmitSlider("dac_ramprate", "DAC Ramp Rate (0-15)", "dacRampRate", 0u, 15u, config->dac_ramprate));
    replaceToken(html, "{{DAC_EQ_BYPASS_CHECKED}}", checkedAttr(config->dac_eq_bypass));
    replaceToken(html, "{{DACEQ_B0_SLIDER}}", buildDacEqSlider("daceq_b0", "DACEQ B0 (REG38-3B)", "daceqB0", config->daceq_b0));
    replaceToken(html, "{{DACEQ_B1_SLIDER}}", buildDacEqSlider("daceq_b1", "DACEQ B1 (REG3C-3F)", "daceqB1", config->daceq_b1));
    replaceToken(html, "{{DACEQ_A1_SLIDER}}", buildDacEqSlider("daceq_a1", "DACEQ A1 (REG40-43)", "daceqA1", config->daceq_a1));
    return html;
}

std::string WifiConfigPortalView_BuildMediaSections(void)
{
#if NRL_BOARD == NRL_BOARD_S31_KORVO || NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    std::string html = std::string(kWifiConfigPortalMediaSectionsTemplate);

    const int target = MUSIC_GetTarget();
    replaceToken(html, "{{TARGET_LOCAL_SELECTED}}", target == MUSIC_TARGET_LOCAL ? " selected" : "");
    replaceToken(html, "{{TARGET_NET_SELECTED}}", target == MUSIC_TARGET_NET ? " selected" : "");
    replaceToken(html, "{{TARGET_BOTH_SELECTED}}", target == MUSIC_TARGET_BOTH ? " selected" : "");
    const int output = MUSIC_GetOutput();
    replaceToken(html, "{{OUTPUT_SPK_SELECTED}}", output == MUSIC_OUTPUT_SPEAKER ? " selected" : "");
    replaceToken(html, "{{OUTPUT_BT_SELECTED}}", output == MUSIC_OUTPUT_BT ? " selected" : "");
    const uint8_t codec = NRLAudioBridge_GetVoiceCodec();
    replaceToken(html, "{{CODEC_G711_SELECTED}}", codec == 0u ? " selected" : "");
    replaceToken(html, "{{CODEC_OPUS_SELECTED}}", codec == 1u ? " selected" : "");
    replaceToken(html, "{{ESPNOW_CHECKED}}", checkedAttr(ESPNOW_LINK_IsEnabled()));
    replaceToken(html, "{{ESPNOW_RX_CHECKED}}", checkedAttr(ESPNOW_LINK_IsRxEnabled()));
    const uint8_t espnow_codec = ESPNOW_LINK_GetTxCodec();
    replaceToken(html, "{{ESPNOW_CODEC_G711_SELECTED}}", espnow_codec == 0u ? " selected" : "");
    replaceToken(html, "{{ESPNOW_CODEC_OPUS_SELECTED}}", espnow_codec == 1u ? " selected" : "");
    const uint8_t ptt_mode = ESPNOW_LINK_GetPttMode();
    replaceToken(html, "{{PTT_MODE_NRL_SELECTED}}", ptt_mode == 0u ? " selected" : "");
    replaceToken(html, "{{PTT_MODE_ESPNOW_SELECTED}}", ptt_mode == 1u ? " selected" : "");

    char ai_url[160] = {};
    char ai_token[96] = {};
    AI_GetConfig(ai_url, sizeof(ai_url), ai_token, sizeof(ai_token));
    replaceToken(html, "{{AI_URL}}", htmlEscape(ai_url));
    replaceToken(html, "{{AI_TOKEN}}", htmlEscape(ai_token));
    replaceToken(html, "{{AI_CHECKED}}", checkedAttr(AI_IsEnabled()));
    char ai_status[224] = {};
    AI_Describe(ai_status, sizeof(ai_status));
    replaceToken(html, "{{AI_STATUS}}", htmlEscape(ai_status));

    char beacon_path[128] = {};
    uint32_t beacon_interval = 0;
    const bool beacon_armed = NANNY_GetBeacon(beacon_path, sizeof(beacon_path), &beacon_interval);
    replaceToken(html, "{{BEACON_PATH}}", htmlEscape(beacon_path));
    replaceToken(html, "{{BEACON_INTERVAL}}",
                 beacon_armed ? fromU32(beacon_interval) : std::string(""));
    replaceToken(html, "{{BEACON_CHECKED}}", checkedAttr(beacon_armed));

    char radio_url[256] = {};
    MUSIC_GetRadioUrl(radio_url, sizeof(radio_url));
    replaceToken(html, "{{RADIO_URL}}", htmlEscape(radio_url));
    const char *playing_path = MUSIC_CurrentPath();
    const bool radio_playing = MUSIC_IsPlaying() &&
                               strncmp(playing_path, "http", 4) == 0;
    replaceToken(html, "{{RADIO_STATUS}}",
                 radio_playing ? (std::string("&#9654; ") + htmlEscape(playing_path)) : std::string(""));

    std::string favs_json = "[";
    const size_t fav_count = RADIO_FAV_Count();
    for (size_t i = 0; i < fav_count; ++i) {
        char fav_name[RADIO_FAV_NAME_SIZE] = {};
        char fav_url[RADIO_FAV_URL_SIZE] = {};
        if (!RADIO_FAV_Get(i, fav_name, sizeof(fav_name), fav_url, sizeof(fav_url))) {
            break;
        }
        if (i > 0u) {
            favs_json += ",";
        }
        favs_json += "{\"name\":\"" + jsonScriptEscape(fav_name) +
                     "\",\"url\":\"" + jsonScriptEscape(fav_url) + "\"}";
    }
    favs_json += "]";
    replaceToken(html, "{{RADIO_FAVS_JSON}}", favs_json);
    replaceToken(html, "{{RADIO_FAV_CUR}}", fromI32(RADIO_FAV_CurrentIndex()));

    char smb_server[64] = {};
    char smb_share[64] = {};
    char smb_user[32] = {};
    char smb_pass[64] = {};
    (void)STORAGE_SmbGetConfig(smb_server, sizeof(smb_server), smb_share, sizeof(smb_share),
                               smb_user, sizeof(smb_user), smb_pass, sizeof(smb_pass));
    replaceToken(html, "{{SMB_SERVER}}", htmlEscape(smb_server));
    replaceToken(html, "{{SMB_SHARE}}", htmlEscape(smb_share));
    replaceToken(html, "{{SMB_USER}}", htmlEscape(smb_user));
    replaceToken(html, "{{SMB_PASSWORD}}", htmlEscape(smb_pass));
    char smb_status[128] = {};
    STORAGE_SmbDescribe(smb_status, sizeof(smb_status));
    replaceToken(html, "{{SMB_STATUS}}", htmlEscape(smb_status));
    replaceToken(html, "{{MUSIC_BROWSER}}", kMusicBrowserSections);
    return html;
#else
    std::string html = R"HTML(
        <section class="panel">
          <div class="section-head"><h2 data-i18n="musicTarget">Playback Target</h2><span class="hint" data-i18n="musicTargetHint">One shared setting for everything the player outputs: music, nanny beacon, and net radio.</span></div>
          <div class="grid">
            <form class="item-form" method="post" action="/save_media"><label data-i18n="musicTarget">Playback Target</label><select name="music_target" onchange="submitSwitch(this)"><option value="0"{{TARGET_LOCAL_SELECTED}} data-i18n="targetLocal">Local speaker</option><option value="1"{{TARGET_NET_SELECTED}} data-i18n="targetNet">NRL network</option><option value="2"{{TARGET_BOTH_SELECTED}} data-i18n="targetBoth">Local + network</option></select></form>
          </div>
        </section>
        <section class="panel">
          <div class="section-head"><h2 data-i18n="voiceLink">Voice Link</h2></div>
          <div class="grid"><form class="item-form" method="post" action="/save_media"><label data-i18n="pttMode">PTT Mode</label><select name="ptt_mode" onchange="submitSwitch(this)"><option value="0"{{PTT_MODE_NRL_SELECTED}} data-i18n="pttModeNrl">NRL network PTT</option><option value="1"{{PTT_MODE_ESPNOW_SELECTED}} data-i18n="pttModeEspnow">ESP-NOW PTT</option></select></form></div>
        </section>
        <section class="panel">
          <div class="section-head"><h2 data-i18n="espnowLabel">ESP-NOW Intercom</h2></div>
          <div class="grid">
            <form class="item-form" method="post" action="/save_media"><label data-i18n="espnowLabel">ESP-NOW Intercom</label><input type="hidden" name="espnow_present" value="1"><label class="hint"><input type="checkbox" name="espnow_enabled" value="1" onchange="submitSwitch(this)" {{ESPNOW_CHECKED}}><span data-i18n="espnowText">Off-grid voice link between nearby devices</span></label></form>
            <form class="item-form" method="post" action="/save_media"><label data-i18n="espnowRxLabel">ESP-NOW Receive</label><input type="hidden" name="espnow_rx_present" value="1"><label class="hint"><input type="checkbox" name="espnow_rx" value="1" onchange="submitSwitch(this)" {{ESPNOW_RX_CHECKED}}><span data-i18n="espnowRxText">Hear intercom voice even while TX stays off</span></label></form>
            <form class="item-form" method="post" action="/save_media"><label data-i18n="espnowCodec">ESP-NOW Voice Codec (TX)</label><select name="espnow_codec" onchange="submitSwitch(this)"><option value="0"{{ESPNOW_CODEC_G711_SELECTED}} data-i18n="codecG711">G.711 8 kHz (compatible)</option><option value="1"{{ESPNOW_CODEC_OPUS_SELECTED}} data-i18n="codecOpus">Opus 16 kHz wideband</option></select></form>
          </div>
        </section>
        <section class="panel">
          <div class="section-head"><h2 data-i18n="netRadio">Net Radio</h2><span class="hint mono">{{RADIO_STATUS}}</span></div>
          <div class="grid">
            <form class="item-form span-2" method="post" action="/save_media">
              <div class="subgrid"><div class="span-2"><label data-i18n="radioUrl">Stream URL (http:// or https://)</label><input name="radio_url" value="{{RADIO_URL}}" placeholder="http://..."></div></div>
              <div class="actions"><button class="btn-small" type="submit" data-i18n="saveItem">Save</button><button class="btn-small" type="button" onclick="submitFormAction(this, 'radio_play')" data-i18n="radioPlay">Play</button><button class="secondary btn-small" type="button" onclick="submitFormAction(this, 'radio_stop')" data-i18n="radioStop">Stop</button></div>
            </form>
            <div class="span-2"><div class="group-label" data-i18n="radioFavs">Favorite stations</div><div id="radio-fav-list" class="fav-list"></div></div>
            <form class="item-form span-2" method="post" action="/save_media" id="radio-fav-form">
              <div class="subgrid"><div><label data-i18n="radioFavName">Station name</label><input name="fav_name" value="" maxlength="47"></div><div><label data-i18n="radioFavUrl">Stream URL (http:// or https://)</label><input name="fav_url" value="" maxlength="199" placeholder="http://..."></div></div>
              <div class="actions"><button class="btn-small" type="button" onclick="addRadioFav(this)" data-i18n="radioFavAdd">Add favorite</button></div>
            </form>
            <script>window.RADIO_FAVS={{RADIO_FAVS_JSON}};window.RADIO_FAV_CUR={{RADIO_FAV_CUR}};</script>
          </div>
        </section>
        <section class="panel">
          <div class="section-head"><h2 data-i18n="smbShare">Network Share (SMB)</h2><span class="hint mono">{{SMB_STATUS}}</span></div>
          <div class="grid">
            <form class="item-form span-2" method="post" action="/save_media">
              <div class="subgrid"><div><label data-i18n="smbServer">Server (NAS / PC)</label><input name="smb_server" value="{{SMB_SERVER}}" placeholder="192.168.1.10"></div><div><label data-i18n="smbShareName">Share name</label><input name="smb_share" value="{{SMB_SHARE}}" placeholder="music"></div><div><label data-i18n="smbUser">Username (empty = guest)</label><input name="smb_user" value="{{SMB_USER}}"></div><div><label data-i18n="smbPassword">Password</label><input name="smb_password" type="password" value="{{SMB_PASSWORD}}"></div></div>
              <div class="actions"><button class="btn-small" type="submit" data-i18n="saveItem">Save</button><button class="secondary btn-small" type="button" onclick="submitFormAction(this, 'smb_clear')" data-i18n="smbClear">Clear</button></div>
            </form>
          </div>
        </section>
        {{MUSIC_BROWSER}}
    )HTML";
    const int target = MUSIC_GetTarget();
    replaceToken(html, "{{TARGET_LOCAL_SELECTED}}", target == MUSIC_TARGET_LOCAL ? " selected" : "");
    replaceToken(html, "{{TARGET_NET_SELECTED}}", target == MUSIC_TARGET_NET ? " selected" : "");
    replaceToken(html, "{{TARGET_BOTH_SELECTED}}", target == MUSIC_TARGET_BOTH ? " selected" : "");
    const uint8_t codec = NRLAudioBridge_GetVoiceCodec();
    replaceToken(html, "{{CODEC_G711_SELECTED}}", codec == 0u ? " selected" : "");
    replaceToken(html, "{{CODEC_OPUS_SELECTED}}", codec == 1u ? " selected" : "");
    replaceToken(html, "{{ESPNOW_CHECKED}}", checkedAttr(ESPNOW_LINK_IsEnabled()));
    replaceToken(html, "{{ESPNOW_RX_CHECKED}}", checkedAttr(ESPNOW_LINK_IsRxEnabled()));
    const uint8_t espnow_codec = ESPNOW_LINK_GetTxCodec();
    replaceToken(html, "{{ESPNOW_CODEC_G711_SELECTED}}", espnow_codec == 0u ? " selected" : "");
    replaceToken(html, "{{ESPNOW_CODEC_OPUS_SELECTED}}", espnow_codec == 1u ? " selected" : "");
    const uint8_t ptt_mode = ESPNOW_LINK_GetPttMode();
    replaceToken(html, "{{PTT_MODE_NRL_SELECTED}}", ptt_mode == 0u ? " selected" : "");
    replaceToken(html, "{{PTT_MODE_ESPNOW_SELECTED}}", ptt_mode == 1u ? " selected" : "");

    char radio_url[256] = {};
    MUSIC_GetRadioUrl(radio_url, sizeof(radio_url));
    replaceToken(html, "{{RADIO_URL}}", htmlEscape(radio_url));
    const char *playing_path = MUSIC_CurrentPath();
    const bool radio_playing = MUSIC_IsPlaying() && strncmp(playing_path, "http", 4) == 0;
    replaceToken(html, "{{RADIO_STATUS}}",
                 radio_playing ? (std::string("&#9654; ") + htmlEscape(playing_path)) : std::string(""));

    std::string favs_json = "[";
    const size_t fav_count = RADIO_FAV_Count();
    for (size_t i = 0; i < fav_count; ++i) {
        char fav_name[RADIO_FAV_NAME_SIZE] = {};
        char fav_url[RADIO_FAV_URL_SIZE] = {};
        if (!RADIO_FAV_Get(i, fav_name, sizeof(fav_name), fav_url, sizeof(fav_url))) break;
        if (i > 0u) favs_json += ",";
        favs_json += "{\"name\":\"" + jsonScriptEscape(fav_name) +
                     "\",\"url\":\"" + jsonScriptEscape(fav_url) + "\"}";
    }
    favs_json += "]";
    replaceToken(html, "{{RADIO_FAVS_JSON}}", favs_json);
    replaceToken(html, "{{RADIO_FAV_CUR}}", fromI32(RADIO_FAV_CurrentIndex()));

    char smb_server[64] = {};
    char smb_share[64] = {};
    char smb_user[32] = {};
    char smb_pass[64] = {};
    (void)STORAGE_SmbGetConfig(smb_server, sizeof(smb_server), smb_share, sizeof(smb_share),
                               smb_user, sizeof(smb_user), smb_pass, sizeof(smb_pass));
    replaceToken(html, "{{SMB_SERVER}}", htmlEscape(smb_server));
    replaceToken(html, "{{SMB_SHARE}}", htmlEscape(smb_share));
    replaceToken(html, "{{SMB_USER}}", htmlEscape(smb_user));
    replaceToken(html, "{{SMB_PASSWORD}}", htmlEscape(smb_pass));
    char smb_status[128] = {};
    STORAGE_SmbDescribe(smb_status, sizeof(smb_status));
    replaceToken(html, "{{SMB_STATUS}}", htmlEscape(smb_status));
    replaceToken(html, "{{MUSIC_BROWSER}}", kMusicBrowserSections);
    return html;
#endif
}

std::string WifiConfigPortalView_BuildAprsSections(void)
{
    std::string html = std::string(kWifiConfigPortalAprsSectionsTemplate);
    AprsConfig cfg;
    APRS_SERVICE_GetConfig(&cfg);

    char status[96];
    snprintf(status, sizeof(status), "%s | APRS-IS %s | GPS %s",
             cfg.enabled ? "ON" : "OFF",
             APRS_SERVICE_IsNetConnected() ? "connected" : "--",
             APRS_SERVICE_GpsHasFix() ? "fix" : "--");
    replaceToken(html, "{{APRS_STATUS}}", status);
    replaceToken(html, "{{APRS_ENABLED_CHECKED}}", checkedAttr(cfg.enabled));
    replaceToken(html, "{{APRS_NET_CHECKED}}", checkedAttr(cfg.net_enabled));
    replaceToken(html, "{{APRS_TX_CHECKED}}", checkedAttr(cfg.rf_tx_enabled));
    replaceToken(html, "{{APRS_RX_CHECKED}}", checkedAttr(cfg.rf_rx_enabled));
    replaceToken(html, "{{APRS_AUTO_CHECKED}}", checkedAttr(cfg.auto_interval));
    replaceToken(html, "{{APRS_FIXED_CHECKED}}", checkedAttr(cfg.fixed_beacon_without_gps));
    replaceToken(html, "{{APRS_SERVER_HOST}}", htmlEscape(cfg.server_host));
    replaceToken(html, "{{APRS_SERVER_PORT}}", fromU32(cfg.server_port));
    replaceToken(html, "{{APRS_SSID}}", fromU32(cfg.ssid));
    {
        char symbol[3] = {cfg.symbol_table, cfg.symbol_code, '\0'};
        replaceToken(html, "{{APRS_SYMBOL}}", htmlEscape(symbol));
    }
    replaceToken(html, "{{APRS_INTERVAL}}", fromU32(cfg.beacon_interval_s));
    {
        char lat[24], lon[24];
        APRS_SERVICE_FormatAprsCoord(static_cast<double>(cfg.default_lat_e6) / 1e6,
                                     true, lat, sizeof(lat));
        APRS_SERVICE_FormatAprsCoord(static_cast<double>(cfg.default_lon_e6) / 1e6,
                                     false, lon, sizeof(lon));
        replaceToken(html, "{{APRS_LAT}}", lat);
        replaceToken(html, "{{APRS_LON}}", lon);
    }
    replaceToken(html, "{{APRS_PATH}}", htmlEscape(cfg.path));
    replaceToken(html, "{{APRS_COMMENT}}", htmlEscape(cfg.comment));
    return html;
}

std::string WifiConfigPortalView_BuildSignalingSections(void)
{
    std::string html = std::string(kWifiConfigPortalSignalingSectionsTemplate);
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);
    replaceToken(html, "{{SIGNALING_STATUS}}", "16 kHz PCM / PSRAM");
    replaceToken(html, "{{CTCSS_RX_MIC_CHECKED}}", checkedAttr(cfg.ctcss_rx_mic));
    replaceToken(html, "{{CTCSS_RX_NRL_CHECKED}}", checkedAttr(cfg.ctcss_rx_nrl));
    replaceToken(html, "{{MDC_RX_MIC_CHECKED}}", checkedAttr(cfg.mdc_rx_mic));
    replaceToken(html, "{{MDC_RX_NRL_CHECKED}}", checkedAttr(cfg.mdc_rx_nrl));
    replaceToken(html, "{{MDC_TX_NRL_CHECKED}}", checkedAttr(cfg.mdc_tx_nrl));
    replaceToken(html, "{{MDC_TX_SPEAKER_CHECKED}}", checkedAttr(cfg.mdc_tx_speaker));
    replaceToken(html, "{{CW_RX_MIC_CHECKED}}", checkedAttr(cfg.cw_rx_mic));
    replaceToken(html, "{{CW_RX_NRL_CHECKED}}", checkedAttr(cfg.cw_rx_nrl));
    replaceToken(html, "{{DTMF_RX_MIC_CHECKED}}", checkedAttr(cfg.dtmf_rx_mic));
    replaceToken(html, "{{DTMF_RX_NRL_CHECKED}}", checkedAttr(cfg.dtmf_rx_nrl));
    replaceToken(html, "{{DTMF_TX_NRL_CHECKED}}", checkedAttr(cfg.dtmf_tx_nrl));
    replaceToken(html, "{{DTMF_TX_SPEAKER_CHECKED}}", checkedAttr(cfg.dtmf_tx_speaker));
    char value[8];
    snprintf(value, sizeof(value), "%02X", cfg.mdc_opcode);
    replaceToken(html, "{{MDC_OPCODE}}", value);
    snprintf(value, sizeof(value), "%02X", cfg.mdc_argument);
    replaceToken(html, "{{MDC_ARGUMENT}}", value);
    snprintf(value, sizeof(value), "%04X", cfg.mdc_unit_id);
    replaceToken(html, "{{MDC_UNIT_ID}}", value);
    replaceToken(html, "{{DTMF_DIGITS}}", htmlEscape(cfg.dtmf_digits));
    return html;
}

std::string WifiConfigPortalView_BuildConfigPage(const ExternalRadioConfig *config,
                                                 const WifiConfigPortalPageState &state,
                                                 const std::string &form_sections)
{
    std::string html = buildPageWithFormSections(form_sections);
    replaceToken(html, "{{TITLE}}", state.title);
    replaceToken(html, "{{HEADLINE}}", state.headline);
    replaceToken(html, "{{HEADLINE_KEY}}", state.headline_key);
    replaceToken(html, "{{INTRO}}", state.intro);
    replaceToken(html, "{{INTRO_KEY}}", state.intro_key);
    replaceToken(html, "{{NETWORK_ACTIVE}}", state.network_active ? "active" : "");
    replaceToken(html, "{{DEVICE_ACTIVE}}", state.device_active ? "active" : "");
    replaceToken(html, "{{AUDIO_ACTIVE}}", state.audio_active ? "active" : "");
    {
        std::string media_tab = std::string(kWifiConfigPortalMediaTabTemplate);
        replaceToken(media_tab, "{{MEDIA_ACTIVE}}", state.media_active ? "active" : "");
        replaceToken(html, "{{MEDIA_TAB}}", media_tab);
    }
    {
        std::string aprs_tab = std::string(kWifiConfigPortalAprsTabTemplate);
        replaceToken(aprs_tab, "{{APRS_ACTIVE}}", state.aprs_active ? "active" : "");
        replaceToken(html, "{{APRS_TAB}}", aprs_tab);
    }
    {
        std::string signaling_tab = std::string(kWifiConfigPortalSignalingTabTemplate);
        replaceToken(signaling_tab, "{{SIGNALING_ACTIVE}}", state.signaling_active ? "active" : "");
        replaceToken(html, "{{SIGNALING_TAB}}", signaling_tab);
    }
    replaceToken(html, "{{AP_IP}}", ipToString(nrlWifiApIp()));
    replaceToken(html, "{{STA_IP}}", staIpOrNotConnected(nrlNetworkIp()));
    replaceToken(html, "{{SSID_OPTIONS}}", "");
    replaceToken(html, "{{WIFI_SSID}}", htmlEscape(config->wifi_ssid));
    replaceToken(html, "{{WIFI_PASSWORD}}", htmlEscape(config->wifi_password));
    replaceToken(html, "{{SERVER_HOST}}", htmlEscape(config->server_host));
    replaceToken(html, "{{SERVER_PORT}}", fromU32(config->server_port));
    replaceToken(html, "{{CHANNEL}}", fromU32(config->channel));
    replaceToken(html, "{{CALLSIGN}}", htmlEscape(config->callsign));
    replaceToken(html, "{{CALLSIGN_SSID}}", fromU32(config->callsign_ssid));
    replaceToken(html, "{{MIC_VOLUME}}", fromU32(config->mic_volume));
    replaceToken(html, "{{LINE_OUT_VOLUME}}", fromU32(config->line_out_volume));
    replaceToken(html, "{{HP_DRIVE_CHECKED}}", config->hp_drive_enabled ? "checked" : "");
    replaceToken(html, "{{FORM_ACTION}}", state.form_action);
    replaceToken(html, "{{FOOTER}}", state.footer);
    replaceToken(html, "{{VERSION}}", NRL_FIRMWARE_VERSION);
    return html;
}

std::string WifiConfigPortalView_BuildUpdatePage(const char *headline,
                                                 const char *headline_key,
                                                 const char *intro,
                                                 const char *intro_key)
{
    std::string html = std::string(kWifiUpdatePortalHtmlTemplate);
    replaceToken(html, "{{HEADLINE}}", headline);
    replaceToken(html, "{{HEADLINE_KEY}}", headline_key);
    replaceToken(html, "{{INTRO}}", intro);
    replaceToken(html, "{{INTRO_KEY}}", intro_key);
    replaceToken(html, "{{AP_IP}}", ipToString(nrlWifiApIp()));
    replaceToken(html, "{{STA_IP}}", staIpOrNotConnected(nrlNetworkIp()));
    replaceToken(html, "{{VERSION}}", NRL_FIRMWARE_VERSION);
    return html;
}
