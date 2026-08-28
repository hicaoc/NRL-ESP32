// Improv Serial protocol implementation for ESP Web Tools detection and
// WiFi provisioning over the USB/UART console.
// Spec: https://www.improv-wifi.com/serial/

#include "improv_protocol.h"
#include "nrl_usb_console.h"
#include "nrl_wifi.h"
#include "nrl_net_compat.h"
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

// --- Protocol constants (per https://www.improv-wifi.com/serial/) -------------
constexpr uint8_t kMagic[] = {'I', 'M', 'P', 'R', 'O', 'V'};
constexpr size_t kMagicLen = 6;
constexpr uint8_t kVersion = 1;

// Packet types
constexpr uint8_t kTypeCurrentState = 0x01;  // device → client
constexpr uint8_t kTypeErrorState   = 0x02;  // device → client
constexpr uint8_t kTypeRpcCommand   = 0x03;  // client → device
constexpr uint8_t kTypeRpcResult    = 0x04;  // device → client

// RPC command IDs (first byte of RPC command/result data)
constexpr uint8_t kCmdWifiSettings  = 0x01;
constexpr uint8_t kCmdRequestState  = 0x02;
constexpr uint8_t kCmdRequestInfo   = 0x03;
constexpr uint8_t kCmdRequestScan   = 0x04;

// Current state values
constexpr uint8_t kStateAuthorized   = 0x02;  // ready to accept credentials
constexpr uint8_t kStateProvisioning = 0x03;
constexpr uint8_t kStateProvisioned  = 0x04;

// Error state values
constexpr uint8_t kErrNone            = 0x00;
constexpr uint8_t kErrInvalidRpc      = 0x01;
constexpr uint8_t kErrUnknownRpc      = 0x02;
constexpr uint8_t kErrUnableToConnect = 0x03;

constexpr size_t kFrameBufSize = 160;

// --- Parser state machine -----------------------------------------------------
enum class ParseState : uint8_t { Idle, Magic, Version, Type, Length, Data, Checksum };

ParseState s_state = ParseState::Idle;
uint8_t s_buf[kFrameBufSize];
size_t s_magic_idx = 0;
size_t s_data_len = 0;
size_t s_data_idx = 0;
uint32_t s_last_byte_ms = 0;

// False-trigger replay: bytes swallowed while probing for the "IMPROV" magic
// that turned out to be plain AT text (e.g. the 'I' in "AT+RADIO=?"). They are
// handed back to the caller via IMPROV_ReadRejected() so the AT parser sees
// the original byte stream.
uint8_t s_pending[kMagicLen];
size_t s_pending_len = 0;
uint8_t s_reject[kMagicLen + 1u];
size_t s_reject_len = 0;

void rejectByte(uint8_t byte)
{
    if (s_reject_len < sizeof(s_reject)) {
        s_reject[s_reject_len++] = byte;
    } else {
        ESP_LOGW(TAG, "reject buffer full, byte dropped");
    }
}

void flushPendingToReject()
{
    for (size_t i = 0; i < s_pending_len; ++i) rejectByte(s_pending[i]);
    s_pending_len = 0;
    s_magic_idx = 0;
}

// --- Frame TX -----------------------------------------------------------------
void sendFrame(uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t frame[kFrameBufSize];
    // magic(6) + version + type + length + data + checksum + newline
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
    // Checksum: simple sum of all preceding bytes, LSB only.
    uint8_t cs = 0;
    for (size_t i = 0; i < pos; ++i) cs += frame[i];
    frame[pos++] = cs;
    frame[pos++] = '\n';

    const size_t wrote = NRL_USB_Console_Write(frame, pos);
    ESP_LOGI(TAG, "TX frame type=0x%02X len=%u wrote=%u", type, (unsigned)pos, (unsigned)wrote);
}

void sendState(uint8_t state)
{
    sendFrame(kTypeCurrentState, &state, 1);
}

void sendError(uint8_t error)
{
    sendFrame(kTypeErrorState, &error, 1);
}

// Append a length-prefixed string to buf; returns new offset.
size_t appendStr(uint8_t *buf, size_t off, const char *str)
{
    const size_t len = strlen(str);
    buf[off++] = static_cast<uint8_t>(len);
    memcpy(buf + off, str, len);
    return off + len;
}

// Build an RPC Result payload into buf: [command][data length][len-prefixed
// strings...]. Returns total payload size (0 on overflow).
size_t buildRpcResult(uint8_t *buf, size_t cap, uint8_t cmd,
                      const char *const *strings, size_t count)
{
    if (cap < 2) return 0;
    size_t off = 0;
    buf[off++] = cmd;
    const size_t len_pos = off++;
    for (size_t i = 0; i < count; ++i) {
        const size_t slen = strlen(strings[i]);
        if (off + 1 + slen > cap || slen > 255) return 0;
        off = appendStr(buf, off, strings[i]);
    }
    buf[len_pos] = static_cast<uint8_t>(off - 2);
    return off;
}

// Best-effort device URL from the STA IP ("http://a.b.c.d").
void deviceUrl(char *buf, size_t size)
{
    char ip[16] = {};
    nrlIpToString(nrlWifiStaIp(), ip, sizeof(ip));
    snprintf(buf, size, "http://%s", ip);
}

// --- Command handlers ---------------------------------------------------------
void handleRequestState()
{
    const bool connected = wifiIsConnected();
    sendState(connected ? kStateProvisioned : kStateAuthorized);
    if (connected) {
        // A provisioned device also returns the provisioning-success response.
        char url[32];
        deviceUrl(url, sizeof(url));
        const char *strings[] = {url};
        uint8_t data[40];
        const size_t n = buildRpcResult(data, sizeof(data), kCmdRequestState, strings, 1);
        if (n > 0) sendFrame(kTypeRpcResult, data, n);
    }
}

void handleRequestInfo()
{
    // Chip family as string
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    char family[16];
    snprintf(family, sizeof(family), "ESP32-%s",
             chip.model == CHIP_ESP32S3 ? "S3" :
             chip.model == CHIP_ESP32S2 ? "S2" :
             chip.model == CHIP_ESP32C3 ? "C3" : "XX");

    // Device name: board name + MAC suffix so units are distinguishable
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[32];
    snprintf(name, sizeof(name), "%s-%02X%02X%02X",
             NRL_FIRMWARE_NAME, mac[3], mac[4], mac[5]);

    // Spec order: firmware name, firmware version, chip/variant, device name
    const char *strings[] = {"nrl-esp32", NRL_FIRMWARE_VERSION, family, name};
    uint8_t data[96];
    const size_t n = buildRpcResult(data, sizeof(data), kCmdRequestInfo, strings, 4);
    if (n > 0) sendFrame(kTypeRpcResult, data, n);
}

void handleWifiSettings(const uint8_t *data, size_t len)
{
    // Format: [ssid_len(1)] [ssid(N)] [pass_len(1)] [pass(M)]
    if (len < 2) { sendError(kErrInvalidRpc); return; }

    size_t pos = 0;
    const uint8_t ssid_len = data[pos++];
    if (pos + ssid_len > len) { sendError(kErrInvalidRpc); return; }
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
        // First result string is the URL to redirect the user to.
        char url[32];
        deviceUrl(url, sizeof(url));
        const char *strings[] = {url};
        uint8_t resp[40];
        const size_t n = buildRpcResult(resp, sizeof(resp), kCmdWifiSettings, strings, 1);
        if (n > 0) sendFrame(kTypeRpcResult, resp, n);
        sendState(kStateProvisioned);
        ESP_LOGI(TAG, "WiFi connected via Improv");
    } else {
        sendError(kErrUnableToConnect);
        sendState(kStateAuthorized);
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
        // Spec order: SSID, RSSI (decimal string), auth required (YES/NO)
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", (int)results[i].rssi);
        const char *strings[] = {results[i].ssid, rssi, results[i].secured ? "YES" : "NO"};
        uint8_t resp[48];
        const size_t n = buildRpcResult(resp, sizeof(resp), kCmdRequestScan, strings, 3);
        if (n > 0) sendFrame(kTypeRpcResult, resp, n);
    }
    // Response with 0 strings signals end of scan results
    uint8_t end[2];
    const size_t n = buildRpcResult(end, sizeof(end), kCmdRequestScan, nullptr, 0);
    if (n > 0) sendFrame(kTypeRpcResult, end, n);
}

// --- Frame dispatch -----------------------------------------------------------
void processFrame(uint8_t type, const uint8_t *data, size_t len)
{
    // RPC command data: [command][data length][data...]
    if (type != kTypeRpcCommand || len < 2) {
        sendError(kErrInvalidRpc);
        return;
    }

    // Per spec, clear the error state when an RPC command is received.
    sendError(kErrNone);

    const uint8_t cmd = data[0];
    const uint8_t *payload = data + 2;
    size_t payload_len = len - 2;
    const uint8_t declared_len = data[1];
    if (declared_len < payload_len) payload_len = declared_len;

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
        // A stalled partial magic was plain text; give those bytes back.
        if (s_state == ParseState::Magic) flushPendingToReject();
        s_state = ParseState::Idle;
        s_magic_idx = 0;
    }
    s_last_byte_ms = now;

    switch (s_state) {
    case ParseState::Idle:
        if (byte == kMagic[0]) {
            s_pending[0] = byte;
            s_pending_len = 1;
            s_magic_idx = 1;
            s_state = ParseState::Magic;
            return true;
        }
        return false;

    case ParseState::Magic:
        if (byte == kMagic[s_magic_idx]) {
            s_pending[s_pending_len++] = byte;
            if (++s_magic_idx >= kMagicLen) {
                s_state = ParseState::Version;
                s_pending_len = 0;
            }
            return true;
        }
        // False trigger – replay the consumed prefix, then re-check this byte
        // as a potential new magic start.
        flushPendingToReject();
        s_state = ParseState::Idle;
        if (byte == kMagic[0]) {
            s_pending[0] = byte;
            s_pending_len = 1;
            s_magic_idx = 1;
            s_state = ParseState::Magic;
        } else {
            rejectByte(byte);
        }
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
        // Verify simple-sum checksum over magic + version + type + length + data
        uint8_t cs = 0;
        for (size_t i = 0; i < kMagicLen; ++i) cs += kMagic[i];
        cs += s_buf[0]; // version
        cs += s_buf[1]; // type
        cs += static_cast<uint8_t>(s_data_len);
        for (size_t i = 0; i < s_data_len; ++i) cs += s_buf[2 + i];

        s_state = ParseState::Idle;
        s_magic_idx = 0;

        if (byte != cs) {
            ESP_LOGW(TAG, "checksum mismatch (got 0x%02X want 0x%02X)", byte, cs);
            return true;
        }
        if (s_buf[0] != kVersion) {
            ESP_LOGW(TAG, "unsupported version 0x%02X", s_buf[0]);
            return true;
        }
        ESP_LOGI(TAG, "RX frame type=0x%02X len=%u cmd=0x%02X", s_buf[1], (unsigned)s_data_len, s_buf[2]);
        processFrame(s_buf[1], s_buf + 2, s_data_len);
        return true;
    }
    }
    return false;
}

extern "C" int IMPROV_ReadRejected(void)
{
    if (s_reject_len == 0) return -1;
    const uint8_t byte = s_reject[0];
    memmove(s_reject, s_reject + 1, --s_reject_len);
    return byte;
}
