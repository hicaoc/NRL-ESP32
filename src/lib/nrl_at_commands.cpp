#include "nrl_at_commands.h"

#include "nrl_audio_bridge.h"
#include "nrl_net_compat.h"
#include "nrl_version.h"

#include "driver/board_pins.h"
#include "driver/display.h"
#include "driver/es8311.h"
#include "driver/es8389.h"
#include "driver/external_radio.h"
#include "driver/gps_serial.h"
#include "driver/serial_port_config.h"
#include "services/ai_assistant.h"
#include "services/aprs_service.h"
#include "services/display_notice.h"
#include "services/espnow_link.h"
#include "services/video_call.h"
#include "services/music_player.h"
#include "services/music_playlist.h"
#include "services/radio_favorites.h"
#include "services/nanny.h"
#include "services/ota_service.h"
#include "services/storage_service.h"
#include "services/signaling_service.h"
#include "services/cw_service.h"
#include "services/sstv_service.h"
#include "driver/sci_serial.h"
#include "app/main_loop_profile.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "NRL";

namespace {

// value is sized for AT+RADIOADD=<name>,<url> (48-byte name + 200-byte URL).
struct AtCommand {
    char command[32];
    char value[256];
};

bool replyContainsError(const NrlAtCommandResult *result)
{
    if (result == nullptr) return true;
    static constexpr char kErrorMarker[] = "AT+ERR=";
    constexpr size_t marker_size = sizeof(kErrorMarker) - 1u;
    for (size_t i = 0; i + marker_size <= result->payload_size; ++i) {
        if (memcmp(result->payload + i, kErrorMarker, marker_size) == 0) return true;
    }
    return false;
}

class SerialAtDisplayNotice {
public:
    SerialAtDisplayNotice(NrlAtCommandSource source, bool query, NrlAtCommandResult *result)
        : enabled_(source == NRL_AT_SOURCE_SERIAL && !query), result_(result) {}

    ~SerialAtDisplayNotice()
    {
        if (!enabled_) return;
        const bool ok = result_ != nullptr && result_->should_reply && !replyContainsError(result_);
        DISPLAY_NOTICE_Post(ok ? "AT COMMAND APPLIED" : "AT COMMAND FAILED",
                            ok ? DISPLAY_NOTICE_SUCCESS : DISPLAY_NOTICE_ERROR,
                            4000u);
    }

private:
    bool enabled_;
    NrlAtCommandResult *result_;
};

static bool stringEqualsIgnoreCase(const char *lhs, const char *rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }

    while (*lhs != '\0' && *rhs != '\0') {
        if (toupper(static_cast<unsigned char>(*lhs)) != toupper(static_cast<unsigned char>(*rhs))) {
            return false;
        }
        ++lhs;
        ++rhs;
    }

    return *lhs == '\0' && *rhs == '\0';
}

static void trimText(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t start = 0u;
    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n') {
        ++start;
    }
    if (start > 0u) {
        memmove(text, text + start, strlen(text + start) + 1u);
    }

    size_t len = strlen(text);
    while (len > 0u) {
        const char ch = text[len - 1u];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        text[--len] = '\0';
    }
}

static bool appendReplyBytes(uint8_t *payload,
                             const size_t capacity,
                             size_t *used,
                             const void *src,
                             const size_t src_size)
{
    if (payload == nullptr || used == nullptr || src == nullptr || *used + src_size > capacity) {
        return false;
    }

    memcpy(payload + *used, src, src_size);
    *used += src_size;
    return true;
}

static bool appendReplyLine(uint8_t *payload,
                            const size_t capacity,
                            size_t *used,
                            const char *line)
{
    static const char kCrLf[] = "\r\n";
    return appendReplyBytes(payload, capacity, used, line, strlen(line)) &&
           appendReplyBytes(payload, capacity, used, kCrLf, 2u);
}

static bool appendKeyValueLine(uint8_t *payload,
                               const size_t capacity,
                               size_t *used,
                               const char *key,
                               const char *value)
{
    static const char kPrefix[] = "AT+";
    static const char kEquals[] = "=";
    static const char kCrLf[] = "\r\n";
    if (key == nullptr || value == nullptr) return false;
    return appendReplyBytes(payload, capacity, used, kPrefix, sizeof(kPrefix) - 1u) &&
           appendReplyBytes(payload, capacity, used, key, strlen(key)) &&
           appendReplyBytes(payload, capacity, used, kEquals, sizeof(kEquals) - 1u) &&
           appendReplyBytes(payload, capacity, used, value, strlen(value)) &&
           appendReplyBytes(payload, capacity, used, kCrLf, sizeof(kCrLf) - 1u);
}

static bool appendKeyValueLineIfFits(uint8_t *payload,
                                     const size_t capacity,
                                     size_t *used,
                                     const char *key,
                                     const char *value)
{
    if (key == nullptr || value == nullptr || used == nullptr) return false;
    const size_t need = 3u + strlen(key) + 1u + strlen(value) + 2u; // AT+ key = value CRLF
    if (*used + need > capacity) return false;
    return appendKeyValueLine(payload, capacity, used, key, value);
}

static bool appendUnsignedLine(uint8_t *payload,
                               const size_t capacity,
                               size_t *used,
                               const char *key,
                               const unsigned value)
{
    char line[96];
    snprintf(line, sizeof(line), "AT+%s=%u", key, value);
    return appendReplyLine(payload, capacity, used, line);
}

static void formatMicPcmGain(const uint16_t gain_milli, char *out, const size_t out_size)
{
    if (out == nullptr || out_size == 0u) return;
    snprintf(out, out_size, "%u.%03u",
             static_cast<unsigned>(gain_milli / 1000u),
             static_cast<unsigned>(gain_milli % 1000u));
    size_t len = strlen(out);
    while (len > 2u && out[len - 1u] == '0' && out[len - 2u] != '.') {
        out[--len] = '\0';
    }
}

static bool parseMicPcmGain(const char *text, uint16_t *out_milli)
{
    if (text == nullptr || out_milli == nullptr || text[0] == '\0') return false;
    char *end = nullptr;
    const float gain = strtof(text, &end);
    if (end == text || end == nullptr || *end != '\0' || !isfinite(gain) ||
        gain < 0.1f || gain > 5.0f) {
        return false;
    }
    const long milli = lroundf(gain * 1000.0f);
    if (milli < 100l || milli > 5000l) return false;
    *out_milli = static_cast<uint16_t>(milli);
    return true;
}

static const char *ipText(const uint32_t value, char *buffer, const size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return "";
    }
    nrlIpToString(value, buffer, buffer_size);
    return buffer;
}

static uint32_t currentWifiIpValue(const uint32_t configured_value,
                                   const uint32_t dhcp_value,
                                   const bool use_dhcp_value)
{
    if (use_dhcp_value) {
        return dhcp_value;
    }
    return configured_value;
}

static void formatSciConfig(const SciSerialConfig &sci, char *buffer, const size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return;
    }
    snprintf(buffer,
             buffer_size,
             "%lu,%u,%c,%u",
             static_cast<unsigned long>(sci.baud),
             static_cast<unsigned>(sci.data_bits),
             sci.parity,
             static_cast<unsigned>(sci.stop_bits));
}

// Format a secret (e.g. WiFi password) for AT replies: never echo plaintext,
// only report its length.
static void formatMaskedSecret(const char *text, char *buffer, const size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0u) {
        return;
    }
    const size_t len = (text != nullptr) ? strlen(text) : 0u;
    if (len == 0u) {
        snprintf(buffer, buffer_size, "(empty)");
    } else {
        snprintf(buffer, buffer_size, "****** (%u chars)", static_cast<unsigned>(len));
    }
}

static bool appendSupportedAtList(uint8_t *payload,
                                  const size_t capacity,
                                  size_t *used)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    char sci_config[32] = "9600,8,N,1";
    char uart1_io[24] = "0,0";
    char uart2_io[24] = "0,0";
    char uart2_config[32] = "9600,8,N,1";
    char hp_drive[4] = "OFF";
    char ptt_mode[8] = "NRL";
    char wifi_ip[16] = "0.0.0.0";
    char mic_pcm_gain[16] = "1.0";
    char aprs_status[8] = "OFF";
    char aprs_net[8] = "OFF";
    char aprs_tx[8] = "OFF";
    char aprs_rx[8] = "OFF";
    char aprs_auto[8] = "OFF";
    char aprs_fixed[8] = "OFF";
    char aprs_server[88] = "";
    char aprs_ssid[8] = "0";
    char aprs_symbol[4] = "/I";
    char aprs_interval[8] = "60";
    char aprs_pos[40] = "";
    char mdc_packet[16] = "";
    if (config != nullptr) {
        formatSciConfig(config->sci, sci_config, sizeof(sci_config));
        snprintf(hp_drive, sizeof(hp_drive), "%s", config->hp_drive_enabled ? "ON" : "OFF");
        snprintf(ptt_mode, sizeof(ptt_mode), "%s",
                 (ESPNOW_LINK_GetPttMode() == 1u) ? "ESPNOW" : "NRL");
        const bool show_dhcp_values = config->wifi_dhcp_enabled && nrlWifiStaConnected();
        ipText(currentWifiIpValue(config->wifi_ip, nrlWifiStaIp(), show_dhcp_values), wifi_ip, sizeof(wifi_ip));
        formatMicPcmGain(config->mic_pcm_gain_milli, mic_pcm_gain, sizeof(mic_pcm_gain));
    }
    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    snprintf(uart1_io, sizeof(uart1_io), "%d,%d", serial.uart1_rx_pin, serial.uart1_tx_pin);
    snprintf(uart2_io, sizeof(uart2_io), "%d,%d", serial.uart2_rx_pin, serial.uart2_tx_pin);
    snprintf(uart2_config, sizeof(uart2_config), "%lu,%u,%c,%u",
             static_cast<unsigned long>(serial.uart2_baud),
             static_cast<unsigned>(serial.uart2_data_bits), serial.uart2_parity,
             static_cast<unsigned>(serial.uart2_stop_bits));
    AprsConfig aprs{};
    APRS_SERVICE_GetConfig(&aprs);
    snprintf(aprs_status, sizeof(aprs_status), "%s", aprs.enabled ? "ON" : "OFF");
    snprintf(aprs_net, sizeof(aprs_net), "%s", aprs.net_enabled ? "ON" : "OFF");
    snprintf(aprs_tx, sizeof(aprs_tx), "%s", aprs.rf_tx_enabled ? "ON" : "OFF");
    snprintf(aprs_rx, sizeof(aprs_rx), "%s", aprs.rf_rx_enabled ? "ON" : "OFF");
    snprintf(aprs_auto, sizeof(aprs_auto), "%s", aprs.auto_interval ? "ON" : "OFF");
    snprintf(aprs_fixed, sizeof(aprs_fixed), "%s",
             aprs.fixed_beacon_without_gps ? "ON" : "OFF");
    snprintf(aprs_server, sizeof(aprs_server), "%s:%u",
             aprs.server_host, static_cast<unsigned>(aprs.server_port));
    snprintf(aprs_ssid, sizeof(aprs_ssid), "%u", static_cast<unsigned>(aprs.ssid));
    snprintf(aprs_symbol, sizeof(aprs_symbol), "%c%c", aprs.symbol_table, aprs.symbol_code);
    snprintf(aprs_interval, sizeof(aprs_interval), "%u",
             static_cast<unsigned>(aprs.beacon_interval_s));
    {
        char lat[16] = {};
        char lon[16] = {};
        APRS_SERVICE_FormatAprsCoord(static_cast<double>(aprs.default_lat_e6) / 1e6,
                                     true, lat, sizeof(lat));
        APRS_SERVICE_FormatAprsCoord(static_cast<double>(aprs.default_lon_e6) / 1e6,
                                     false, lon, sizeof(lon));
        snprintf(aprs_pos, sizeof(aprs_pos), "%s,%s", lat, lon);
    }
    SignalingConfig sig{};
    SIGNALING_GetConfig(&sig);
    snprintf(mdc_packet, sizeof(mdc_packet), "%02X,%02X,%04X",
             static_cast<unsigned>(sig.mdc_opcode),
             static_cast<unsigned>(sig.mdc_argument),
             static_cast<unsigned>(sig.mdc_unit_id));

    // Keep the on-wire structure unchanged: every entry remains AT+NAME=value.
    // The NRL AT packet is capped at 1024 bytes and the server does not support
    // fragmented replies yet, so long text fields such as APRS_COMMENT are not
    // included here. They remain queryable with their own AT+NAME=? command.
    const bool ok =
        appendKeyValueLineIfFits(payload, capacity, used, "WIFI_SSID", (config != nullptr) ? config->wifi_ssid : "") &&
        appendKeyValueLineIfFits(payload, capacity, used, "WIFI_DHCP", (config != nullptr && config->wifi_dhcp_enabled) ? "1" : "0") &&
        appendKeyValueLineIfFits(payload, capacity, used, "WIFI_IP", wifi_ip) &&
        appendKeyValueLineIfFits(payload, capacity, used, "SCI", sci_config) &&
        appendKeyValueLineIfFits(payload, capacity, used, "UART0", "RESERVED(LOG/AT)") &&
        appendKeyValueLineIfFits(payload, capacity, used, "UART1", sci_config) &&
        appendKeyValueLineIfFits(payload, capacity, used, "UART1_ENABLE", serial.uart1_enabled ? "ON" : "OFF") &&
        appendKeyValueLineIfFits(payload, capacity, used, "UART1_IO", uart1_io) &&
        appendKeyValueLineIfFits(payload, capacity, used, "UART2", uart2_config) &&
        appendKeyValueLineIfFits(payload, capacity, used, "UART2_ENABLE", serial.uart2_enabled ? "ON" : "OFF") &&
        appendKeyValueLineIfFits(payload, capacity, used, "UART2_IO", uart2_io);
    if (!ok) return false;

    char number[16];
    snprintf(number, sizeof(number), "%u", (config != nullptr) ? config->channel : 0u);
    appendKeyValueLineIfFits(payload, capacity, used, "CH", number);
    appendKeyValueLineIfFits(payload, capacity, used, "D_IP", (config != nullptr) ? config->server_host : "");
    snprintf(number, sizeof(number), "%u", (config != nullptr) ? config->server_port : 0u);
    appendKeyValueLineIfFits(payload, capacity, used, "D_PORT", number);
    snprintf(number, sizeof(number), "%u", (config != nullptr) ? config->local_port : 0u);
    appendKeyValueLineIfFits(payload, capacity, used, "L_PORT", number);
    appendKeyValueLineIfFits(payload, capacity, used, "CALL", (config != nullptr) ? config->callsign : "");
    snprintf(number, sizeof(number), "%u", (config != nullptr) ? config->callsign_ssid : 0u);
    appendKeyValueLineIfFits(payload, capacity, used, "SSID", number);
    snprintf(number, sizeof(number), "%u", (config != nullptr) ? config->mic_volume : 0u);
    appendKeyValueLineIfFits(payload, capacity, used, "MIC_GAIN", number);
    appendKeyValueLineIfFits(payload, capacity, used, "MIC_PCM_GAIN", mic_pcm_gain);
    snprintf(number, sizeof(number), "%u", (config != nullptr) ? config->line_out_volume : 0u);
    appendKeyValueLineIfFits(payload, capacity, used, "VOLUME", number);
    appendKeyValueLineIfFits(payload, capacity, used, "HP_DRIVE", hp_drive);
    appendKeyValueLineIfFits(payload, capacity, used, "CODEC",
                             (NRLAudioBridge_GetVoiceCodec() == 1u) ? "OPUS" : "G711");
    appendKeyValueLineIfFits(payload, capacity, used, "PTT_MODE", ptt_mode);
    appendKeyValueLineIfFits(payload, capacity, used, "CTCSS_RX_MIC", sig.ctcss_rx_mic ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "CTCSS_RX_NRL", sig.ctcss_rx_nrl ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "MDC", mdc_packet);
    appendKeyValueLineIfFits(payload, capacity, used, "MDC_RX_MIC", sig.mdc_rx_mic ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "MDC_RX_NRL", sig.mdc_rx_nrl ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "MDC_TX_NRL", sig.mdc_tx_nrl ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "MDC_TX_SPK", sig.mdc_tx_speaker ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "CW_RX_MIC", sig.cw_rx_mic ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "CW_RX_NRL", sig.cw_rx_nrl ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "DTMF", sig.dtmf_digits);
    appendKeyValueLineIfFits(payload, capacity, used, "DTMF_RX_MIC", sig.dtmf_rx_mic ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "DTMF_RX_NRL", sig.dtmf_rx_nrl ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "DTMF_TX_NRL", sig.dtmf_tx_nrl ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "DTMF_TX_SPK", sig.dtmf_tx_speaker ? "ON" : "OFF");
    appendKeyValueLineIfFits(payload, capacity, used, "APRS", aprs_status);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_NET", aprs_net);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_TX", aprs_tx);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_RX", aprs_rx);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_AUTO", aprs_auto);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_FIXED", aprs_fixed);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_SERVER", aprs_server);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_SSID", aprs_ssid);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_SYMBOL", aprs_symbol);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_INTERVAL", aprs_interval);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_POS", aprs_pos);
    appendKeyValueLineIfFits(payload, capacity, used, "APRS_PATH", aprs.path);
    appendKeyValueLineIfFits(payload, capacity, used, "READ", "123");
    appendKeyValueLineIfFits(payload, capacity, used, "REBOOT", "1");
    return true;
}

static bool decodeAtCommand(const uint8_t *payload, const size_t payload_size, AtCommand *out_cmd)
{
    if (payload == nullptr || out_cmd == nullptr || payload_size < 3u || payload[0] != 0x01u) {
        return false;
    }

    const uint8_t *text = payload + 1u;
    const size_t text_size = payload_size - 1u;
    const auto *separator = static_cast<const uint8_t *>(memchr(text, '=', text_size));
    if (separator == nullptr) return false;

    const size_t command_size = static_cast<size_t>(separator - text);
    const size_t value_size = text_size - command_size - 1u;
    const size_t command_copy = command_size < sizeof(out_cmd->command) - 1u
        ? command_size : sizeof(out_cmd->command) - 1u;
    const size_t value_copy = value_size < sizeof(out_cmd->value) - 1u
        ? value_size : sizeof(out_cmd->value) - 1u;
    memcpy(out_cmd->command, text, command_copy);
    out_cmd->command[command_copy] = '\0';
    memcpy(out_cmd->value, separator + 1u, value_copy);
    out_cmd->value[value_copy] = '\0';

    trimText(out_cmd->command);
    trimText(out_cmd->value);
    // Some serial terminals can leave a previously typed "AT" in front of a
    // pasted command ("ATAT+TOP=1"). Collapse repeated leading AT prefixes
    // before stripping the normal AT+ prefix.
    while (strncasecmp(out_cmd->command, "ATAT+", 5u) == 0) {
        memmove(out_cmd->command, out_cmd->command + 2u,
                strlen(out_cmd->command + 2u) + 1u);
    }
    if (strncasecmp(out_cmd->command, "AT+", 3u) == 0) {
        memmove(out_cmd->command, out_cmd->command + 3u, strlen(out_cmd->command + 3u) + 1u);
    }
    return out_cmd->command[0] != '\0';
}

static bool parseUnsignedValue(const char *text, unsigned long *out_value)
{
    if (text == nullptr || out_value == nullptr || text[0] == '\0') {
        return false;
    }

    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || (end != nullptr && *end != '\0')) {
        return false;
    }

    *out_value = value;
    return true;
}

static bool parseHexValue(const char *text, unsigned long *out_value)
{
    if (text == nullptr || out_value == nullptr || text[0] == '\0') return false;
    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 16);
    if (end == text || (end != nullptr && *end != '\0')) return false;
    *out_value = value;
    return true;
}

static bool parseBoolValue(const char *text, bool *out_value)
{
    if (text == nullptr || out_value == nullptr) {
        return false;
    }
    if (stringEqualsIgnoreCase(text, "1") ||
        stringEqualsIgnoreCase(text, "ON") ||
        stringEqualsIgnoreCase(text, "TRUE") ||
        stringEqualsIgnoreCase(text, "ENABLE")) {
        *out_value = true;
        return true;
    }
    if (stringEqualsIgnoreCase(text, "0") ||
        stringEqualsIgnoreCase(text, "OFF") ||
        stringEqualsIgnoreCase(text, "FALSE") ||
        stringEqualsIgnoreCase(text, "DISABLE")) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool parseIpValue(const char *text, uint32_t *out_value)
{
    if (text == nullptr || out_value == nullptr) {
        return false;
    }
    struct in_addr addr = {};
    if (inet_aton(text, &addr) == 0) {
        return false;
    }
    *out_value = addr.s_addr;
    return true;
}

static bool parseSciConfigValue(const char *text,
                                uint32_t *out_baud,
                                uint8_t *out_data_bits,
                                char *out_parity,
                                uint8_t *out_stop_bits)
{
    if (text == nullptr || out_baud == nullptr || out_data_bits == nullptr ||
        out_parity == nullptr || out_stop_bits == nullptr) {
        return false;
    }

    char buffer[80];
    snprintf(buffer, sizeof(buffer), "%.*s", static_cast<int>(sizeof(buffer) - 1u), text);

    char *parts[4] = {};
    char *cursor = buffer;
    for (size_t i = 0; i < 4u; ++i) {
        parts[i] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == nullptr) {
            if (i != 3u) {
                return false;
            }
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    if (strchr(parts[3], ',') != nullptr) {
        return false;
    }

    for (size_t i = 0; i < 4u; ++i) {
        trimText(parts[i]);
        if (parts[i][0] == '\0') {
            return false;
        }
    }

    unsigned long baud = 0u;
    unsigned long data_bits = 0u;
    unsigned long stop_bits = 0u;
    if (!parseUnsignedValue(parts[0], &baud) ||
        !parseUnsignedValue(parts[1], &data_bits) ||
        !parseUnsignedValue(parts[3], &stop_bits)) {
        return false;
    }

    const char parity = static_cast<char>(toupper(static_cast<unsigned char>(parts[2][0])));
    if (parts[2][1] != '\0' ||
        baud < 300u || baud > 921600u ||
        data_bits < 5u || data_bits > 8u ||
        (parity != 'N' && parity != 'E' && parity != 'O') ||
        (stop_bits != 1u && stop_bits != 2u)) {
        return false;
    }

    *out_baud = static_cast<uint32_t>(baud);
    *out_data_bits = static_cast<uint8_t>(data_bits);
    *out_parity = parity;
    *out_stop_bits = static_cast<uint8_t>(stop_bits);
    return true;
}

static bool parseIoConfigValue(const char *text, int *out_rx, int *out_tx)
{
    if (text == nullptr || out_rx == nullptr || out_tx == nullptr) return false;
    char buffer[48];
    const size_t text_len = strlen(text);
    if (text_len == 0u || text_len >= sizeof(buffer)) return false;
    memcpy(buffer, text, text_len + 1u);
    char *comma = strchr(buffer, ',');
    if (comma == nullptr || strchr(comma + 1, ',') != nullptr) return false;
    *comma = '\0';
    char *rx_text = buffer;
    char *tx_text = comma + 1;
    trimText(rx_text);
    trimText(tx_text);
    char *rx_end = nullptr;
    char *tx_end = nullptr;
    const long rx = strtol(rx_text, &rx_end, 10);
    const long tx = strtol(tx_text, &tx_end, 10);
    if (rx_end == rx_text || tx_end == tx_text ||
        rx_end == nullptr || tx_end == nullptr ||
        *rx_end != '\0' || *tx_end != '\0' ||
        rx < -1L || rx > 127L || tx < -1L || tx > 127L) {
        return false;
    }
    *out_rx = static_cast<int>(rx);
    *out_tx = static_cast<int>(tx);
    return true;
}

static bool startAtReply(NrlAtCommandResult *result)
{
    static const uint8_t kPrefix = 0x02u;
    static const char kBanner[] = NRL_FIRMWARE_BANNER "\r\n";

    result->payload_size = 0u;
    return appendReplyBytes(result->payload, sizeof(result->payload), &result->payload_size, &kPrefix, 1u) &&
           appendReplyBytes(result->payload, sizeof(result->payload), &result->payload_size, kBanner, sizeof(kBanner) - 1u);
}

static bool applyCurrentAudioConfig(void)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config == nullptr) {
        return false;
    }
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    return ES8311_ApplyAudioConfig(config->mic_volume,
                                   config->line_out_volume,
                                   config->hp_drive_enabled,
                                   config->drc_enabled,
                                   config->drc_winsize,
                                   config->drc_maxlevel,
                                   config->drc_minlevel,
                                   config->dac_ramprate,
                                   config->dac_eq_bypass,
                                   config->daceq_b0,
                                   config->daceq_b1,
                                   config->daceq_a1,
                                   config->adc_dmic_enabled,
                                   config->adc_linsel,
                                   config->adc_pga_gain,
                                   config->adc_ramprate,
                                   config->adc_dmic_sense,
                                   config->adc_sync,
                                   config->adc_inv,
                                   config->adc_ramclr,
                                   config->adc_scale,
                                   config->alc_enabled,
                                   config->adc_automute_enabled,
                                   config->alc_winsize,
                                   config->alc_maxlevel,
                                   config->alc_minlevel,
                                   config->adc_automute_winsize,
                                   config->adc_automute_noise_gate,
                                   config->adc_automute_volume,
                                   config->adc_hpfs1,
                                   config->adc_eq_bypass,
                                   config->adc_hpf,
                                   config->adc_hpfs2,
                                   config->adceq_b0,
                                   config->adceq_a1,
                                   config->adceq_a2,
                                   config->adceq_b1,
                                   config->adceq_b2);
#elif defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    return ES8389_SetOutputVolume(config->line_out_volume) &&
           ES8389_SetInputGain(config->mic_volume);
#else
    return true;
#endif
}

} // namespace

void NRL_AT_HandlePayload(const uint8_t *payload,
                          const size_t payload_size,
                          const NrlAtCommandSource source,
                          NrlAtCommandResult *result)
{
    if (result == nullptr) {
        return;
    }
    *result = NrlAtCommandResult{};

    AtCommand command = {};
    if (!startAtReply(result)) {
        return;
    }

    result->should_reply = true;
    if (!decodeAtCommand(payload, payload_size, &command)) {
        if (!appendSupportedAtList(result->payload, sizeof(result->payload), &result->payload_size)) {
            result->should_reply = false;
        }
        return;
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const bool is_query = stringEqualsIgnoreCase(command.value, "?");
    const bool allow_wifi_config_write = source == NRL_AT_SOURCE_SERIAL;
    const bool allow_ota_write = source == NRL_AT_SOURCE_SERIAL;

    if (stringEqualsIgnoreCase(command.command, "READ") &&
        stringEqualsIgnoreCase(command.value, "123")) {
        if (!appendSupportedAtList(result->payload, sizeof(result->payload), &result->payload_size)) {
            result->should_reply = false;
        }
        return;
    }

    SerialAtDisplayNotice display_notice(source, is_query, result);

    // Remote OTA commands are serial-only: an NRL network AT packet must not
    // be able to replace the server/token or reboot a radio by installing a
    // new image. The board's local Wi-Fi portal provides the browser path.
    if (stringEqualsIgnoreCase(command.command, "OTAURL")) {
        if (is_query) {
            NrlOtaStatus status = {};
            OtaService_GetStatus(&status);
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "OTAURL", status.configured ? status.server_url : "OFF");
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "OTALATEST", status.latest_version[0] != '\0' ? status.latest_version : "-");
            return;
        }
        if (!allow_ota_write) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "OTAURL");
            return;
        }
        char value[sizeof(command.value)] = {};
        snprintf(value, sizeof(value), "%s", command.value);
        char *token = strchr(value, ',');
        if (token != nullptr) {
            *token++ = '\0';
        }
        const bool clear = stringEqualsIgnoreCase(value, "OFF");
        if ((!clear && value[0] == '\0') ||
            !OtaService_SetConfig(clear ? "" : value, clear ? "" : (token != nullptr ? token : ""))) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "OTAURL");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           "OTAURL", clear ? "OFF" : value);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "OTACHECK")) {
        if (is_query) {
            NrlOtaStatus status = {};
            OtaService_GetStatus(&status);
            const char *state = status.updating ? "UPDATING" : status.checking ? "CHECKING" :
                                status.last_error[0] != '\0' ? status.last_error : "IDLE";
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "OTACHECK", state);
            return;
        }
        if (!allow_ota_write || !OtaService_CheckNow()) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "OTACHECK");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "OTACHECK", "QUEUED");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "OTALIST")) {
        if (!is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "OTALIST");
            return;
        }
        NrlOtaStatus status = {};
        OtaService_GetStatus(&status);
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size,
                           "OTACNT", static_cast<unsigned>(status.release_count));
        for (size_t i = 0; i < status.release_count; ++i) {
            char key[16] = {};
            snprintf(key, sizeof(key), "OTA%u", static_cast<unsigned>(i));
            if (!appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                                    key, status.releases[i].version)) {
                break;
            }
        }
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "OTA")) {
        if (is_query) {
            NrlOtaStatus status = {};
            OtaService_GetStatus(&status);
            const char *state = status.updating ? "UPDATING" : status.checking ? "CHECKING" :
                                status.latest_version[0] != '\0' ? status.latest_version : "NO_RELEASE";
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "OTA", state);
            return;
        }
        const bool ok = allow_ota_write &&
            (stringEqualsIgnoreCase(command.value, "LATEST")
                ? OtaService_CheckAndUpdateLatest()
                : OtaService_UpdateVersion(command.value));
        if (!ok) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "OTA");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           "OTA", stringEqualsIgnoreCase(command.value, "LATEST") ? "CHECKING" : "QUEUED");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "WIFI_SSID")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_SSID", config->wifi_ssid);
            return;
        }
        if (!allow_wifi_config_write ||
            !EXTERNAL_RADIO_SetWifiSsid(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "WIFI_SSID");
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_SSID", EXTERNAL_RADIO_GetConfig()->wifi_ssid);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "WIFI_PASS") ||
        stringEqualsIgnoreCase(command.command, "WIFI_PASSWORD")) {
        char masked[32];
        if (is_query) {
            formatMaskedSecret(config->wifi_password, masked, sizeof(masked));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_PASS", masked);
            return;
        }
        if (!allow_wifi_config_write ||
            !EXTERNAL_RADIO_SetWifiPassword(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "WIFI_PASS");
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        formatMaskedSecret(EXTERNAL_RADIO_GetConfig()->wifi_password, masked, sizeof(masked));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_PASS", masked);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "WIFI_DHCP")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_DHCP", config->wifi_dhcp_enabled ? 1u : 0u);
            return;
        }
        bool enabled = false;
        if (!allow_wifi_config_write ||
            !parseBoolValue(command.value, &enabled) ||
            !EXTERNAL_RADIO_SetWifiDhcpEnabled(enabled, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "WIFI_DHCP");
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "WIFI_DHCP", EXTERNAL_RADIO_GetConfig()->wifi_dhcp_enabled ? 1u : 0u);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "WIFI_IP") ||
        stringEqualsIgnoreCase(command.command, "WIFI_MASK") ||
        stringEqualsIgnoreCase(command.command, "WIFI_GW") ||
        stringEqualsIgnoreCase(command.command, "WIFI_DNS")) {
        char current[16];
        uint32_t configured = config->wifi_ip;
        uint32_t dhcp = nrlWifiStaIp();
        const char *key = "WIFI_IP";
        if (stringEqualsIgnoreCase(command.command, "WIFI_MASK")) {
            configured = config->wifi_netmask;
            dhcp = nrlWifiStaNetmask();
            key = "WIFI_MASK";
        } else if (stringEqualsIgnoreCase(command.command, "WIFI_GW")) {
            configured = config->wifi_gateway;
            dhcp = nrlWifiStaGateway();
            key = "WIFI_GW";
        } else if (stringEqualsIgnoreCase(command.command, "WIFI_DNS")) {
            configured = config->wifi_dns;
            dhcp = nrlWifiStaDns();
            key = "WIFI_DNS";
        }
        if (is_query) {
            const bool show_dhcp_value = config->wifi_dhcp_enabled && nrlWifiStaConnected();
            appendKeyValueLine(result->payload,
                               sizeof(result->payload),
                               &result->payload_size,
                               key,
                               ipText(currentWifiIpValue(configured, dhcp, show_dhcp_value), current, sizeof(current)));
            return;
        }
        uint32_t value = 0U;
        bool ok = allow_wifi_config_write && parseIpValue(command.value, &value);
        if (ok && strcmp(key, "WIFI_IP") == 0) {
            ok = EXTERNAL_RADIO_SetWifiIp(value, true);
        } else if (ok && strcmp(key, "WIFI_MASK") == 0) {
            ok = EXTERNAL_RADIO_SetWifiNetmask(value, true);
        } else if (ok && strcmp(key, "WIFI_GW") == 0) {
            ok = EXTERNAL_RADIO_SetWifiGateway(value, true);
        } else if (ok && strcmp(key, "WIFI_DNS") == 0) {
            ok = EXTERNAL_RADIO_SetWifiDns(value, true);
        }
        if (!ok) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", key);
            return;
        }
        result->restart_wifi = true;
        result->restart_udp = true;
        appendKeyValueLine(result->payload,
                           sizeof(result->payload),
                           &result->payload_size,
                           key,
                           ipText(value, current, sizeof(current)));
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "REBOOT")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "REBOOT", "1");
            return;
        }
        if (command.value[0] != '\0' && !stringEqualsIgnoreCase(command.value, "1") &&
            !stringEqualsIgnoreCase(command.value, "ON")) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "REBOOT");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "REBOOT", "OK");
        result->reboot = true;
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "CH")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "CH", config->channel);
            return;
        }

        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 7u ||
            !EXTERNAL_RADIO_SetChannel(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "CH");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "CH", EXTERNAL_RADIO_GetChannel());
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "D_IP")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "D_IP", config->server_host);
            return;
        }
        if (!EXTERNAL_RADIO_SetServerHost(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "D_IP");
            return;
        }
        result->restart_udp = true;
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "D_IP", EXTERNAL_RADIO_GetConfig()->server_host);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "D_PORT")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "D_PORT", config->server_port);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value == 0u || value > 65535u ||
            !EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "D_PORT");
            return;
        }
        result->restart_udp = true;
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "D_PORT", EXTERNAL_RADIO_GetConfig()->server_port);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "L_PORT") ||
        stringEqualsIgnoreCase(command.command, "LOCAL_PORT")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "L_PORT", config->local_port);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value == 0u || value > 65535u ||
            !EXTERNAL_RADIO_SetLocalPort(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "L_PORT");
            return;
        }
        result->restart_udp = true;
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "L_PORT", EXTERNAL_RADIO_GetConfig()->local_port);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "CALL")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CALL", config->callsign);
            return;
        }
        if (!EXTERNAL_RADIO_SetCallsign(command.value, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "CALL");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CALL", EXTERNAL_RADIO_GetConfig()->callsign);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "SSID")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "SSID", config->callsign_ssid);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 255u ||
            !EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "SSID");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "SSID", EXTERNAL_RADIO_GetConfig()->callsign_ssid);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "PTT_TIMEOUT")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "PTT_TIMEOUT", config->ptt_timeout_s);
            return;
        }
        unsigned long value = 0u;
        // 5..3600 s; the explicit bound also stops a >65535 value from
        // wrapping when narrowed to uint16_t inside the setter.
        if (!parseUnsignedValue(command.value, &value) || value < 5u || value > 3600u ||
            !EXTERNAL_RADIO_SetPttTimeout(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "PTT_TIMEOUT");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "PTT_TIMEOUT", EXTERNAL_RADIO_GetConfig()->ptt_timeout_s);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "VOICE_BYTES")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOICE_BYTES", config->voice_payload_bytes);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value < 160u || value > 500u ||
            !EXTERNAL_RADIO_SetVoicePayloadBytes(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "VOICE_BYTES");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOICE_BYTES", EXTERNAL_RADIO_GetConfig()->voice_payload_bytes);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "TAIL_SUPPRESS")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "TAIL_SUPPRESS", config->tail_suppress_ms);
            return;
        }
        unsigned long value = 0u;
        // 0..5000 ms; 0 disables. The explicit bound also stops a >65535 value
        // from wrapping when narrowed to uint16_t inside the setter.
        if (!parseUnsignedValue(command.value, &value) || value > 5000u ||
            !EXTERNAL_RADIO_SetTailSuppressMs(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "TAIL_SUPPRESS");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "TAIL_SUPPRESS", EXTERNAL_RADIO_GetConfig()->tail_suppress_ms);
        return;
    }

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY
    // Read-only: the current calibrated battery voltage in millivolts. Always
    // 0 if the ADC is not initialised (e.g. before Display_Init).
    if (stringEqualsIgnoreCase(command.command, "BATT")) {
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT",
                           static_cast<unsigned>(Display_GetBatteryCalibratedMv()));
        return;
    }

    // Read-only: the raw (pre-calibration) battery voltage in millivolts.
    // Useful for working out a fresh calibration factor by comparing against a
    // multimeter reading.
    if (stringEqualsIgnoreCase(command.command, "BATT_RAW")) {
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_RAW",
                           static_cast<unsigned>(Display_GetBatteryRawMv()));
        return;
    }

    // Battery calibration scale factor in units of 1/1000 (1000 = no
    // correction). Accepts 500..2000.
    if (stringEqualsIgnoreCase(command.command, "BATT_CAL")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_CAL", config->battery_cal_milli);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value < 500u || value > 2000u ||
            !EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_CAL", EXTERNAL_RADIO_GetConfig()->battery_cal_milli);
        return;
    }

    // Auto-calibrate from a multimeter reading: AT+BATT_CAL_AUTO=<actual_mv>.
    // Samples the raw voltage now, computes scale = actual_mv * 1000 / raw_mv,
    // clamps to the valid range, and persists it.
    if (stringEqualsIgnoreCase(command.command, "BATT_CAL_AUTO")) {
        unsigned long actual_mv = 0u;
        if (is_query || !parseUnsignedValue(command.value, &actual_mv) ||
            actual_mv < 1000u || actual_mv > 9000u) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL_AUTO");
            return;
        }
        const int raw_mv = Display_GetBatteryRawMv();
        if (raw_mv <= 0) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL_AUTO");
            return;
        }
        const unsigned long scale = (actual_mv * 1000ul + static_cast<unsigned long>(raw_mv) / 2ul) /
                                    static_cast<unsigned long>(raw_mv);
        if (scale < 500ul || scale > 2000ul ||
            !EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(scale), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BATT_CAL_AUTO");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "BATT_CAL", EXTERNAL_RADIO_GetConfig()->battery_cal_milli);
        return;
    }
#endif

    if (stringEqualsIgnoreCase(command.command, "MIC_GAIN")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "MIC_GAIN", config->mic_volume);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 255u ||
            !EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "MIC_GAIN");
            return;
        }
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        applyCurrentAudioConfig();
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "MIC_GAIN", updated->mic_volume);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "MIC_PCM_GAIN")) {
        if (is_query) {
            char gain[16];
            formatMicPcmGain(config->mic_pcm_gain_milli, gain, sizeof(gain));
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "MIC_PCM_GAIN", gain);
            return;
        }
        uint16_t gain_milli = 0u;
        if (!parseMicPcmGain(command.value, &gain_milli) ||
            !EXTERNAL_RADIO_SetMicPcmGain(gain_milli, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "ERR", "MIC_PCM_GAIN");
            return;
        }
        char gain[16];
        formatMicPcmGain(EXTERNAL_RADIO_GetConfig()->mic_pcm_gain_milli,
                         gain, sizeof(gain));
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, "MIC_PCM_GAIN", gain);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "VOLUME")) {
        if (is_query) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOLUME", config->line_out_volume);
            return;
        }
        unsigned long value = 0u;
        if (!parseUnsignedValue(command.value, &value) || value > 255u ||
            !EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(value), true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "VOLUME");
            return;
        }
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        applyCurrentAudioConfig();
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "VOLUME", updated->line_out_volume);
        return;
    }

    // Music player bring-up commands: AT+PLAY=/sdcard/music/a.wav, AT+PLAY=?
    // (current track), AT+STOP. UI/playlist control arrives with the player
    // screen; these stay as the serial/remote test path.
    if (stringEqualsIgnoreCase(command.command, "PLAY")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "PLAY",
                               MUSIC_IsPlaying() ? MUSIC_CurrentPath() : "(idle)");
            const MediaTrackInfo *track = MUSIC_GetTrackInfo();
            if (MUSIC_IsPlaying() && track != nullptr && track->title[0] != '\0') {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TITLE", track->title);
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ARTIST", track->artist);
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ALBUM", track->album);
            }
            return;
        }
        if (!MUSIC_PlayFile(command.value)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "PLAY");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "PLAY", command.value);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "STOP")) {
        MUSIC_Stop();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "STOP", "OK");
        return;
    }

    // TF card maintenance. AT+SDMOUNT=1 retries the mount after a hot insert
    // (AT+SDMOUNT=? reports state). AT+SDFORMAT=YES ERASES the card and
    // formats it FAT in-device -- the rescue path for factory exFAT/NTFS
    // cards this FatFs build cannot read (also sidesteps Windows' 32 GB
    // FAT32 limit). Formatting a large card can block for tens of seconds.
    if (stringEqualsIgnoreCase(command.command, "SDMOUNT")) {
        const bool mounted = is_query ? STORAGE_SdMounted() : STORAGE_SdMountRetry();
        if (!is_query && mounted) {
            (void)PLAYLIST_Scan();
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SDMOUNT",
                           mounted ? STORAGE_SdMountPoint() : "(not mounted)");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "SDFORMAT")) {
        if (!stringEqualsIgnoreCase(command.value, "YES")) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SDFORMAT",
                               "card will be ERASED; confirm with AT+SDFORMAT=YES");
            return;
        }
        if (!STORAGE_SdFormat()) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "SDFORMAT");
            return;
        }
        (void)PLAYLIST_Scan();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SDFORMAT", "OK");
        return;
    }

    // Net-radio favorite stations (services/radio_favorites.h):
    //   AT+RADIOLIST=?      -> AT+RADIOCNT/RADIOCUR + AT+RADIO<i>=<name> lines
    //   AT+RADIOLIST=<i>    -> one entry with its URL
    //   AT+RADIOADD=<name>,<url>  (URL detected at the ",http" boundary so
    //                              names may contain commas)
    //   AT+RADIODEL=<i>
    //   AT+RADIOPLAY=<i>    -> tune favorite i (AT+RADIOPLAY=? for status)
    //   AT+RADIONEXT=1 / AT+RADIOPREV=1 -> switch station; AT+STOP stops.
    if (stringEqualsIgnoreCase(command.command, "RADIOLIST")) {
        char name[RADIO_FAV_NAME_SIZE];
        char url[RADIO_FAV_URL_SIZE];
        char line[300];
        if (!is_query) {
            unsigned long index = 0UL;
            if (!parseUnsignedValue(command.value, &index) ||
                !RADIO_FAV_Get(index, name, sizeof(name), url, sizeof(url))) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "RADIOLIST");
                return;
            }
            snprintf(line, sizeof(line), "AT+RADIO%lu=%s,%s", index, name, url);
            appendReplyLine(result->payload, sizeof(result->payload), &result->payload_size, line);
            return;
        }
        const size_t count = RADIO_FAV_Count();
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "RADIOCNT",
                           static_cast<unsigned>(count));
        const int current = RADIO_FAV_CurrentIndex();
        if (current >= 0) {
            appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "RADIOCUR",
                               static_cast<unsigned>(current));
        }
        for (size_t i = 0; i < count; ++i) {
            if (!RADIO_FAV_Get(i, name, sizeof(name), nullptr, 0)) {
                break;
            }
            snprintf(line, sizeof(line), "AT+RADIO%u=%s", static_cast<unsigned>(i), name);
            appendReplyLine(result->payload, sizeof(result->payload), &result->payload_size, line);
        }
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "RADIOADD")) {
        char parse[sizeof(command.value)];
        snprintf(parse, sizeof(parse), "%s", command.value);
        char *url = nullptr;
        if (strncmp(parse, "http://", 7) == 0 || strncmp(parse, "https://", 8) == 0) {
            url = parse; // URL only, name defaults to the URL
        } else {
            char *split = strstr(parse, ",http");
            if (split != nullptr) {
                *split = '\0';
                url = split + 1;
            }
        }
        int index = -1;
        if (url == nullptr ||
            !RADIO_FAV_Set(-1, (url == parse) ? nullptr : parse, url, &index)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "RADIOADD");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "RADIOADD",
                           static_cast<unsigned>(index));
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "RADIODEL")) {
        unsigned long index = 0UL;
        if (is_query || !parseUnsignedValue(command.value, &index) ||
            !RADIO_FAV_Remove(index)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "RADIODEL");
            return;
        }
        appendUnsignedLine(result->payload, sizeof(result->payload), &result->payload_size, "RADIODEL",
                           static_cast<unsigned>(index));
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "RADIOPLAY") ||
        stringEqualsIgnoreCase(command.command, "RADIONEXT") ||
        stringEqualsIgnoreCase(command.command, "RADIOPREV")) {
        if (is_query) {
            const int current = RADIO_FAV_CurrentIndex();
            char name[RADIO_FAV_NAME_SIZE] = {};
            char status[300];
            if (current >= 0 && RADIO_FAV_Get(current, name, sizeof(name), nullptr, 0)) {
                snprintf(status, sizeof(status), "%d,%s%s", current, name,
                         MUSIC_IsPlaying() ? "" : " (stopped)");
            } else {
                snprintf(status, sizeof(status), "(none)");
            }
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "RADIOPLAY", status);
            return;
        }
        bool ok;
        if (stringEqualsIgnoreCase(command.command, "RADIONEXT")) {
            ok = RADIO_FAV_Next();
        } else if (stringEqualsIgnoreCase(command.command, "RADIOPREV")) {
            ok = RADIO_FAV_Prev();
        } else {
            unsigned long index = 0UL;
            ok = parseUnsignedValue(command.value, &index) && RADIO_FAV_PlayIndex(index);
        }
        if (!ok) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", command.command);
            return;
        }
        char name[RADIO_FAV_NAME_SIZE] = {};
        (void)RADIO_FAV_Get(RADIO_FAV_CurrentIndex(), name, sizeof(name), nullptr, 0);
        char line[300];
        snprintf(line, sizeof(line), "AT+RADIOPLAY=%d,%s", RADIO_FAV_CurrentIndex(), name);
        appendReplyLine(result->payload, sizeof(result->payload), &result->payload_size, line);
        return;
    }

    // Video call camera TX: AT+VIDEO=ON|OFF|? (RX is always passive; open
    // the LCD Video page to watch the remote side).
    if (stringEqualsIgnoreCase(command.command, "VIDEO")) {
        if (is_query) {
            char status[48];
            snprintf(status, sizeof(status), "TX %s, RX %s",
                     VIDEO_TxEnabled() ? "ON" : "OFF",
                     VIDEO_Receiving() ? "ACTIVE" : "IDLE");
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "VIDEO", status);
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled) || !VIDEO_SetTxEnabled(enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "VIDEO");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "VIDEO",
                           enabled ? "ON" : "OFF");
        return;
    }

    // xiaozhi AI assistant: AT+AI=wss://server/xiaozhi/v1/,token (configure),
    // AT+AI=ON|OFF, AT+AI=?; AT+AITALK=START|STOP for a push-to-talk turn.
    if (stringEqualsIgnoreCase(command.command, "AI")) {
        char status[224];
        if (is_query) {
            AI_Describe(status, sizeof(status));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AI", status);
            return;
        }
        bool enabled = false;
        if (parseBoolValue(command.value, &enabled)) {
            if (!AI_SetEnabled(enabled)) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "AI");
                return;
            }
        } else {
            char parse[224];
            snprintf(parse, sizeof(parse), "%.*s",
                     static_cast<int>(sizeof(parse) - 1u), command.value);
            char *token = strchr(parse, ',');
            if (token != nullptr) {
                *token++ = '\0';
            }
            if (!AI_Configure(parse, token)) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "AI");
                return;
            }
        }
        AI_Describe(status, sizeof(status));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AI", status);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "AITALK")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AITALK",
                               AI_IsListening() ? "LISTENING" : "IDLE");
            return;
        }
        if (stringEqualsIgnoreCase(command.value, "START")) {
            if (!AI_StartListen()) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "AITALK");
                return;
            }
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AITALK", "LISTENING");
            return;
        }
        AI_StopListen();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AITALK", "IDLE");
        return;
    }

    // Music local output device: AT+OUTPUT=SPK (onboard hi-fi speaker) or
    // AT+OUTPUT=BT (Bluetooth headset via A2DP; falls back to the speaker
    // when the headset is unreachable). Applies from the next track.
    if (stringEqualsIgnoreCase(command.command, "OUTPUT")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "OUTPUT",
                               (MUSIC_GetOutput() == MUSIC_OUTPUT_BT) ? "BT" : "SPK");
            return;
        }
        int output = -1;
        if (stringEqualsIgnoreCase(command.value, "SPK") ||
            stringEqualsIgnoreCase(command.value, "SPEAKER")) {
            output = MUSIC_OUTPUT_SPEAKER;
        } else if (stringEqualsIgnoreCase(command.value, "BT")) {
            output = MUSIC_OUTPUT_BT;
        }
        if (output < 0) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "OUTPUT");
            return;
        }
        MUSIC_SetOutput(output);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "OUTPUT",
                           (output == MUSIC_OUTPUT_BT) ? "BT" : "SPK");
        return;
    }

    // NRL TX voice codec: AT+CODEC=G711 (8k narrowband, packet type 1) or
    // AT+CODEC=OPUS (16k wideband, packet type 8). RX always accepts both.
    if (stringEqualsIgnoreCase(command.command, "CODEC")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CODEC",
                               (NRLAudioBridge_GetVoiceCodec() == 1u) ? "OPUS" : "G711");
            return;
        }
        uint8_t codec = 0xFFu;
        if (stringEqualsIgnoreCase(command.value, "G711")) {
            codec = 0u;
        } else if (stringEqualsIgnoreCase(command.value, "OPUS")) {
            codec = 1u;
        }
        if (codec > 1u) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "CODEC");
            return;
        }
        if (!NRLAudioBridge_SetVoiceCodec(codec)) {
            // Opus pre-allocation failed; the codec stayed on G.711.
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "ERR", "CODEC_NOMEM");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "CODEC",
                           (codec == 1u) ? "OPUS" : "G711");
        return;
    }

    // ESP-NOW off-grid voice link: AT+ESPNOW=ON|OFF|? (broadcast intercom
    // between NRL-ESP32 devices on the same WiFi channel, no server).
    if (stringEqualsIgnoreCase(command.command, "ESPNOW")) {
        if (is_query) {
            char peer[16] = {};
            ESPNOW_LINK_GetLastPeer(peer, sizeof(peer));
            char status[48];
            snprintf(status, sizeof(status), "%s%s%s",
                     ESPNOW_LINK_IsEnabled() ? "ON" : "OFF",
                     peer[0] != '\0' ? " last=" : "",
                     peer);
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ESPNOW", status);
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled) || !ESPNOW_LINK_SetEnabled(enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "ESPNOW");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ESPNOW",
                           enabled ? "ON" : "OFF");
        return;
    }

    // Independent ESP-NOW receive switch (persisted, defaults ON): incoming
    // intercom voice is heard whenever this is on, regardless of AT+ESPNOW.
    if (stringEqualsIgnoreCase(command.command, "ESPNOW_RX")) {
        if (!is_query) {
            bool enabled = false;
            if (!parseBoolValue(command.value, &enabled)) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "ESPNOW_RX");
                return;
            }
            ESPNOW_LINK_SetRxEnabled(enabled);
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ESPNOW_RX",
                           ESPNOW_LINK_IsRxEnabled() ? "ON" : "OFF");
        return;
    }

    // ESP-NOW TX voice codec, independent of the NRL AT+CODEC:
    // AT+ESPNOW_CODEC=G711|OPUS|?. RX always auto-detects both.
    if (stringEqualsIgnoreCase(command.command, "ESPNOW_CODEC")) {
        if (!is_query) {
            uint8_t codec = 0xFFu;
            if (stringEqualsIgnoreCase(command.value, "G711")) {
                codec = 0u;
            } else if (stringEqualsIgnoreCase(command.value, "OPUS")) {
                codec = 1u;
            }
            if (codec > 1u) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "ESPNOW_CODEC");
                return;
            }
            if (!ESPNOW_LINK_SetTxCodec(codec)) {
                // Opus pre-allocation failed; the codec stayed on G.711.
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "CODEC_NOMEM");
                return;
            }
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ESPNOW_CODEC",
                           (ESPNOW_LINK_GetTxCodec() == 1u) ? "OPUS" : "G711");
        return;
    }

    // CTCSS/PL receive detection. The master command controls both sources;
    // the route commands below keep MIC and NRL independently selectable.
    if (stringEqualsIgnoreCase(command.command, "CTCSS")) {
        SignalingConfig cfg{};
        SIGNALING_GetConfig(&cfg);
        if (!is_query) {
            bool enabled = false;
            if (!parseBoolValue(command.value, &enabled) ||
                !SIGNALING_SetCtcssRoute(SIGNAL_ROUTE_RX_MIC, enabled) ||
                !SIGNALING_SetCtcssRoute(SIGNAL_ROUTE_RX_NRL, enabled)) {
                appendKeyValueLine(result->payload, sizeof(result->payload),
                                   &result->payload_size, "ERR", "CTCSS");
                return;
            }
            cfg.ctcss_rx_mic = enabled;
            cfg.ctcss_rx_nrl = enabled;
        }
        char status[48];
        snprintf(status, sizeof(status), "RXMIC=%s,RXNRL=%s",
                 cfg.ctcss_rx_mic ? "ON" : "OFF",
                 cfg.ctcss_rx_nrl ? "ON" : "OFF");
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, "CTCSS", status);
        return;
    }

    struct CtcssAtRoute {
        const char *command;
        SignalingRoute route;
    };
    static const CtcssAtRoute ctcss_routes[] = {
        {"CTCSS_RX_MIC", SIGNAL_ROUTE_RX_MIC},
        {"CTCSS_RX_NRL", SIGNAL_ROUTE_RX_NRL},
    };
    for (const CtcssAtRoute &entry : ctcss_routes) {
        if (!stringEqualsIgnoreCase(command.command, entry.command)) continue;
        SignalingConfig cfg{};
        SIGNALING_GetConfig(&cfg);
        bool enabled = entry.route == SIGNAL_ROUTE_RX_MIC
                           ? cfg.ctcss_rx_mic : cfg.ctcss_rx_nrl;
        if (!is_query && (!parseBoolValue(command.value, &enabled) ||
                          !SIGNALING_SetCtcssRoute(entry.route, enabled))) {
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "ERR", entry.command);
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, entry.command,
                           enabled ? "ON" : "OFF");
        return;
    }

    // CW receive decoding mirrors MDC: MIC and NRL downlink are independently
    // selectable. AT+CW is a convenient master for both receive sources.
    if (stringEqualsIgnoreCase(command.command, "CW")) {
        SignalingConfig cfg{};
        SIGNALING_GetConfig(&cfg);
        if (!is_query) {
            bool enabled = false;
            if (!parseBoolValue(command.value, &enabled) ||
                !SIGNALING_SetCwRoute(SIGNAL_ROUTE_RX_MIC, enabled) ||
                !SIGNALING_SetCwRoute(SIGNAL_ROUTE_RX_NRL, enabled)) {
                appendKeyValueLine(result->payload, sizeof(result->payload),
                                   &result->payload_size, "ERR", "CW");
                return;
            }
            cfg.cw_rx_mic = enabled;
            cfg.cw_rx_nrl = enabled;
        }
        char status[48];
        snprintf(status, sizeof(status), "RXMIC=%s,RXNRL=%s",
                 cfg.cw_rx_mic ? "ON" : "OFF", cfg.cw_rx_nrl ? "ON" : "OFF");
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, "CW", status);
        return;
    }
    struct CwAtRoute { const char *command; SignalingRoute route; };
    static const CwAtRoute cw_routes[] = {
        {"CW_RX_MIC", SIGNAL_ROUTE_RX_MIC}, {"CW_RX_NRL", SIGNAL_ROUTE_RX_NRL},
    };
    for (const CwAtRoute &entry : cw_routes) {
        if (!stringEqualsIgnoreCase(command.command, entry.command)) continue;
        SignalingConfig cfg{};
        SIGNALING_GetConfig(&cfg);
        bool enabled = entry.route == SIGNAL_ROUTE_RX_MIC ? cfg.cw_rx_mic : cfg.cw_rx_nrl;
        if (!is_query && (!parseBoolValue(command.value, &enabled) ||
                          !SIGNALING_SetCwRoute(entry.route, enabled))) {
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "ERR", entry.command);
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, entry.command, enabled ? "ON" : "OFF");
        return;
    }

    // CW practice configuration: character set (KOCH / LETTERS / DIGITS /
    // CUSTOM:...) and adaptive speed. Values persist in NVS.
    if (stringEqualsIgnoreCase(command.command, "CWSET")) {
        if (!is_query) {
            CwCharset charset = CW_CHARSET_KOCH;
            const char *custom = nullptr;
            bool valid = true;
            if (stringEqualsIgnoreCase(command.value, "KOCH")) {
                charset = CW_CHARSET_KOCH;
            } else if (stringEqualsIgnoreCase(command.value, "LETTERS")) {
                charset = CW_CHARSET_LETTERS;
            } else if (stringEqualsIgnoreCase(command.value, "DIGITS")) {
                charset = CW_CHARSET_DIGITS;
            } else if (strncasecmp(command.value, "CUSTOM", 6u) == 0) {
                charset = CW_CHARSET_CUSTOM;
                custom = command.value[6] == ':' ? command.value + 7 : nullptr;
            } else {
                valid = false;
            }
            if (!valid) {
                appendKeyValueLine(result->payload, sizeof(result->payload),
                                   &result->payload_size, "ERR", "CWSET");
                return;
            }
            CW_SERVICE_SetCharset(charset, custom);
        }
        static const char *const kCharsetNames[] = {"KOCH", "LETTERS", "DIGITS", "CUSTOM"};
        CwSnapshot cw{};
        CW_SERVICE_GetSnapshot(&cw);
        char custom[40];
        CW_SERVICE_GetCustomCharset(custom, sizeof(custom));
        char status[128];
        snprintf(status, sizeof(status), "%s,LV=%u,WPM=%u,ADAPT=%s,CUSTOM=%s",
                 kCharsetNames[cw.charset <= CW_CHARSET_CUSTOM ? cw.charset : 0u],
                 static_cast<unsigned>(cw.koch_unlocked),
                 static_cast<unsigned>(cw.wpm),
                 cw.adaptive_wpm ? "ON" : "OFF", custom[0] != '\0' ? custom : "-");
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, "CWSET", status);
        return;
    }
    if (stringEqualsIgnoreCase(command.command, "CWADAPT")) {
        CwSnapshot cw{};
        CW_SERVICE_GetSnapshot(&cw);
        bool enabled = cw.adaptive_wpm;
        if (!is_query && !parseBoolValue(command.value, &enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "ERR", "CWADAPT");
            return;
        }
        if (!is_query) CW_SERVICE_SetAdaptiveWpm(enabled);
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, "CWADAPT", enabled ? "ON" : "OFF");
        return;
    }

    // MDC1200/DTMF signaling. Route switches are independent; MDC/DTMF
    // payloads are pre-generated into PSRAM when their IDs change.
    if (stringEqualsIgnoreCase(command.command, "MDC") ||
        stringEqualsIgnoreCase(command.command, "DTMF")) {
        const bool mdc = stringEqualsIgnoreCase(command.command, "MDC");
        SignalingConfig cfg{};
        SIGNALING_GetConfig(&cfg);
        if (is_query) {
            char status[128];
            if (mdc) {
                snprintf(status, sizeof(status), "ID=%04X,OP=%02X,ARG=%02X,RXMIC=%s,RXNRL=%s,TXNRL=%s,TXSPK=%s",
                         cfg.mdc_unit_id, cfg.mdc_opcode, cfg.mdc_argument,
                         cfg.mdc_rx_mic ? "ON" : "OFF", cfg.mdc_rx_nrl ? "ON" : "OFF",
                         cfg.mdc_tx_nrl ? "ON" : "OFF", cfg.mdc_tx_speaker ? "ON" : "OFF");
            } else {
                snprintf(status, sizeof(status), "ID=%s,RXMIC=%s,RXNRL=%s,TXNRL=%s,TXSPK=%s",
                         cfg.dtmf_digits, cfg.dtmf_rx_mic ? "ON" : "OFF",
                         cfg.dtmf_rx_nrl ? "ON" : "OFF", cfg.dtmf_tx_nrl ? "ON" : "OFF",
                         cfg.dtmf_tx_speaker ? "ON" : "OFF");
            }
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               mdc ? "MDC" : "DTMF", status);
            return;
        }
        bool ok = false;
        if (mdc) {
            char value[sizeof(command.value)];
            snprintf(value, sizeof(value), "%s", command.value);
            char *arg_text = strchr(value, ',');
            char *id_text = arg_text != nullptr ? strchr(arg_text + 1, ',') : nullptr;
            if (arg_text != nullptr && id_text != nullptr) {
                *arg_text++ = '\0';
                *id_text++ = '\0';
                unsigned long op = 0, arg = 0, unit_id = 0;
                ok = parseHexValue(value, &op) && op <= 0xFFUL &&
                     parseHexValue(arg_text, &arg) && arg <= 0xFFUL &&
                     parseHexValue(id_text, &unit_id) && unit_id <= 0xFFFFUL &&
                     SIGNALING_SetMdcPacket(static_cast<uint8_t>(op), static_cast<uint8_t>(arg),
                                            static_cast<uint16_t>(unit_id));
            }
        } else {
            ok = SIGNALING_SetDtmfDigits(command.value);
        }
        if (!ok) appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", mdc ? "MDC" : "DTMF");
        else appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, mdc ? "MDC" : "DTMF", command.value);
        return;
    }

    struct SignalingAtRoute {
        const char *command;
        bool mdc;
        SignalingRoute route;
    };
    static const SignalingAtRoute signaling_routes[] = {
        {"MDC_RX_MIC", true, SIGNAL_ROUTE_RX_MIC}, {"MDC_RX_NRL", true, SIGNAL_ROUTE_RX_NRL},
        {"MDC_TX_NRL", true, SIGNAL_ROUTE_TX_NRL}, {"MDC_TX_SPK", true, SIGNAL_ROUTE_TX_SPEAKER},
        {"DTMF_RX_MIC", false, SIGNAL_ROUTE_RX_MIC}, {"DTMF_RX_NRL", false, SIGNAL_ROUTE_RX_NRL},
        {"DTMF_TX_NRL", false, SIGNAL_ROUTE_TX_NRL}, {"DTMF_TX_SPK", false, SIGNAL_ROUTE_TX_SPEAKER},
    };
    for (const SignalingAtRoute &entry : signaling_routes) {
        if (!stringEqualsIgnoreCase(command.command, entry.command)) continue;
        SignalingConfig cfg{};
        SIGNALING_GetConfig(&cfg);
        bool enabled = false;
        if (entry.mdc) {
            if (entry.route == SIGNAL_ROUTE_RX_MIC) enabled = cfg.mdc_rx_mic;
            else if (entry.route == SIGNAL_ROUTE_RX_NRL) enabled = cfg.mdc_rx_nrl;
            else if (entry.route == SIGNAL_ROUTE_TX_NRL) enabled = cfg.mdc_tx_nrl;
            else enabled = cfg.mdc_tx_speaker;
        } else {
            if (entry.route == SIGNAL_ROUTE_RX_MIC) enabled = cfg.dtmf_rx_mic;
            else if (entry.route == SIGNAL_ROUTE_RX_NRL) enabled = cfg.dtmf_rx_nrl;
            else if (entry.route == SIGNAL_ROUTE_TX_NRL) enabled = cfg.dtmf_tx_nrl;
            else enabled = cfg.dtmf_tx_speaker;
        }
        if (!is_query) {
            if (!parseBoolValue(command.value, &enabled) ||
                !(entry.mdc ? SIGNALING_SetMdcRoute(entry.route, enabled)
                            : SIGNALING_SetDtmfRoute(entry.route, enabled))) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", entry.command);
                return;
            }
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           entry.command, enabled ? "ON" : "OFF");
        return;
    }

    // APRS transceiver family:
    //   AT+APRS=ON|OFF|?          master switch / status summary
    //   AT+APRS_NET=ON|OFF        APRS-IS uplink/downlink
    //   AT+APRS_TX=ON|OFF         AFSK beacon out through the radio
    //   AT+APRS_RX=ON|OFF         demodulate radio audio
    //   AT+APRS_AUTO=ON|OFF       speed/movement-adaptive beacon interval
    //   AT+APRS_FIXED=ON|OFF      allow default-position beacons without GPS
    //   AT+APRS_SERVER=host:port  APRS-IS server
    //   AT+APRS_SSID=0..15        SSID appended to the radio callsign
    //   AT+APRS_SYMBOL=/I         symbol table+code (TCP/IP by default)
    //   AT+APRS_INTERVAL=60       beacon interval, seconds
    //   AT+APRS_POS=ddmm.mmmm,dddmm.mmmm
    //                              default WGS-84 position (N/E automatic)
    //   AT+APRS_PATH=WIDE1-1      RF digi path
    //   AT+APRS_COMMENT=text      beacon comment
    //   AT+APRS_BEACON            queue a beacon right now
    //   AT+APRS_LIST              recent stations heard
    if (stringEqualsIgnoreCase(command.command, "APRS")) {
        if (is_query) {
            AprsConfig cfg;
            APRS_SERVICE_GetConfig(&cfg);
            char status[128];
            snprintf(status, sizeof(status), "%s net=%s%s rf_tx=%s rf_rx=%s auto=%s fixed=%s gps=%s rx=%lu tx=%lu",
                     cfg.enabled ? "ON" : "OFF",
                     cfg.net_enabled ? "ON" : "OFF",
                     APRS_SERVICE_IsNetConnected() ? "(conn)" : "",
                     cfg.rf_tx_enabled ? "ON" : "OFF",
                     cfg.rf_rx_enabled ? "ON" : "OFF",
                     cfg.auto_interval ? "ON" : "OFF",
                     cfg.fixed_beacon_without_gps ? "ON" : "OFF",
                     APRS_SERVICE_GpsHasFix() ? "FIX" : "NO",
                     static_cast<unsigned long>(APRS_SERVICE_GetRxCount()),
                     static_cast<unsigned long>(APRS_SERVICE_GetTxCount()));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS", status);
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled) || !APRS_SERVICE_SetEnabled(enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS",
                           enabled ? "ON" : "OFF");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_NET") ||
        stringEqualsIgnoreCase(command.command, "APRS_TX") ||
        stringEqualsIgnoreCase(command.command, "APRS_RX") ||
        stringEqualsIgnoreCase(command.command, "APRS_AUTO") ||
        stringEqualsIgnoreCase(command.command, "APRS_FIXED")) {
        AprsConfig cfg;
        APRS_SERVICE_GetConfig(&cfg);
        const bool is_net = stringEqualsIgnoreCase(command.command, "APRS_NET");
        const bool is_tx = stringEqualsIgnoreCase(command.command, "APRS_TX");
        const bool is_auto = stringEqualsIgnoreCase(command.command, "APRS_AUTO");
        const bool is_fixed = stringEqualsIgnoreCase(command.command, "APRS_FIXED");
        if (!is_query) {
            bool enabled = false;
            bool ok = parseBoolValue(command.value, &enabled);
            if (ok) {
                if (is_net) {
                    ok = APRS_SERVICE_SetNetEnabled(enabled);
                } else if (is_tx) {
                    ok = APRS_SERVICE_SetRfTxEnabled(enabled);
                } else if (is_auto) {
                    ok = APRS_SERVICE_SetAutoInterval(enabled);
                } else if (is_fixed) {
                    ok = APRS_SERVICE_SetFixedBeaconWithoutGps(enabled);
                } else {
                    ok = APRS_SERVICE_SetRfRxEnabled(enabled);
                }
            }
            if (!ok) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", command.command);
                return;
            }
            APRS_SERVICE_GetConfig(&cfg);
        }
        const bool state = is_net ? cfg.net_enabled
                                  : (is_tx ? cfg.rf_tx_enabled
                                           : (is_auto ? cfg.auto_interval
                                                      : (is_fixed ? cfg.fixed_beacon_without_gps
                                                                  : cfg.rf_rx_enabled)));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           command.command, state ? "ON" : "OFF");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_SERVER")) {
        if (!is_query) {
            char host[65] = {};
            unsigned port = 14580;
            const char *colon = strrchr(command.value, ':');
            if (colon != nullptr && colon != command.value) {
                const size_t host_len = static_cast<size_t>(colon - command.value);
                if (host_len >= sizeof(host) || sscanf(colon + 1, "%u", &port) != 1) {
                    appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_SERVER");
                    return;
                }
                memcpy(host, command.value, host_len);
            } else {
                snprintf(host, sizeof(host), "%.64s", command.value);
            }
            if (port == 0 || port > 65535u || !APRS_SERVICE_SetServer(host, static_cast<uint16_t>(port))) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_SERVER");
                return;
            }
        }
        AprsConfig cfg;
        APRS_SERVICE_GetConfig(&cfg);
        char server[80];
        snprintf(server, sizeof(server), "%s:%u", cfg.server_host, static_cast<unsigned>(cfg.server_port));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_SERVER", server);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_SSID")) {
        AprsConfig cfg;
        if (!is_query) {
            unsigned ssid = 0;
            if (sscanf(command.value, "%u", &ssid) != 1 || ssid > 15u ||
                !APRS_SERVICE_SetSsid(static_cast<uint8_t>(ssid))) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_SSID");
                return;
            }
        }
        APRS_SERVICE_GetConfig(&cfg);
        char text[8];
        snprintf(text, sizeof(text), "%u", static_cast<unsigned>(cfg.ssid));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_SSID", text);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_SYMBOL")) {
        AprsConfig cfg;
        if (!is_query) {
            if (strlen(command.value) != 2u ||
                !APRS_SERVICE_SetSymbol(command.value[0], command.value[1])) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_SYMBOL");
                return;
            }
        }
        APRS_SERVICE_GetConfig(&cfg);
        char text[4] = {cfg.symbol_table, cfg.symbol_code, '\0'};
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_SYMBOL", text);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_INTERVAL")) {
        AprsConfig cfg;
        if (!is_query) {
            unsigned seconds = 0;
            if (sscanf(command.value, "%u", &seconds) != 1 || seconds > 65535u ||
                !APRS_SERVICE_SetBeaconInterval(static_cast<uint16_t>(seconds))) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_INTERVAL");
                return;
            }
        }
        APRS_SERVICE_GetConfig(&cfg);
        char text[8];
        snprintf(text, sizeof(text), "%u", static_cast<unsigned>(cfg.beacon_interval_s));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_INTERVAL", text);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_POS")) {
        if (!is_query) {
            double lat = 0.0, lon = 0.0;
            const char *comma = strchr(command.value, ',');
            if (comma == nullptr) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_POS");
                return;
            }
            char lat_text[24] = {};
            const size_t lat_len = static_cast<size_t>(comma - command.value);
            if (lat_len == 0u || lat_len >= sizeof(lat_text)) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_POS");
                return;
            }
            memcpy(lat_text, command.value, lat_len);
            if (!APRS_SERVICE_ParseAprsCoord(lat_text, true, &lat) ||
                !APRS_SERVICE_ParseAprsCoord(comma + 1, false, &lon) ||
                !APRS_SERVICE_SetDefaultPosition(lat, lon)) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_POS");
                return;
            }
        }
        double lat = 0.0, lon = 0.0, alt = 0.0;
        const bool fix = APRS_SERVICE_GetOwnPosition(&lat, &lon, &alt);
        char text[64];
        char lat_text[16], lon_text[16];
        APRS_SERVICE_FormatAprsCoord(lat, true, lat_text, sizeof(lat_text));
        APRS_SERVICE_FormatAprsCoord(lon, false, lon_text, sizeof(lon_text));
        snprintf(text, sizeof(text), "%s,%s %s", lat_text, lon_text, fix ? "GPS" : "DEFAULT");
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_POS", text);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_PATH")) {
        AprsConfig cfg;
        if (!is_query) {
            if (!APRS_SERVICE_SetPath(command.value)) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_PATH");
                return;
            }
        }
        APRS_SERVICE_GetConfig(&cfg);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_PATH",
                           cfg.path[0] != '\0' ? cfg.path : "(none)");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_COMMENT")) {
        AprsConfig cfg;
        if (!is_query) {
            if (!APRS_SERVICE_SetComment(command.value)) {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_COMMENT");
                return;
            }
        }
        APRS_SERVICE_GetConfig(&cfg);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_COMMENT",
                           cfg.comment[0] != '\0' ? cfg.comment : "(none)");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "APRS_BEACON")) {
        if (!APRS_SERVICE_SendBeaconNow()) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "APRS_OFF");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_BEACON", "QUEUED");
        return;
    }

    // Station list. Replies must fit the 1024-byte remote-AT UDP budget, so
    // cap the station count and keep each line compact.
    if (stringEqualsIgnoreCase(command.command, "APRS_LIST")) {
        AprsStationInfo stations[8];
        const size_t count = APRS_SERVICE_GetStations(stations, 8);
        if (count == 0) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "APRS_LIST", "EMPTY");
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            const AprsStationInfo &s = stations[i];
            char line[104];
            char spd[16] = "-";
            if (!isnan(s.speed_kmh)) {
                snprintf(spd, sizeof(spd), "%.0fkm/h", static_cast<double>(s.speed_kmh));
            } else if (!isnan(s.derived_speed_kmh)) {
                snprintf(spd, sizeof(spd), "~%.0fkm/h", static_cast<double>(s.derived_speed_kmh));
            }
            char dist[16] = "-";
            if (!isnan(s.distance_km)) {
                snprintf(dist, sizeof(dist), "%.1fkm", static_cast<double>(s.distance_km));
            }
            snprintf(line, sizeof(line), "%.4f,%.4f %s %s %s %lus",
                     static_cast<double>(s.lat), static_cast<double>(s.lon),
                     dist, spd, s.via_rf ? "RF" : "IS",
                     static_cast<unsigned long>(s.age_s));
            if (!appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                                    s.name, line)) {
                break;
            }
        }
        return;
    }

    // Physical/user PTT target:
    //   AT+PTT_MODE=NRL     -> existing NRL uplink behavior
    //   AT+PTT_MODE=ESPNOW  -> the same PTT/SQL gate broadcasts over ESP-NOW
    if (stringEqualsIgnoreCase(command.command, "PTT_MODE")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "PTT_MODE", (ESPNOW_LINK_GetPttMode() == 1u) ? "ESPNOW" : "NRL");
            return;
        }
        uint8_t mode = 0xFFu;
        if (stringEqualsIgnoreCase(command.value, "NRL") ||
            stringEqualsIgnoreCase(command.value, "NET") ||
            stringEqualsIgnoreCase(command.value, "NETWORK")) {
            mode = 0u;
        } else if (stringEqualsIgnoreCase(command.value, "ESPNOW") ||
                   stringEqualsIgnoreCase(command.value, "ESP-NOW")) {
            mode = 1u;
        }
        if (mode > 1u) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "ERR", "PTT_MODE");
            return;
        }
        if (mode == 1u && !ESPNOW_LINK_IsEnabled() &&
            !ESPNOW_LINK_SetEnabled(true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "ERR", "ESPNOW");
            return;
        }
        ESPNOW_LINK_SetPttMode(mode);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           "PTT_MODE", (mode == 1u) ? "ESPNOW" : "NRL");
        return;
    }

    // Nanny beacon: AT+BEACON=/sdcard/beacon/id.wav,30 (path, minutes),
    // AT+BEACON=OFF, AT+BEACON=?.
    if (stringEqualsIgnoreCase(command.command, "BEACON")) {
        char path[128];
        uint32_t interval = 0;
        if (is_query) {
            if (NANNY_GetBeacon(path, sizeof(path), &interval)) {
                char status[160];
                snprintf(status, sizeof(status), "%s,%lu", path, static_cast<unsigned long>(interval));
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", status);
            } else {
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", "OFF");
            }
            return;
        }
        if (stringEqualsIgnoreCase(command.value, "OFF")) {
            NANNY_DisableBeacon();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", "OFF");
            return;
        }
        char parse[160];
        snprintf(parse, sizeof(parse), "%.*s",
                 static_cast<int>(sizeof(parse) - 1u), command.value);
        char *comma = strrchr(parse, ',');
        unsigned long minutes = 0;
        if (comma == nullptr) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BEACON");
            return;
        }
        *comma++ = '\0';
        if (!parseUnsignedValue(comma, &minutes) ||
            !NANNY_SetBeacon(parse, static_cast<uint32_t>(minutes))) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "BEACON");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "BEACON", command.value);
        return;
    }

    // Music playback target (nanny 三档切换): AT+TARGET=LOCAL|NET|BOTH.
    // LOCAL = speaker only; NET = stream to the NRL server (8k G.711, mic
    // muted while streaming); BOTH = fan-out to both. Applies to the next
    // track started.
    if (stringEqualsIgnoreCase(command.command, "TARGET")) {
        static const char *kTargetNames[] = {"LOCAL", "NET", "BOTH"};
        if (is_query) {
            const int t = MUSIC_GetTarget();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TARGET",
                               (t >= 0 && t <= 2) ? kTargetNames[t] : "?");
            return;
        }
        int target = -1;
        if (stringEqualsIgnoreCase(command.value, "LOCAL")) {
            target = MUSIC_TARGET_LOCAL;
        } else if (stringEqualsIgnoreCase(command.value, "NET")) {
            target = MUSIC_TARGET_NET;
        } else if (stringEqualsIgnoreCase(command.value, "BOTH")) {
            target = MUSIC_TARGET_BOTH;
        }
        if (target < 0) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "TARGET");
            return;
        }
        MUSIC_SetTarget(target);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TARGET",
                           kTargetNames[target]);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "NEXT") ||
        stringEqualsIgnoreCase(command.command, "PREV")) {
        const bool ok = stringEqualsIgnoreCase(command.command, "NEXT") ? PLAYLIST_Next() : PLAYLIST_Prev();
        if (!ok) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "PLAYLIST_EMPTY");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "PLAY", MUSIC_CurrentPath());
        return;
    }

    // CJK font engine on the S31 LCD: AT+FONT=BMP (built-in bitmap subset)
    // or AT+FONT=FT (FreeType vector from /sdcard/fonts/cjk.ttf).
    if (stringEqualsIgnoreCase(command.command, "FONT")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "FONT",
                               (Display_GetCjkFontEngine() == DISPLAY_CJK_FONT_FREETYPE) ? "FT" : "BMP");
            return;
        }
        int engine = -1;
        if (stringEqualsIgnoreCase(command.value, "BMP")) {
            engine = DISPLAY_CJK_FONT_BITMAP;
        } else if (stringEqualsIgnoreCase(command.value, "FT")) {
            engine = DISPLAY_CJK_FONT_FREETYPE;
        }
        if (engine < 0 || !Display_SetCjkFontEngine(engine)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "FONT");
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "FONT",
                           (engine == DISPLAY_CJK_FONT_FREETYPE) ? "FT" : "BMP");
        return;
    }

    // SMB network share for music playback (S31):
    //   AT+SMB=server/share[,user[,password]]   configure + mount
    //   AT+SMB=OFF                              unmount + clear config
    //   AT+SMB=?                                status
    if (stringEqualsIgnoreCase(command.command, "SMB")) {
        char status[128];
        if (is_query) {
            STORAGE_SmbDescribe(status, sizeof(status));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SMB", status);
            return;
        }
        if (stringEqualsIgnoreCase(command.value, "OFF")) {
            STORAGE_SmbClear();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SMB", "OFF");
            return;
        }
        char parse[192];
        snprintf(parse, sizeof(parse), "%.*s",
                 static_cast<int>(sizeof(parse) - 1u), command.value);
        char *user = strchr(parse, ',');
        char *pass = nullptr;
        if (user != nullptr) {
            *user++ = '\0';
            pass = strchr(user, ',');
            if (pass != nullptr) {
                *pass++ = '\0';
            }
        }
        char *share = strchr(parse, '/');
        if (share != nullptr) {
            *share++ = '\0';
        }
        if (share == nullptr || !STORAGE_SmbConfigure(parse, share, user, pass)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "SMB");
            return;
        }
        STORAGE_SmbDescribe(status, sizeof(status));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "SMB", status);
        return;
    }

    // Memory bandwidth microbenchmark: AT+MEMBENCH=1. Times memset/memcpy on
    // PSRAM and internal RAM so render-throughput problems can be split into
    // "PSRAM path is slow" vs "the drawing code is slow".
    if (stringEqualsIgnoreCase(command.command, "MEMBENCH")) {
        constexpr size_t kBench = 256 * 1024;
        uint8_t *ps = static_cast<uint8_t *>(heap_caps_malloc(kBench, MALLOC_CAP_SPIRAM));
        uint8_t *in = static_cast<uint8_t *>(heap_caps_malloc(32 * 1024, MALLOC_CAP_INTERNAL));
        char line[64];
        if (ps != nullptr) {
            int64_t t0 = esp_timer_get_time();
            for (int i = 0; i < 4; ++i) {
                memset(ps, i, kBench);
            }
            int64_t us = esp_timer_get_time() - t0;
            snprintf(line, sizeof(line), "psram memset %lld MB/s",
                     static_cast<long long>(4LL * kBench * 1000000LL / us / 1048576LL));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "MEM", line);

            t0 = esp_timer_get_time();
            for (int i = 0; i < 4; ++i) {
                memcpy(ps, ps + kBench / 2, kBench / 2);
            }
            us = esp_timer_get_time() - t0;
            snprintf(line, sizeof(line), "psram memcpy %lld MB/s",
                     static_cast<long long>(4LL * (kBench / 2) * 1000000LL / us / 1048576LL));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "MEM", line);
            heap_caps_free(ps);
        }
        if (in != nullptr) {
            const int64_t t0 = esp_timer_get_time();
            for (int i = 0; i < 32; ++i) {
                memset(in, i, 32 * 1024);
            }
            const int64_t us = esp_timer_get_time() - t0;
            snprintf(line, sizeof(line), "iram memset %lld MB/s",
                     static_cast<long long>(32LL * 32768LL * 1000000LL / us / 1048576LL));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "MEM", line);
            heap_caps_free(in);
        }
        const long fb_mbps = Display_FramebufferBenchMBps();
        snprintf(line, sizeof(line), "framebuffer memset %ld MB/s", fb_mbps);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "MEM", line);
        // Flash-through-cache read: what CJK glyph lookups actually pay during
        // rendering. 256 KB sequential mmap read defeats the cache, so this
        // times the cache-miss fill path, which the RAM benches above never see.
        uint8_t *dst = static_cast<uint8_t *>(heap_caps_malloc(kBench, MALLOC_CAP_SPIRAM));
        const esp_partition_t *app_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
        if (dst != nullptr && app_part != nullptr) {
            const void *mapped = nullptr;
            esp_partition_mmap_handle_t map_handle = 0;
            if (esp_partition_mmap(app_part, 0, kBench, ESP_PARTITION_MMAP_DATA,
                                   &mapped, &map_handle) == ESP_OK) {
                const int64_t t0 = esp_timer_get_time();
                for (int i = 0; i < 4; ++i) {
                    memcpy(dst, mapped, kBench);
                }
                const int64_t us = esp_timer_get_time() - t0;
                esp_partition_munmap(map_handle);
                snprintf(line, sizeof(line), "flash read %lld MB/s",
                         static_cast<long long>(4LL * kBench * 1000000LL / us / 1048576LL));
                appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "MEM", line);
            }
        }
        if (dst != nullptr) {
            heap_caps_free(dst);
        }
        return;
    }

    // On-demand breakdown of the work executed by nrl_main_loop. Profiling is
    // normally off, so its timestamp calls cannot affect regular CPU usage.
    // Start it, wait several seconds, then query the averages and maxima:
    //   AT+LOOP=1
    //   AT+LOOP=?
    //   AT+LOOP=0
    if (stringEqualsIgnoreCase(command.command, "LOOP")) {
        if (!is_query) {
            bool enabled = false;
            if (!parseBoolValue(command.value, &enabled)) {
                appendKeyValueLine(result->payload, sizeof(result->payload),
                                   &result->payload_size, "ERR", "LOOP");
                return;
            }
            MAIN_LOOP_PROFILE_SetEnabled(enabled);
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "LOOP", enabled ? "STARTED" : "STOPPED");
            return;
        }

        static constexpr const char *kStageNames[MAIN_LOOP_STAGE_COUNT] = {
            "status", "wifi_portal", "ble", "bt", "serial", "display",
        };
        MainLoopProfileSnapshot snapshot{};
        MAIN_LOOP_PROFILE_Get(&snapshot);
        char line[80];
        snprintf(line, sizeof(line), "%s loops=%lu",
                 snapshot.enabled ? "RUNNING" : "STOPPED",
                 static_cast<unsigned long>(snapshot.loops));
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, "LOOP", line);
        for (size_t i = 0; i < MAIN_LOOP_STAGE_COUNT; ++i) {
            const uint64_t average_us = snapshot.loops != 0u
                ? snapshot.total_us[i] / snapshot.loops : 0u;
            snprintf(line, sizeof(line), "%-11s avg=%llu us max=%lu us",
                     kStageNames[i], static_cast<unsigned long long>(average_us),
                     static_cast<unsigned long>(snapshot.max_us[i]));
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "LOOP", line);
        }
        return;
    }

    // Per-task CPU usage, top consumers first. The first request reports the
    // cumulative average since boot; later requests report the interval since
    // the previous request. This is intentionally non-blocking: TOP is handled
    // by nrl_main_loop, so sleeping here would hide that task's own CPU usage.
    if (stringEqualsIgnoreCase(command.command, "TOP")) {
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        constexpr UBaseType_t kMaxTasks = 40;
        struct Row {
            const TaskStatus_t *task;
            uint32_t delta;
        };
        // Keep snapshots and sort rows out of the 6 KB nrl_main_loop stack.
        static TaskStatus_t s_previous[kMaxTasks];
        static TaskStatus_t s_current[kMaxTasks];
        static Row s_rows[kMaxTasks];
        static UBaseType_t s_previous_n = 0u;
        static uint32_t s_previous_us = 0u;
        const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
        const UBaseType_t current_n = uxTaskGetSystemState(s_current, kMaxTasks, nullptr);
        const uint32_t window_us = s_previous_us == 0u ? now_us : now_us - s_previous_us;

        UBaseType_t rows_n = 0;
        for (UBaseType_t i = 0; i < current_n; ++i) {
            uint32_t before = 0;
            bool found = s_previous_n == 0u;
            for (UBaseType_t j = 0; j < s_previous_n; ++j) {
                // Heap reuse can give a newly-created task the same TCB
                // address as a deleted task. Match FreeRTOS' unique task
                // number as well as the handle, otherwise the subtraction
                // underflows and TOP reports impossible values (e.g. 3777%).
                if (s_previous[j].xHandle == s_current[i].xHandle &&
                    s_previous[j].xTaskNumber == s_current[i].xTaskNumber) {
                    before = s_previous[j].ulRunTimeCounter;
                    found = true;
                    break;
                }
            }
            // A task created after the previous sample has no valid interval
            // baseline; start reporting it on the next request.
            s_rows[rows_n].task = &s_current[i];
            uint32_t delta = found ? s_current[i].ulRunTimeCounter - before : 0u;
            // A single task cannot consume more than one core over the sample
            // window. This also contains any counter/identity anomaly without
            // hiding legitimate aggregate dual-core load in the other rows.
            if (delta > window_us) delta = window_us;
            s_rows[rows_n].delta = delta;
            ++rows_n;
        }
        for (UBaseType_t i = 1; i < rows_n; ++i) { // insertion sort, desc
            const Row key = s_rows[i];
            UBaseType_t j = i;
            while (j > 0 && s_rows[j - 1].delta < key.delta) {
                s_rows[j] = s_rows[j - 1];
                --j;
            }
            s_rows[j] = key;
        }
        const UBaseType_t show = (rows_n < 12u) ? rows_n : 12u;
        for (UBaseType_t i = 0; i < show; ++i) {
            // Percent of ONE core over the window, one decimal.
            const unsigned pct10 = (window_us != 0u)
                ? static_cast<unsigned>(static_cast<uint64_t>(s_rows[i].delta) * 1000u / window_us)
                : 0u;
            char core_ch = '*';
#if defined(CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID) && CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
            if (s_rows[i].task->xCoreID == 0) core_ch = '0';
            else if (s_rows[i].task->xCoreID == 1) core_ch = '1';
#endif
            char line[64];
            snprintf(line, sizeof(line), "%-16s c%c %u.%u%%",
                     s_rows[i].task->pcTaskName, core_ch, pct10 / 10u, pct10 % 10u);
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TOP", line);
        }
        memcpy(s_previous, s_current, current_n * sizeof(s_current[0]));
        s_previous_n = current_n;
        s_previous_us = now_us;
        return;
#else
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "TOP");
        return;
#endif
    }

    if (stringEqualsIgnoreCase(command.command, "PLAYLIST")) {
        if (is_query) {
            char count_line[16];
            snprintf(count_line, sizeof(count_line), "%u", static_cast<unsigned>(PLAYLIST_Count()));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TRACKS", count_line);
            return;
        }
        const size_t found = PLAYLIST_Scan();
        char count_line[16];
        snprintf(count_line, sizeof(count_line), "%u", static_cast<unsigned>(found));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "TRACKS", count_line);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "HP_DRIVE")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "HP_DRIVE", config->hp_drive_enabled ? "ON" : "OFF");
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "HP_DRIVE");
            return;
        }
        if (!EXTERNAL_RADIO_SetHpDriveEnabled(enabled, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "HP_DRIVE");
            return;
        }
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        applyCurrentAudioConfig();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "HP_DRIVE", updated->hp_drive_enabled ? "ON" : "OFF");
        return;
    }

#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    if (stringEqualsIgnoreCase(command.command, "AEC")) {
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AEC", config->aec_enabled ? "ON" : "OFF");
            return;
        }
        bool enabled = false;
        if (!parseBoolValue(command.value, &enabled)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "AEC");
            return;
        }
        if (!EXTERNAL_RADIO_SetAecEnabled(enabled, true)) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "AEC");
            return;
        }
        // AEC is brought up by the ES8311 driver at boot; the new value takes
        // effect after a restart.
        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "AEC", updated->aec_enabled ? "ON" : "OFF");
        return;
    }
#endif

    if (stringEqualsIgnoreCase(command.command, "UART0")) {
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           is_query ? "UART0" : "ERR",
                           is_query ? "RESERVED(LOG/AT)" : "UART0_RESERVED");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "UART1_ENABLE") ||
        stringEqualsIgnoreCase(command.command, "UART2_ENABLE")) {
        const bool uart1 = stringEqualsIgnoreCase(command.command, "UART1_ENABLE");
        const char *reply_key = uart1 ? "UART1_ENABLE" : "UART2_ENABLE";
        SerialPortConfig old_config{};
        SERIAL_PORT_CONFIG_Get(&old_config);
        if (is_query) {
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               reply_key,
                               (uart1 ? old_config.uart1_enabled : old_config.uart2_enabled)
                                   ? "ON" : "OFF");
            return;
        }
        bool enabled = false;
        SerialPortConfig updated = old_config;
        bool ok = parseBoolValue(command.value, &enabled);
        if (ok) {
            if (uart1) updated.uart1_enabled = enabled;
            else updated.uart2_enabled = enabled;
        }
        ok = ok && SERIAL_PORT_CONFIG_Set(&updated, true) &&
             SERIAL_PORT_CONFIG_ReloadDrivers();
        if (!ok) {
            (void)SERIAL_PORT_CONFIG_Set(&old_config, true);
            (void)SERIAL_PORT_CONFIG_ReloadDrivers();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "ERR", reply_key);
            return;
        }
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           reply_key, enabled ? "ON" : "OFF");
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "UART1_IO") ||
        stringEqualsIgnoreCase(command.command, "UART2_IO")) {
        const bool uart1 = stringEqualsIgnoreCase(command.command, "UART1_IO");
        SerialPortConfig old_config{};
        SERIAL_PORT_CONFIG_Get(&old_config);
        if (is_query) {
            char text[24];
            snprintf(text, sizeof(text), "%d,%d",
                     uart1 ? old_config.uart1_rx_pin : old_config.uart2_rx_pin,
                     uart1 ? old_config.uart1_tx_pin : old_config.uart2_tx_pin);
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               uart1 ? "UART1_IO" : "UART2_IO", text);
            return;
        }
        int rx = -1, tx = -1;
        SerialPortConfig updated = old_config;
        bool ok = parseIoConfigValue(command.value, &rx, &tx);
        if (ok && uart1) {
            updated.uart1_rx_pin = rx;
            updated.uart1_tx_pin = tx;
        } else if (ok) {
            updated.uart2_rx_pin = rx;
            updated.uart2_tx_pin = tx;
        }
        ok = ok && SERIAL_PORT_CONFIG_Set(&updated, true) &&
             SERIAL_PORT_CONFIG_ReloadDrivers();
        if (!ok) {
            (void)SERIAL_PORT_CONFIG_Set(&old_config, true);
            (void)SERIAL_PORT_CONFIG_ReloadDrivers();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "ERR", uart1 ? "UART1_IO" : "UART2_IO");
            return;
        }
        char text[24];
        snprintf(text, sizeof(text), "%d,%d", rx, tx);
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           uart1 ? "UART1_IO" : "UART2_IO", text);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "UART2")) {
        SerialPortConfig old_config{};
        SERIAL_PORT_CONFIG_Get(&old_config);
        char uart_config[32];
        if (is_query) {
            snprintf(uart_config, sizeof(uart_config), "%lu,%u,%c,%u",
                     static_cast<unsigned long>(old_config.uart2_baud),
                     static_cast<unsigned>(old_config.uart2_data_bits),
                     old_config.uart2_parity,
                     static_cast<unsigned>(old_config.uart2_stop_bits));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "UART2", uart_config);
            return;
        }
        SerialPortConfig updated = old_config;
        bool ok = parseSciConfigValue(command.value, &updated.uart2_baud,
                                      &updated.uart2_data_bits, &updated.uart2_parity,
                                      &updated.uart2_stop_bits) &&
                  SERIAL_PORT_CONFIG_Set(&updated, true) &&
                  SERIAL_PORT_CONFIG_ReloadDrivers();
        if (!ok) {
            (void)SERIAL_PORT_CONFIG_Set(&old_config, true);
            (void)SERIAL_PORT_CONFIG_ReloadDrivers();
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                               "ERR", "UART2");
            return;
        }
        snprintf(uart_config, sizeof(uart_config), "%lu,%u,%c,%u",
                 static_cast<unsigned long>(updated.uart2_baud),
                 static_cast<unsigned>(updated.uart2_data_bits), updated.uart2_parity,
                 static_cast<unsigned>(updated.uart2_stop_bits));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size,
                           "UART2", uart_config);
        return;
    }

    if (stringEqualsIgnoreCase(command.command, "SCI") ||
        stringEqualsIgnoreCase(command.command, "UART1")) {
        const bool sci_alias = stringEqualsIgnoreCase(command.command, "SCI");
        const char *reply_key = sci_alias ? "SCI" : "UART1";
        char sci_config[32];
        if (is_query) {
            formatSciConfig(config->sci, sci_config, sizeof(sci_config));
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, reply_key, sci_config);
            return;
        }

        const SciSerialConfig old_sci = config->sci;
        uint32_t baud = 0u;
        uint8_t data_bits = 0u;
        char parity = '\0';
        uint8_t stop_bits = 0u;
        if (!parseSciConfigValue(command.value, &baud, &data_bits, &parity, &stop_bits) ||
            !EXTERNAL_RADIO_SetSciConfig(baud, data_bits, parity, stop_bits, true) ||
            !SCI_SERIAL_ApplyConfig(baud, data_bits, parity, stop_bits)) {
            (void)EXTERNAL_RADIO_SetSciConfig(old_sci.baud, old_sci.data_bits,
                                              old_sci.parity, old_sci.stop_bits, true);
            (void)SCI_SERIAL_ApplyConfig(old_sci.baud, old_sci.data_bits,
                                         old_sci.parity, old_sci.stop_bits);
            appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", reply_key);
            return;
        }

        const ExternalRadioConfig *updated = EXTERNAL_RADIO_GetConfig();
        formatSciConfig(updated->sci, sci_config, sizeof(sci_config));
        appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, reply_key, sci_config);
        return;
    }

    // SSTV picture transmission:
    //   AT+SSTV=ROBOT36,<path>   start a Robot 36 frame (320x240, ~37 s)
    //   AT+SSTV=MARTIN1,<path>   start a Martin M1 frame (320x256, ~115 s)
    //   AT+SSTV=PD120,<path>     start a PD120 frame (640x496, ~127 s)
    //   AT+SSTV?                 state / mode / progress
    //   AT+SSTV=STOP             abort the running frame
    //   AT+SSTV=RX,MIC|NRL       start receiving from the mic tap / NRL downlink
    //   AT+SSTV=RX,OFF           stop receiving
    if (stringEqualsIgnoreCase(command.command, "SSTV")) {
        if (is_query) {
            SstvSnapshot snap{};
            SSTV_SERVICE_GetSnapshot(&snap);
            const char *state = "IDLE";
            if (snap.state == SSTV_STATE_PREPARING) state = "PREPARING";
            else if (snap.state == SSTV_STATE_SENDING) state = "SENDING";
            else if (snap.state == SSTV_STATE_DONE) state = "DONE";
            else if (snap.state == SSTV_STATE_ERROR) state = "ERROR";
            char status[192];
            snprintf(status, sizeof(status), "%s,MODE=%s,PROGRESS=%u%%,PATH=%s,ERR=%s",
                     state, snap.mode == SSTV_MODE_ROBOT36 ? "ROBOT36" :
                             snap.mode == SSTV_MODE_MARTIN_M1 ? "MARTIN1" : "PD120",
                     static_cast<unsigned>(snap.progress_percent), snap.path, snap.error);
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "SSTV", status);
            const char *rx_state = "OFF";
            if (snap.rx_active) {
                rx_state = snap.rx_state == SSTV_RX_VIS ? "VIS" :
                           snap.rx_state == SSTV_RX_LINES ? "RECEIVING" :
                           snap.rx_state == SSTV_RX_DONE ? "DONE" : "LISTENING";
            }
            char rx_status[96];
            snprintf(rx_status, sizeof(rx_status), "%s,SRC=%s,MODE=%s,LINES=%u/%u,Q=%u",
                     rx_state, snap.rx_source == SSTV_SOURCE_MIC ? "MIC" : "NRL",
                     snap.rx_mode == SSTV_MODE_ROBOT36 ? "ROBOT36" : "MARTIN1",
                     static_cast<unsigned>(snap.rx_lines),
                     static_cast<unsigned>(snap.rx_lines_total),
                     static_cast<unsigned>(snap.rx_quality));
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "SSTV_RX", rx_status);
            return;
        }
        if (strncasecmp(command.value, "RX,", 3u) == 0) {
            const char *arg = command.value + 3u;
            if (stringEqualsIgnoreCase(arg, "OFF")) {
                const bool was = SSTV_SERVICE_StopRx();
                appendKeyValueLine(result->payload, sizeof(result->payload),
                                   &result->payload_size, "SSTV_RX", was ? "OFF" : "IDLE");
                return;
            }
            SstvRxSource source = SSTV_SOURCE_MIC;
            if (stringEqualsIgnoreCase(arg, "MIC")) {
                source = SSTV_SOURCE_MIC;
            } else if (stringEqualsIgnoreCase(arg, "NRL")) {
                source = SSTV_SOURCE_NRL;
            } else {
                appendKeyValueLine(result->payload, sizeof(result->payload),
                                   &result->payload_size, "ERR", "SSTV_RX");
                return;
            }
            const bool ok = SSTV_SERVICE_StartRx(source);
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "SSTV_RX",
                               ok ? (source == SSTV_SOURCE_MIC ? "MIC" : "NRL") : "BUSY");
            return;
        }
        if (stringEqualsIgnoreCase(command.value, "STOP")) {
            const bool stopped = SSTV_SERVICE_Stop();
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "SSTV", stopped ? "STOPPED" : "IDLE");
            return;
        }
        const char *comma = strchr(command.value, ',');
        if (comma == nullptr || comma == command.value || comma[1] == '\0') {
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "ERR", "SSTV");
            return;
        }
        SSTV_Mode mode = SSTV_MODE_ROBOT36;
        const size_t mode_len = static_cast<size_t>(comma - command.value);
        if (mode_len == 7u && strncasecmp(command.value, "ROBOT36", 7u) == 0) {
            mode = SSTV_MODE_ROBOT36;
        } else if ((mode_len == 7u && strncasecmp(command.value, "MARTIN1", 7u) == 0) ||
                   (mode_len == 9u && strncasecmp(command.value, "MARTIN_M1", 9u) == 0)) {
            mode = SSTV_MODE_MARTIN_M1;
        } else if (mode_len == 5u && strncasecmp(command.value, "PD120", 5u) == 0) {
            mode = SSTV_MODE_PD120;
        } else {
            appendKeyValueLine(result->payload, sizeof(result->payload),
                               &result->payload_size, "ERR", "SSTV_MODE");
            return;
        }
        const bool started = SSTV_SERVICE_SendJpeg(comma + 1, mode);
        appendKeyValueLine(result->payload, sizeof(result->payload),
                           &result->payload_size, "SSTV", started ? "STARTED" : "BUSY");
        return;
    }

    ESP_LOGI(TAG, "unknown AT command: %s=%s, returning command list",
             command.command,
             command.value);
    if (!appendKeyValueLine(result->payload, sizeof(result->payload), &result->payload_size, "ERR", "UNKNOWN") ||
        !appendSupportedAtList(result->payload, sizeof(result->payload), &result->payload_size)) {
        result->should_reply = false;
    }
}
