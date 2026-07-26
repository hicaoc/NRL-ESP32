// Improv Serial protocol implementation for ESP Web Tools detection and
// WiFi provisioning over the USB/UART console.
// Spec: https://www.improv-wifi.com/serial/

#include "improv_protocol.h"
#include "nrl_usb_console.h"
#include "nrl_wifi.h"
#include "nrl_version.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_chip_info.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

#include "../app/driver/external_radio.h"

static const char *TAG = "IMPROV";

namespace {

// --- Protocol constants -------------------------------------------------------
constexpr uint8_t kMagic[] = {'I', 'M', 'P', 'R', 'O', 'V'};
constexpr size_t kMagicLen = 6;
constexpr uint8_t kVersion = 1;

// RPC command types (client → device)
constexpr uint8_t kCmdWifiSettings = 0x01;
constexpr uint8_t kCmdRequestState = 0x02;
constexpr uint8_t kCmdRequestInfo = 0x03;
constexpr uint8_t kCmdRequestScan = 0x04;

// Response types (device → client)
constexpr uint8_t kTypeRpcResponse = 0x01;
constexpr uint8_t kTypeProvisioningState = 0x02;
constexpr uint8_t kTypeErrorResponse = 0x03;

// Provisioning states
constexpr uint8_t kStateStopped = 0x00;
constexpr uint8_t kStateProvisioning = 0x02;
constexpr uint8_t kStateProvisioned = 0x03;

// Errors
constexpr uint8_t kErrUnknownRpc = 0x01;
constexpr uint8_t kErrUnableToConnect = 0x02;

constexpr size_t kFrameBufSize = 160;

// --- Parser state machine -----------------------------------------------------
enum class ParseState : uint8_t { Idle, Magic, Version, Type, Length, Data, Checksum };

ParseState s_state = ParseState::Idle;
uint8_t s_buf[kFrameBufSize];
size_t s_magic_idx = 0;
size_t s_data_len = 0;
size_t s_data_idx = 0;
uint32_t s_last_byte_ms = 0;

// --- Frame TX -----------------------------------------------------------------
void sendFrame(uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t frame[kFrameBufSize];
    if (len + kMagicLen + 5 > sizeof(frame)) return;

    size_t pos = 0;
    memcpy(frame, kMagic, kMagicLen);
    pos += kMagicLen;
    frame[pos++] = kVersion;
    frame[pos++] = type;
    frame[pos++] = static_cast<uint8_t>(len);
    if (len > 0 && data != nullptr) {
        memcpy(frame + pos, data, len);
        pos += len;
    }
    uint8_t cs = 0;
    for (size_t i = 0; i < pos; ++i) cs ^= frame[i];
    frame[pos++] = cs;

    NRL_USB_Console_Write(frame, pos);
}

void sendState(uint8_t state)
{
    sendFrame(kTypeProvisioningState, &state, 1);
}

void sendError(uint8_t error)
{
    sendFrame(kTypeErrorResponse, &error, 1);
}

// Append a length-prefixed string to buf; returns new offset.
size_t appendStr(uint8_t *buf, size_t off, const char *str)
{
    const size_t len = strlen(str);
    buf[off++] = static_cast<uint8_t>(len);
    memcpy(buf + off, str, len);
    return off + len;
}

// --- Command handlers ---------------------------------------------------------
void handleRequestState()
{
    sendState(wifiIsConnected() ? kStateProvisioned : kStateStopped);
}

void handleRequestInfo()
{
    uint8_t data[128];
    size_t off = 0;
    data[off++] = kCmdRequestInfo; // echo command

    off = appendStr(data, off, "nrl-esp32");           // firmware variant
    off = appendStr(data, off, NRL_FIRMWARE_VERSION);   // firmware version

    // Chip family as string
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    char family[16];
    snprintf(family, sizeof(family), "ESP32-%s",
             chip.model == CHIP_ESP32S3 ? "S3" :
             chip.model == CHIP_ESP32S2 ? "S2" :
             chip.model == CHIP_ESP32C3 ? "C3" : "XX");
    off = appendStr(data, off, family);

    // MAC address (AA:BB:CC:DD:EE:FF)
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    off = appendStr(data, off, mac_str);

    // Board name
    off = appendStr(data, off, NRL_FIRMWARE_NAME);

    sendFrame(kTypeRpcResponse, data, off);
}

void handleWifiSettings(const uint8_t *data, size_t len)
{
    // Format: [ssid_len(1)] [ssid(N)] [pass_len(1)] [pass(M)]
    if (len < 2) { sendError(kErrUnknownRpc); return; }

    size_t pos = 0;
    const uint8_t ssid_len = data[pos++];
    if (pos + ssid_len > len) { sendError(kErrUnknownRpc); return; }
    char ssid[33] = {};
    memcpy(ssid, data + pos, ssid_len < 32 ? ssid_len : 32);
    pos += ssid_len;

    char pass[65] = {};
    if (pos < len) {
        const uint8_t pass_len = data[pos++];
        if (pos + pass_len <= len) {
            memcpy(pass, data + pos, pass_len < 64 ? pass_len : 64);
        }
    }

    ESP_LOGI(TAG, "WiFi settings received: SSID=\"%s\"", ssid);

    // Persist credentials and enable WiFi
    EXTERNAL_RADIO_SetWifiSsid(ssid, false);
    EXTERNAL_RADIO_SetWifiPassword(pass, false);
    EXTERNAL_RADIO_SetWifiEnabled(true, true);

    sendState(kStateProvisioning);

    // Attempt connection (blocks up to 10 s; contains internal yields)
    const WifiConnResult result = wifiEnsureConnected(ssid, pass, 10000u);
    if (result == WIFI_CONN_OK || result == WIFI_ALREADY_ON_SSID) {
        uint8_t resp[64];
        size_t off = 0;
        resp[off++] = kCmdWifiSettings;
        off = appendStr(resp, off, "OK");
        sendFrame(kTypeRpcResponse, resp, off);
        sendState(kStateProvisioned);
        ESP_LOGI(TAG, "WiFi connected via Improv");
    } else {
        sendError(kErrUnableToConnect);
        sendState(kStateStopped);
        ESP_LOGW(TAG, "WiFi connect failed via Improv (%d)", (int)result);
    }
}

void handleScanRequest()
{
    // Blocking scan (yields internally); max 4 s to keep the console alive.
    nrlWifiScanStartBlocking(4000u);
    NrlWifiScanResult results[16];
    const size_t count = nrlWifiScanGetCache(results, 16);

    for (size_t i = 0; i < count; ++i) {
        if (results[i].ssid[0] == '\0') continue;
        uint8_t resp[48];
        size_t off = 0;
        resp[off++] = kCmdRequestScan;
        off = appendStr(resp, off, results[i].ssid);
        sendFrame(kTypeRpcResponse, resp, off);
    }
    // Empty response signals end of scan results
    uint8_t end[1] = {kCmdRequestScan};
    sendFrame(kTypeRpcResponse, end, 1);
}

// --- Frame dispatch -----------------------------------------------------------
void processFrame(uint8_t type, const uint8_t *data, size_t len)
{
    if (type != kTypeRpcResponse || len < 1) return;

    const uint8_t cmd = data[0];
    const uint8_t *payload = data + 1;
    const size_t payload_len = len - 1;

    switch (cmd) {
    case kCmdRequestState: handleRequestState(); break;
    case kCmdRequestInfo:  handleRequestInfo(); break;
    case kCmdRequestScan:  handleScanRequest(); break;
    case kCmdWifiSettings: handleWifiSettings(payload, payload_len); break;
    default:
        ESP_LOGW(TAG, "unknown RPC command 0x%02X", cmd);
        sendError(kErrUnknownRpc);
        break;
    }
}

} // namespace

// --- Public API ----------------------------------------------------------------
extern "C" bool IMPROV_ProcessByte(uint8_t byte)
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

    // Inter-byte timeout: reset if the frame stalls (e.g. false 'I' trigger).
    if (s_state != ParseState::Idle && (now - s_last_byte_ms) > 200u) {
        s_state = ParseState::Idle;
        s_magic_idx = 0;
    }
    s_last_byte_ms = now;

    switch (s_state) {
    case ParseState::Idle:
        if (byte == kMagic[0]) {
            s_magic_idx = 1;
            s_state = ParseState::Magic;
            return true;
        }
        return false;

    case ParseState::Magic:
        if (byte == kMagic[s_magic_idx]) {
            if (++s_magic_idx >= kMagicLen) s_state = ParseState::Version;
            return true;
        }
        // False trigger – reset. Re-check byte as a potential new start.
        s_state = (byte == kMagic[0]) ? ParseState::Magic : ParseState::Idle;
        s_magic_idx = (byte == kMagic[0]) ? 1u : 0u;
        return true;

    case ParseState::Version:
        s_buf[0] = byte; // version (expected: 1)
        s_state = ParseState::Type;
        return true;

    case ParseState::Type:
        s_buf[1] = byte;
        s_state = ParseState::Length;
        return true;

    case ParseState::Length:
        s_data_len = byte;
        s_data_idx = 0;
        if (s_data_len == 0 || s_data_len > kFrameBufSize - 2) {
            s_state = ParseState::Checksum;
        } else {
            s_state = ParseState::Data;
        }
        return true;

    case ParseState::Data:
        s_buf[2 + s_data_idx] = byte;
        if (++s_data_idx >= s_data_len) s_state = ParseState::Checksum;
        return true;

    case ParseState::Checksum: {
        // Verify XOR over magic + version + type + length + data
        uint8_t cs = 0;
        for (size_t i = 0; i < kMagicLen; ++i) cs ^= kMagic[i];
        cs ^= s_buf[0]; // version
        cs ^= s_buf[1]; // type
        cs ^= static_cast<uint8_t>(s_data_len);
        for (size_t i = 0; i < s_data_len; ++i) cs ^= s_buf[2 + i];

        s_state = ParseState::Idle;
        s_magic_idx = 0;

        if (byte != cs) {
            ESP_LOGW(TAG, "checksum mismatch (got 0x%02X want 0x%02X)", byte, cs);
            return true;
        }
        processFrame(s_buf[1], s_buf + 2, s_data_len);
        return true;
    }
    }
    return false;
}
