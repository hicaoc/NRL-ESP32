#include "wifi_config_portal.h"
#include "wifi_config_portal_view.h"
#include "wifi_portal_assets.generated.h"
#include "nrl_audio_bridge.h"
#include "nrl_at_commands.h"
#include "nrl_captive_dns.h"
#include "nrl_net_compat.h"
#include "nrl_psram.h"
#include "nrl_version.h"
#include "nrl_wifi.h"
#include "../services/ota_service.h"

#include "../app/driver/es8311.h"
#include "../app/driver/external_radio.h"
#include "../app/driver/gps_serial.h"
#include "../app/driver/sci_serial.h"
#include "../app/driver/serial_port_config.h"
#include "../app/driver/board_pins.h"
#include "../app/driver/display.h"
#include "../app/driver/environment_sensors.h"
#include "../app/driver/i2c_device_discovery.h"
#include "../app/driver/bh4tdv_rf_io.h"
#include "../app/driver/sr110u.h"
#include "../services/radio_config.h"
#include "../services/ai_assistant.h"
#include "../services/aprs_service.h"
#include "../services/signaling_service.h"
#include "../services/display_notice.h"
#include "../services/espnow_link.h"
#include "../services/fmo_cert_store.h"
#include "../services/fmo_service.h"
#include "../services/fmo_station_broadcast.h"
#include "../services/fmo_station_broadcast_core.h"
#include "../services/fmo_qso.h"
#include "../services/music_player.h"
#include "../services/music_playlist.h"
#include "../services/nanny.h"
#include "../services/radio_favorites.h"
#include "../services/server_list_store.h"
#include "../services/storage_service.h"

#include <cJSON.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <math.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/sockets.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

static const char *TAG = "CFG";

namespace {

// ----------------------------------------------------------------------------
// FormBody + PortalRequest adapter
//
// The handler functions used to call `s_server.arg("foo")` etc. against
// Arduino's `WebServer`. With esp_http_server we pre-parse the POST body once
// at handler entry and expose the parsed key/value pairs through a small
// adapter that mimics the WebServer accessor surface so handler bodies don't
// have to change. Default esp_http_server config runs one handler task at a
// time, so a single global PortalRequest is safe.
// ----------------------------------------------------------------------------

constexpr size_t kFormParamMax = 16u;
constexpr size_t kFormKeyMax = 48u;
constexpr size_t kFormValueMax = 768u;

struct FormParam {
    char key[kFormKeyMax];
    char value[kFormValueMax];
};

struct FormBody {
    FormParam params[kFormParamMax];
    size_t count;
};

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// In-place URL decode (handles %XX and '+'). Returns the new length.
static size_t urlDecodeInPlace(char *s)
{
    if (s == nullptr) {
        return 0u;
    }
    char *out = s;
    for (char *in = s; *in != '\0'; ) {
        if (*in == '+') {
            *out++ = ' ';
            ++in;
        } else if (*in == '%' && in[1] != '\0' && in[2] != '\0') {
            const int hi = hexDigit(in[1]);
            const int lo = hexDigit(in[2]);
            if (hi >= 0 && lo >= 0) {
                *out++ = static_cast<char>((hi << 4) | lo);
                in += 3;
            } else {
                *out++ = *in++;
            }
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
    return static_cast<size_t>(out - s);
}

// Read the entire request body and parse application/x-www-form-urlencoded.
// Returns true on success. Body must fit in heap (up to ~4 KB form bodies are
// typical for the audio config page).
static bool parseFormBody(httpd_req_t *req, FormBody &out)
{
    out.count = 0;
    const size_t total = req->content_len;
    if (total == 0) {
        return true;
    }
    if (total > 8192u) {
        ESP_LOGW(TAG, "form body too large (%u bytes), refusing", static_cast<unsigned>(total));
        return false;
    }
    char *buf = static_cast<char *>(malloc(total + 1u));
    if (buf == nullptr) {
        return false;
    }
    size_t got = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) {
            free(buf);
            return false;
        }
        got += static_cast<size_t>(n);
    }
    buf[got] = '\0';

    char *cursor = buf;
    while (cursor < buf + got && out.count < kFormParamMax) {
        char *amp = strchr(cursor, '&');
        char *eq = strchr(cursor, '=');
        if (amp != nullptr) {
            *amp = '\0';
        }
        if (eq != nullptr && (amp == nullptr || eq < amp)) {
            *eq = '\0';
            FormParam &p = out.params[out.count];
            const size_t key_len = strlen(cursor);
            const size_t val_len = strlen(eq + 1);
            if (key_len < kFormKeyMax && val_len < kFormValueMax) {
                memcpy(p.key, cursor, key_len + 1u);
                memcpy(p.value, eq + 1, val_len + 1u);
                urlDecodeInPlace(p.key);
                urlDecodeInPlace(p.value);
                ++out.count;
            }
        } else if (cursor[0] != '\0') {
            // Bare key with no '=' sign.
            FormParam &p = out.params[out.count];
            const size_t key_len = strlen(cursor);
            if (key_len < kFormKeyMax) {
                memcpy(p.key, cursor, key_len + 1u);
                p.value[0] = '\0';
                urlDecodeInPlace(p.key);
                ++out.count;
            }
        }
        if (amp == nullptr) {
            break;
        }
        cursor = amp + 1;
    }
    free(buf);
    return true;
}

static const FormParam *formFindParam(const FormBody &body, const char *key)
{
    if (key == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < body.count; ++i) {
        if (strcmp(body.params[i].key, key) == 0) {
            return &body.params[i];
        }
    }
    return nullptr;
}

// Mimics arduino-esp32 WebServer's accessor surface so handler bodies that
// were written against `s_server.arg/hasArg/send/sendContent/...` keep
// compiling unchanged. Stores the current httpd_req_t and the parsed body.
struct PortalRequest {
    httpd_req_t *req = nullptr;
    FormBody body{};
    char ct_header[96] = {};

    void bind(httpd_req_t *r)
    {
        req = r;
        body.count = 0;
        ct_header[0] = '\0';
        if (r != nullptr) {
            httpd_req_get_hdr_value_str(r, "Content-Type", ct_header, sizeof(ct_header));
        }
    }

    bool bindPost(httpd_req_t *r)
    {
        bind(r);
        return parseFormBody(r, body);
    }

    std::string arg(const char *name) const
    {
        const FormParam *p = formFindParam(body, name);
        return (p != nullptr) ? std::string(p->value) : std::string();
    }
    bool hasArg(const char *name) const { return formFindParam(body, name) != nullptr; }
    int args() const { return static_cast<int>(body.count); }
    std::string argName(int i) const
    {
        return (i >= 0 && static_cast<size_t>(i) < body.count) ? std::string(body.params[i].key) : std::string();
    }
    std::string header(const char *name) const
    {
        if (name != nullptr && strcasecmp(name, "Content-Type") == 0) {
            return std::string(ct_header);
        }
        char buf[160] = {};
        if (req != nullptr && name != nullptr) {
            httpd_req_get_hdr_value_str(req, name, buf, sizeof(buf));
        }
        return std::string(buf);
    }
    size_t clientContentLength() const { return req != nullptr ? req->content_len : 0u; }

    void setContentLength(size_t /*unused*/) {}  // chunked encoding handled by httpd_resp_send_chunk

    void sendHeader(const char *name, const char *value, bool /*first*/ = false) const
    {
        if (req != nullptr) {
            httpd_resp_set_hdr(req, name, value);
        }
    }

    void send(int code, const char *content_type, const std::string &body_text) const
    {
        sendBytes(code, content_type, body_text.c_str(), body_text.size());
    }
    void send(int code, const char *content_type, const char *body_text) const
    {
        sendBytes(code, content_type, body_text != nullptr ? body_text : "",
                  body_text != nullptr ? strlen(body_text) : 0u);
    }

    void sendContent(const char *data, size_t n) const
    {
        if (req != nullptr && n > 0u) {
            httpd_resp_send_chunk(req, data, n);
        }
    }
    void sendContent(const char *data) const
    {
        if (req == nullptr) {
            return;
        }
        if (data != nullptr && *data != '\0') {
            httpd_resp_send_chunk(req, data, strlen(data));
        } else {
            httpd_resp_send_chunk(req, nullptr, 0);  // end marker
        }
    }

private:
    void sendBytes(int code, const char *ct, const char *body_text, size_t body_len) const
    {
        if (req == nullptr) {
            return;
        }
        char status[32];
        const char *reason = "OK";
        if (code == 302) reason = "Found";
        else if (code == 204) reason = "No Content";
        else if (code == 400) reason = "Bad Request";
        else if (code == 404) reason = "Not Found";
        else if (code == 500) reason = "Internal Server Error";
        snprintf(status, sizeof(status), "%d %s", code, reason);
        httpd_resp_set_status(req, status);
        if (ct != nullptr && *ct != '\0') {
            httpd_resp_set_type(req, ct);
        }
        httpd_resp_send(req, body_text, body_len);
    }
};

// 13 KB of form buffers; only touched by the httpd handler task, so keep it
// in PSRAM instead of the scarce internal heap.
NRL_PSRAM_BSS PortalRequest s_server;
httpd_handle_t s_httpd = nullptr;
bool s_server_started = false;
bool s_dns_started = false;
bool s_ap_started = false;
bool s_ap_should_run = true;
// Backs off AP/WiFi-stack bring-up retries: a failed esp_wifi_init is almost
// always low internal RAM, and hammering it every poll only floods the log.
unsigned long s_ap_retry_at_ms = 0UL;
constexpr unsigned long kApRetryBackoffMs = 5000UL;
bool s_sta_was_connected = false;
bool s_ap_close_scheduled = false;
unsigned long s_ap_close_at_ms = 0UL;
unsigned long s_sta_disconnect_started_ms = 0UL;
bool s_boot_pressed = false;
bool s_update_reboot_pending = false;
unsigned long s_boot_press_started_ms = 0UL;
unsigned long s_update_reboot_at_ms = 0UL;

static inline unsigned long nowMsCfg()
{
    return static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
}

// Cached WiFi scan results. The scan runs once before the config AP starts
// (no portal client connected yet, so the channel-hopping scan disturbs
// nobody); the portal then serves this cache and never does a live scan,
// which would otherwise drop the connected client.
const size_t kWifiScanCacheMax = 24u;
WifiConfigPortalScanEntry s_wifi_scan_cache[kWifiScanCacheMax];
int s_wifi_scan_count = 0;
bool s_wifi_prescan_done = false;

constexpr unsigned long kBootResetHoldMs = 5000UL;
constexpr unsigned long kApCloseDelayMs = 5000UL;
constexpr unsigned long kApReopenAfterStaDownMs = 60000UL;
constexpr unsigned long kWifiPrescanTimeoutMs = 12000UL;
constexpr unsigned long kDacEqCoefficientMax = 1073741823UL;
constexpr uint8_t kApChannel = 1;
constexpr uint8_t kApMaxConn = 4;
constexpr uint32_t kApIp = NRL_IPV4(192, 168, 4, 1);
constexpr uint32_t kApGateway = NRL_IPV4(192, 168, 4, 1);
constexpr uint32_t kApSubnet = NRL_IPV4(255, 255, 255, 0);

static std::string jsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += ch;     break;
        }
    }
    return out;
}

// Mask secret fields (e.g. WiFi password) for serial logs: show length only,
// never the plaintext.
static std::string maskSecret(const char *text)
{
    if (text == nullptr || *text == '\0') {
        return std::string("(empty)");
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "****** (%u chars)", static_cast<unsigned>(strlen(text)));
    return std::string(buf);
}

static std::string ipToString(const uint32_t value)
{
    char buf[16] = {};
    nrlIpToString(value, buf, sizeof(buf));
    return std::string(buf);
}

static std::string buildApSsid()
{
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    const uint32_t tail = (static_cast<uint32_t>(mac[3]) << 16) |
                          (static_cast<uint32_t>(mac[4]) << 8) |
                          mac[5];
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "NRL-ESP32-%06lX",
             static_cast<unsigned long>(tail));
    return std::string(buffer);
}

static void redirectToPortal()
{
    s_server.sendHeader("Location", "/");
    s_server.send(302, "text/plain", "");
}

static esp_err_t handleFavicon(httpd_req_t *req)
{
    s_server.bind(req);
    s_server.sendHeader("Cache-Control", "max-age=86400");
    s_server.send(204, "image/x-icon", "");
    return ESP_OK;
}

// Scan nearby WiFi and store the result in s_wifi_scan_cache.
static void performWifiPrescan()
{
    ESP_LOGI(TAG, "pre-scanning WiFi before AP start...");
    s_wifi_scan_count = 0;
    if (!nrlWifiScanStartBlocking(static_cast<uint32_t>(kWifiPrescanTimeoutMs))) {
        ESP_LOGW(TAG, "WiFi pre-scan failed");
        return;
    }

    NrlWifiScanResult results[kWifiScanCacheMax];
    const size_t got = nrlWifiScanGetCache(results, kWifiScanCacheMax);
    for (size_t i = 0; i < got && static_cast<size_t>(s_wifi_scan_count) < kWifiScanCacheMax; ++i) {
        if (results[i].ssid[0] == '\0') {
            continue;
        }
        s_wifi_scan_cache[s_wifi_scan_count].ssid = results[i].ssid;
        s_wifi_scan_cache[s_wifi_scan_count].rssi = results[i].rssi;
        ++s_wifi_scan_count;
    }
    ESP_LOGI(TAG, "pre-scan cached %d WiFi networks", s_wifi_scan_count);
}

static void ensureApRunning()
{
    if (!s_ap_should_run || s_ap_started) {
        return;
    }
    if (s_ap_retry_at_ms != 0UL &&
        static_cast<long>(nowMsCfg() - s_ap_retry_at_ms) < 0L) {
        return;
    }

    // Make sure the WiFi stack is up before we scan or open the AP.
    if (!nrlWifiInit()) {
        s_ap_retry_at_ms = nowMsCfg() + kApRetryBackoffMs;
        ESP_LOGW(TAG, "WiFi stack init failed; retry in %lums "
                      "(internal free=%u largest=%u)",
                 static_cast<unsigned long>(kApRetryBackoffMs),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return;
    }

    // First time the config AP comes up: scan nearby WiFi while no portal
    // client is connected yet, and cache it. The page then shows the list
    // immediately and never needs a live scan (which would drop the client).
    if (!s_wifi_prescan_done) {
        s_wifi_prescan_done = true;
        performWifiPrescan();
    }

    const std::string ap_ssid = buildApSsid();
    const bool ap_ok = nrlWifiApStart(ap_ssid.c_str(), kApChannel, kApMaxConn,
                                      kApIp, kApGateway, kApSubnet);
    if (ap_ok) {
        s_ap_started = true;
        s_ap_retry_at_ms = 0UL;
        const size_t station_count = nrlWifiApGetStationCount();
        char ip_buf[16] = {};
        nrlIpToString(nrlWifiApIp(), ip_buf, sizeof(ip_buf));
        ESP_LOGI(TAG, "AP ready: ssid=%s open ip=%s channel=%u stations=%u",
                 ap_ssid.c_str(), ip_buf,
                 static_cast<unsigned>(kApChannel),
                 static_cast<unsigned>(station_count));
    } else {
        s_ap_retry_at_ms = nowMsCfg() + kApRetryBackoffMs;
        ESP_LOGE(TAG, "AP start failed; retry in %lums",
                 static_cast<unsigned long>(kApRetryBackoffMs));
    }
}

static void shutdownDnsAndAp()
{
    if (s_dns_started) {
        NRL_CaptiveDNS_Stop();
        s_dns_started = false;
    }
    if (s_ap_started) {
        nrlWifiApStop();
        s_ap_started = false;
        ESP_LOGI(TAG, "AP closed (STA online)");
    }
    s_ap_should_run = false;
}

static void manageApLifecycle()
{
    const bool sta_connected = nrlNetworkConnected();
    const unsigned long now = nowMsCfg();

    if (sta_connected) {
        s_sta_disconnect_started_ms = 0UL;
        if (!s_sta_was_connected) {
            s_sta_was_connected = true;
            s_ap_close_scheduled = true;
            s_ap_close_at_ms = now + kApCloseDelayMs;
            char ip_buf[16] = {};
            nrlIpToString(nrlNetworkIp(), ip_buf, sizeof(ip_buf));
            ESP_LOGI(TAG, "network connected ip=%s, AP will close in %lu ms",
                     ip_buf, static_cast<unsigned long>(kApCloseDelayMs));
        }
        if (s_ap_close_scheduled && static_cast<int32_t>(now - s_ap_close_at_ms) >= 0) {
            shutdownDnsAndAp();
            s_ap_close_scheduled = false;
        }
        return;
    }

    if (!s_sta_was_connected) {
        return;
    }

    if (s_sta_disconnect_started_ms == 0UL) {
        s_sta_disconnect_started_ms = now;
        ESP_LOGI(TAG, "STA dropped, monitoring for reopen");
        return;
    }

    if ((now - s_sta_disconnect_started_ms) >= kApReopenAfterStaDownMs) {
        s_sta_was_connected = false;
        s_ap_close_scheduled = false;
        s_sta_disconnect_started_ms = 0UL;
        s_ap_should_run = true;
        ESP_LOGI(TAG, "STA down too long, reopening config AP");
    }
}

static void pollBootResetGesture()
{
    if (NRL_PIN_BOOT_BUTTON < 0) {
        return;  // no boot button on this board (pin reused for I2C, etc.)
    }
    const bool pressed = gpio_get_level((gpio_num_t)NRL_PIN_BOOT_BUTTON) == 0;
    const unsigned long now = nowMsCfg();

    if (!pressed) {
        s_boot_pressed = false;
        s_boot_press_started_ms = 0UL;
        return;
    }

    if (!s_boot_pressed) {
        s_boot_pressed = true;
        s_boot_press_started_ms = now;
        return;
    }

    if (s_boot_press_started_ms != 0UL && (now - s_boot_press_started_ms) >= kBootResetHoldMs) {
        if (EXTERNAL_RADIO_ResetNetworkConfig()) {
            ESP_LOGI(TAG, "BOOT held 5s, network config reset to defaults");
        } else {
            ESP_LOGE(TAG, "BOOT held 5s, network config reset failed");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

static void ensureDnsRunning()
{
    if (s_dns_started || !s_ap_should_run || !s_ap_started) {
        return;
    }

    if (!NRL_CaptiveDNS_Start(nrlWifiApIp())) {
        ESP_LOGE(TAG, "captive DNS start failed");
        return;
    }
    s_dns_started = true;
}

static std::string buildNetworkSection(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildNetworkSection(config,
                                                    s_wifi_scan_cache,
                                                    static_cast<size_t>(s_wifi_scan_count));
}

static std::string buildDeviceSections(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildDeviceSections(config);
}

static std::string buildAudioSections(const ExternalRadioConfig *config)
{
    return WifiConfigPortalView_BuildAudioSections(config);
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

// Look up the canonical, just-saved value for one form field name. The save
// handler echoes these back so the client can refresh its inputs from device
// truth (post-clamp, post-sanitize) without re-rendering the whole page.
static std::string savedValueForArg(const ExternalRadioConfig *config, const std::string &name)
{
    if (config == nullptr) {
        return std::string();
    }
    if (name == "wifi_ssid") return std::string(config->wifi_ssid);
    if (name == "wifi_password") return std::string(config->wifi_password);
    if (name == "wifi_dhcp_enabled") return config->wifi_dhcp_enabled ? "1" : "0";
    if (name == "wifi_ip") return ipToString(config->wifi_ip);
    if (name == "wifi_mask") return ipToString(config->wifi_netmask);
    if (name == "wifi_gateway") return ipToString(config->wifi_gateway);
    if (name == "wifi_dns") return ipToString(config->wifi_dns);
    if (name == "server_host") return std::string(config->server_host);
    if (name == "server_port") return std::to_string(config->server_port);
    if (name == "channel") return std::to_string(config->channel);
    if (name == "callsign") return std::string(config->callsign);
    if (name == "callsign_ssid") return std::to_string(config->callsign_ssid);
    if (name == "ptt_timeout") return std::to_string(config->ptt_timeout_s);
    if (name == "voice_payload_bytes") return std::to_string(config->voice_payload_bytes);
    if (name == "tail_suppress_ms") return std::to_string(config->tail_suppress_ms);
    if (name == "battery_cal_milli") return std::to_string(config->battery_cal_milli);
    if (name == "mic_volume") return std::to_string(config->mic_volume);
    if (name == "mic_pcm_gain") return formatMicPcmGain(config->mic_pcm_gain_milli);
    if (name == "line_out_volume") return std::to_string(config->line_out_volume);
    if (name == "hp_drive_enabled") return config->hp_drive_enabled ? "1" : "0";
    if (name == "aec_enabled") return config->aec_enabled ? "1" : "0";
    if (name == "aec_reference_source") return std::to_string(config->aec_reference_source);
    if (name == "ai_noise_enabled") return config->ai_noise_enabled ? "1" : "0";
    if (name == "adc_dmic_enabled") return config->adc_dmic_enabled ? "1" : "0";
    if (name == "adc_linsel") return config->adc_linsel ? "1" : "0";
    if (name == "adc_pga_gain") return std::to_string(config->adc_pga_gain);
    if (name == "adc_ramprate") return std::to_string(config->adc_ramprate);
    if (name == "adc_scale") return std::to_string(config->adc_scale);
    if (name == "adc_dmic_sense") return config->adc_dmic_sense ? "1" : "0";
    if (name == "adc_sync") return config->adc_sync ? "1" : "0";
    if (name == "adc_inv") return config->adc_inv ? "1" : "0";
    if (name == "adc_ramclr") return config->adc_ramclr ? "1" : "0";
    if (name == "alc_enabled") return config->alc_enabled ? "1" : "0";
    if (name == "adc_automute_enabled") return config->adc_automute_enabled ? "1" : "0";
    if (name == "alc_winsize") return std::to_string(config->alc_winsize);
    if (name == "alc_maxlevel") return std::to_string(config->alc_maxlevel);
    if (name == "alc_minlevel") return std::to_string(config->alc_minlevel);
    if (name == "adc_automute_winsize") return std::to_string(config->adc_automute_winsize);
    if (name == "adc_automute_noise_gate") return std::to_string(config->adc_automute_noise_gate);
    if (name == "adc_automute_volume") return std::to_string(config->adc_automute_volume);
    if (name == "adc_hpfs1") return std::to_string(config->adc_hpfs1);
    if (name == "adc_hpfs2") return std::to_string(config->adc_hpfs2);
    if (name == "adc_eq_bypass") return config->adc_eq_bypass ? "1" : "0";
    if (name == "adc_hpf") return config->adc_hpf ? "1" : "0";
    if (name == "mic_hpf_enabled") return config->mic_hpf_enabled ? "1" : "0";
    if (name == "adceq_b0") return std::to_string(config->adceq_b0);
    if (name == "adceq_a1") return std::to_string(config->adceq_a1);
    if (name == "adceq_a2") return std::to_string(config->adceq_a2);
    if (name == "adceq_b1") return std::to_string(config->adceq_b1);
    if (name == "adceq_b2") return std::to_string(config->adceq_b2);
    if (name == "drc_enabled") return config->drc_enabled ? "1" : "0";
    if (name == "drc_winsize") return std::to_string(config->drc_winsize);
    if (name == "drc_maxlevel") return std::to_string(config->drc_maxlevel);
    if (name == "drc_minlevel") return std::to_string(config->drc_minlevel);
    if (name == "dac_ramprate") return std::to_string(config->dac_ramprate);
    if (name == "dac_eq_bypass") return config->dac_eq_bypass ? "1" : "0";
    if (name == "daceq_b0") return std::to_string(config->daceq_b0);
    if (name == "daceq_b1") return std::to_string(config->daceq_b1);
    if (name == "daceq_a1") return std::to_string(config->daceq_a1);
    return std::string();
}

// Diff the config snapshot taken before save against the live config and log
// every field whose value actually changed. Keeps the serial trace honest:
// each save line lists exactly the fields the user touched, nothing else.
static void logChangedFields(const ExternalRadioConfig *before,
                             const ExternalRadioConfig *after)
{
    if (before == nullptr || after == nullptr) {
        return;
    }
    std::string out;
    auto sep = [&out]() {
        if (!out.empty()) out += ' ';
    };
#define LOG_BOOL(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += after->field ? "1" : "0"; \
    }
#define LOG_UINT(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += std::to_string(static_cast<unsigned>(after->field)); \
    }
#define LOG_U32(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += std::to_string(static_cast<unsigned long>(after->field)); \
    }
#define LOG_IP(field) \
    if (before->field != after->field) { \
        sep(); \
        out += #field "="; \
        out += ipToString(after->field); \
    }
#define LOG_STR(field) \
    if (strcmp(before->field, after->field) != 0) { \
        sep(); \
        out += #field "=\""; \
        out += after->field; \
        out += '"'; \
    }
    LOG_STR(wifi_ssid);
    if (strcmp(before->wifi_password, after->wifi_password) != 0) {
        sep();
        out += "wifi_password=";
        out += maskSecret(after->wifi_password);
    }
    LOG_BOOL(wifi_dhcp_enabled);
    LOG_IP(wifi_ip);
    LOG_IP(wifi_netmask);
    LOG_IP(wifi_gateway);
    LOG_IP(wifi_dns);
    LOG_STR(server_host);
    LOG_UINT(server_port);
    LOG_UINT(channel);
    LOG_STR(callsign);
    LOG_UINT(callsign_ssid);
    LOG_UINT(ptt_timeout_s);
    LOG_UINT(battery_cal_milli);
    LOG_UINT(voice_payload_bytes);
    LOG_UINT(tail_suppress_ms);
    LOG_UINT(mic_volume);
    LOG_UINT(mic_pcm_gain_milli);
    LOG_UINT(line_out_volume);
    LOG_BOOL(hp_drive_enabled);
    LOG_BOOL(aec_enabled);
    LOG_BOOL(ai_noise_enabled);
    LOG_BOOL(mic_hpf_enabled);
    LOG_BOOL(drc_enabled);
    LOG_UINT(drc_winsize);
    LOG_UINT(drc_maxlevel);
    LOG_UINT(drc_minlevel);
    LOG_UINT(dac_ramprate);
    LOG_BOOL(dac_eq_bypass);
    LOG_U32(daceq_b0);
    LOG_U32(daceq_b1);
    LOG_U32(daceq_a1);
    LOG_BOOL(adc_dmic_enabled);
    LOG_BOOL(adc_linsel);
    LOG_UINT(adc_pga_gain);
    LOG_UINT(adc_ramprate);
    LOG_BOOL(adc_dmic_sense);
    LOG_BOOL(adc_sync);
    LOG_BOOL(adc_inv);
    LOG_BOOL(adc_ramclr);
    LOG_UINT(adc_scale);
    LOG_BOOL(alc_enabled);
    LOG_BOOL(adc_automute_enabled);
    LOG_UINT(alc_winsize);
    LOG_UINT(alc_maxlevel);
    LOG_UINT(alc_minlevel);
    LOG_UINT(adc_automute_winsize);
    LOG_UINT(adc_automute_noise_gate);
    LOG_UINT(adc_automute_volume);
    LOG_UINT(adc_hpfs1);
    LOG_UINT(adc_hpfs2);
    LOG_BOOL(adc_eq_bypass);
    LOG_BOOL(adc_hpf);
    LOG_U32(adceq_b0);
    LOG_U32(adceq_a1);
    LOG_U32(adceq_a2);
    LOG_U32(adceq_b1);
    LOG_U32(adceq_b2);
#undef LOG_BOOL
#undef LOG_UINT
#undef LOG_U32
#undef LOG_IP
#undef LOG_STR
    if (!out.empty()) {
        ESP_LOGI(TAG, "saved: %s", out.c_str());
    } else {
        ESP_LOGI(TAG, "saved: (no change)");
        return;
    }

#if defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389
    if (before->mic_volume != after->mic_volume) {
        const unsigned step = (static_cast<unsigned>(after->mic_volume) * 14u + 127u) / 255u;
        ESP_LOGI(TAG, "[CFG]   ES8389 mic PGA: volume=%u gain=%u dB",
                 static_cast<unsigned>(after->mic_volume),
                 step * 3u);
    }
#endif

#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
    // For ES8311-backed fields, print the affected register address, its full
    // 8-bit value (hex + binary), and the bit breakdown so the change can be
    // cross-checked against the datasheet without having to recompute the
    // packed value. Register layouts mirror es8311.cpp's es8311_*_regNN().
    auto bin8 = [](uint8_t v) -> std::string {
        std::string s;
        s.reserve(8);
        for (int i = 7; i >= 0; --i) {
            s += (v & (1u << i)) ? '1' : '0';
        }
        return s;
    };
    auto bin32 = [](uint32_t v) -> std::string {
        // Group 32 bits by byte ("0b00000000_00000000_00110000_00111001")
        // so 30-bit EQ coefficients stay legible.
        std::string s;
        s.reserve(35);
        for (int i = 31; i >= 0; --i) {
            s += (v & (1u << i)) ? '1' : '0';
            if (i > 0 && (i % 8) == 0) s += '_';
        }
        return s;
    };
    auto reg13 = [](const ExternalRadioConfig *c) -> uint8_t {
        // REG13 HPSW = bit4; 0x10 = drive HP, 0x00 = drive line.
        return c->hp_drive_enabled ? 0x10u : 0x00u;
    };
    auto reg14 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->adc_dmic_enabled ? 0x40u : 0u) |
                                    (c->adc_linsel ? 0x10u : 0u) |
                                    (c->adc_pga_gain & 0x0fu));
    };
    auto reg15 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->adc_ramprate & 0x0fu) << 4) |
                                    (c->adc_dmic_sense ? 0x01u : 0u));
    };
    auto reg16 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->adc_sync ? 0x20u : 0u) |
                                    (c->adc_inv ? 0x10u : 0u) |
                                    (c->adc_ramclr ? 0x08u : 0u) |
                                    (c->adc_scale & 0x07u));
    };
    auto reg18 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->alc_enabled ? 0x80u : 0u) |
                                    (c->adc_automute_enabled ? 0x40u : 0u) |
                                    (c->alc_winsize & 0x0fu));
    };
    auto reg19 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->alc_maxlevel & 0x0fu) << 4) |
                                    (c->alc_minlevel & 0x0fu));
    };
    auto reg1a = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->adc_automute_winsize & 0x0fu) << 4) |
                                    (c->adc_automute_noise_gate & 0x0fu));
    };
    auto reg1b = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->adc_automute_volume & 0x07u) << 5) |
                                    (c->adc_hpfs1 & 0x1fu));
    };
    auto reg1c = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->adc_eq_bypass ? 0x40u : 0u) |
                                    (c->adc_hpf ? 0x20u : 0u) |
                                    (c->adc_hpfs2 & 0x1fu));
    };
    auto reg34 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>((c->drc_enabled ? 0x80u : 0u) |
                                    (c->drc_winsize & 0x0fu));
    };
    auto reg35 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->drc_maxlevel & 0x0fu) << 4) |
                                    (c->drc_minlevel & 0x0fu));
    };
    auto reg37 = [](const ExternalRadioConfig *c) -> uint8_t {
        return static_cast<uint8_t>(((c->dac_ramprate & 0x0fu) << 4) |
                                    (c->dac_eq_bypass ? 0x08u : 0u));
    };

    if (before->hp_drive_enabled != after->hp_drive_enabled) {
        const uint8_t v = reg13(after);
        ESP_LOGI(TAG,"[CFG]   REG13=0x%02X 0b%s (hp_drive=%u)\n",
                      v, bin8(v).c_str(),
                      after->hp_drive_enabled ? 1u : 0u);
    }
    if (before->adc_dmic_enabled != after->adc_dmic_enabled ||
        before->adc_linsel != after->adc_linsel ||
        before->adc_pga_gain != after->adc_pga_gain) {
        const uint8_t v = reg14(after);
        ESP_LOGI(TAG,"[CFG]   REG14=0x%02X 0b%s (dmic_enabled=%u linsel=%u pga_gain=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_dmic_enabled ? 1u : 0u,
                      after->adc_linsel ? 1u : 0u,
                      static_cast<unsigned>(after->adc_pga_gain));
    }
    if (before->adc_ramprate != after->adc_ramprate ||
        before->adc_dmic_sense != after->adc_dmic_sense) {
        const uint8_t v = reg15(after);
        ESP_LOGI(TAG,"[CFG]   REG15=0x%02X 0b%s (adc_ramprate=%u dmic_sense=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_ramprate),
                      after->adc_dmic_sense ? 1u : 0u);
    }
    if (before->adc_sync != after->adc_sync ||
        before->adc_inv != after->adc_inv ||
        before->adc_ramclr != after->adc_ramclr ||
        before->adc_scale != after->adc_scale) {
        const uint8_t v = reg16(after);
        ESP_LOGI(TAG,"[CFG]   REG16=0x%02X 0b%s (sync=%u inv=%u ramclr=%u scale=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_sync ? 1u : 0u,
                      after->adc_inv ? 1u : 0u,
                      after->adc_ramclr ? 1u : 0u,
                      static_cast<unsigned>(after->adc_scale));
    }
    if (before->mic_volume != after->mic_volume) {
        ESP_LOGI(TAG,"[CFG]   REG17=0x%02X 0b%s (mic_volume=%u)\n",
                      after->mic_volume, bin8(after->mic_volume).c_str(),
                      static_cast<unsigned>(after->mic_volume));
    }
    if (before->alc_enabled != after->alc_enabled ||
        before->adc_automute_enabled != after->adc_automute_enabled ||
        before->alc_winsize != after->alc_winsize) {
        const uint8_t v = reg18(after);
        ESP_LOGI(TAG,"[CFG]   REG18=0x%02X 0b%s (alc_enabled=%u automute_enabled=%u alc_winsize=%u)\n",
                      v, bin8(v).c_str(),
                      after->alc_enabled ? 1u : 0u,
                      after->adc_automute_enabled ? 1u : 0u,
                      static_cast<unsigned>(after->alc_winsize));
    }
    if (before->alc_maxlevel != after->alc_maxlevel ||
        before->alc_minlevel != after->alc_minlevel) {
        const uint8_t v = reg19(after);
        ESP_LOGI(TAG,"[CFG]   REG19=0x%02X 0b%s (alc_maxlevel=%u alc_minlevel=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->alc_maxlevel),
                      static_cast<unsigned>(after->alc_minlevel));
    }
    if (before->adc_automute_winsize != after->adc_automute_winsize ||
        before->adc_automute_noise_gate != after->adc_automute_noise_gate) {
        const uint8_t v = reg1a(after);
        ESP_LOGI(TAG,"[CFG]   REG1A=0x%02X 0b%s (automute_winsize=%u automute_noise_gate=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_automute_winsize),
                      static_cast<unsigned>(after->adc_automute_noise_gate));
    }
    if (before->adc_automute_volume != after->adc_automute_volume ||
        before->adc_hpfs1 != after->adc_hpfs1) {
        const uint8_t v = reg1b(after);
        ESP_LOGI(TAG,"[CFG]   REG1B=0x%02X 0b%s (automute_volume=%u hpfs1=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->adc_automute_volume),
                      static_cast<unsigned>(after->adc_hpfs1));
    }
    if (before->adc_eq_bypass != after->adc_eq_bypass ||
        before->adc_hpf != after->adc_hpf ||
        before->adc_hpfs2 != after->adc_hpfs2) {
        const uint8_t v = reg1c(after);
        ESP_LOGI(TAG,"[CFG]   REG1C=0x%02X 0b%s (eq_bypass=%u dynamic_hpf=%u hpfs2=%u)\n",
                      v, bin8(v).c_str(),
                      after->adc_eq_bypass ? 1u : 0u,
                      after->adc_hpf ? 1u : 0u,
                      static_cast<unsigned>(after->adc_hpfs2));
    }
    if (before->line_out_volume != after->line_out_volume) {
        ESP_LOGI(TAG,"[CFG]   REG32=0x%02X 0b%s (line_out_volume=%u)\n",
                      after->line_out_volume, bin8(after->line_out_volume).c_str(),
                      static_cast<unsigned>(after->line_out_volume));
    }
    if (before->drc_enabled != after->drc_enabled ||
        before->drc_winsize != after->drc_winsize) {
        const uint8_t v = reg34(after);
        ESP_LOGI(TAG,"[CFG]   REG34=0x%02X 0b%s (drc_enabled=%u drc_winsize=%u)\n",
                      v, bin8(v).c_str(),
                      after->drc_enabled ? 1u : 0u,
                      static_cast<unsigned>(after->drc_winsize));
    }
    if (before->drc_maxlevel != after->drc_maxlevel ||
        before->drc_minlevel != after->drc_minlevel) {
        const uint8_t v = reg35(after);
        ESP_LOGI(TAG,"[CFG]   REG35=0x%02X 0b%s (drc_maxlevel=%u drc_minlevel=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->drc_maxlevel),
                      static_cast<unsigned>(after->drc_minlevel));
    }
    if (before->dac_ramprate != after->dac_ramprate ||
        before->dac_eq_bypass != after->dac_eq_bypass) {
        const uint8_t v = reg37(after);
        ESP_LOGI(TAG,"[CFG]   REG37=0x%02X 0b%s (dac_ramprate=%u dac_eq_bypass=%u)\n",
                      v, bin8(v).c_str(),
                      static_cast<unsigned>(after->dac_ramprate),
                      after->dac_eq_bypass ? 1u : 0u);
    }
    // 30-bit EQ coefficients occupy 4 consecutive registers each. Print the
    // raw 32-bit value (top 2 bits unused) in hex + binary, plus the address
    // range.
    auto log_eq = [&bin32](const char *range, uint32_t value, const char *field_name) {
        const uint32_t v = value & kDacEqCoefficientMax;
        ESP_LOGI(TAG,"[CFG]   %s=0x%08lX 0b%s (%s=%lu)\n",
                      range,
                      static_cast<unsigned long>(v),
                      bin32(v).c_str(),
                      field_name,
                      static_cast<unsigned long>(value));
    };
    if (before->adceq_b0 != after->adceq_b0) log_eq("REG1D-20", after->adceq_b0, "adceq_b0");
    if (before->adceq_a1 != after->adceq_a1) log_eq("REG21-24", after->adceq_a1, "adceq_a1");
    if (before->adceq_a2 != after->adceq_a2) log_eq("REG25-28", after->adceq_a2, "adceq_a2");
    if (before->adceq_b1 != after->adceq_b1) log_eq("REG29-2C", after->adceq_b1, "adceq_b1");
    if (before->adceq_b2 != after->adceq_b2) log_eq("REG2D-30", after->adceq_b2, "adceq_b2");
    if (before->daceq_b0 != after->daceq_b0) log_eq("REG38-3B", after->daceq_b0, "daceq_b0");
    if (before->daceq_b1 != after->daceq_b1) log_eq("REG3C-3F", after->daceq_b1, "daceq_b1");
    if (before->daceq_a1 != after->daceq_a1) log_eq("REG40-43", after->daceq_a1, "daceq_a1");
#endif
}

// Reply to /save_* with {"ok": bool, "fields": {name: stored_value, ...}}.
// Only fields that the client actually submitted are echoed (skipping the
// hidden _present markers). The client updates its inputs from these values
// so the form always reflects on-device truth without a page reload.
static bool endsWith(const std::string &s, const char *suffix)
{
    if (suffix == nullptr) return false;
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static void sendSavedFieldsJson(const bool ok)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    std::string body;
    body.reserve(1024);
    body += "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"fields\":{";
    bool first = true;
    auto appendField = [&](const std::string &name) {
        const std::string value = savedValueForArg(config, name);
        if (value.empty() && name != "wifi_ssid" && name != "wifi_password" &&
            name != "server_host" && name != "callsign") {
            return;
        }
        if (!first) {
            body += ",";
        }
        first = false;
        body += "\"";
        body += jsonEscape(name);
        body += "\":\"";
        body += jsonEscape(value);
        body += "\"";
    };
    for (int i = 0; i < s_server.args(); ++i) {
        const std::string name = s_server.argName(i);
        if (endsWith(name, "_present") || name == "audio_reset_defaults") {
            continue;
        }
        appendField(name);
    }
    if (s_server.hasArg("audio_reset_defaults")) {
        static const char *kAudioFields[] = {
            "mic_volume", "mic_pcm_gain", "line_out_volume", "hp_drive_enabled",
            "aec_enabled", "aec_reference_source", "ai_noise_enabled",
            "mic_hpf_enabled", "drc_enabled", "drc_winsize",
            "drc_maxlevel", "drc_minlevel", "dac_ramprate",
            "dac_eq_bypass", "daceq_b0", "daceq_b1", "daceq_a1",
            "adc_dmic_enabled", "adc_linsel", "adc_pga_gain",
            "adc_ramprate", "adc_scale", "adc_dmic_sense",
            "adc_sync", "adc_inv", "adc_ramclr", "alc_enabled",
            "adc_automute_enabled", "alc_winsize", "alc_maxlevel",
            "alc_minlevel", "adc_automute_winsize",
            "adc_automute_noise_gate", "adc_automute_volume",
            "adc_hpfs1", "adc_hpfs2", "adc_eq_bypass", "adc_hpf",
            "adceq_b0", "adceq_a1", "adceq_a2", "adceq_b1", "adceq_b2",
        };
        for (const char *name : kAudioFields) {
            appendField(name);
        }
    }
    body += "}}";
    s_server.send(ok ? 200 : 400, "application/json; charset=utf-8", body);
}

static void sendChunkedHtml(const int code, const std::string &html)
{
    httpd_req_t *req = s_server.req;
    if (req == nullptr) {
        return;
    }
    char status[32];
    const char *reason = (code == 200) ? "OK" : (code == 500) ? "Internal Server Error" : "OK";
    snprintf(status, sizeof(status), "%d %s", code, reason);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    constexpr size_t kChunkSize = 1024;
    const char *data = html.c_str();
    size_t remaining = html.size();
    while (remaining > 0) {
        const size_t n = (remaining < kChunkSize) ? remaining : kChunkSize;
        httpd_resp_send_chunk(req, data, n);
        data += n;
        remaining -= n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
}

static void sendConfigPage(const char *title,
                           const char *headline,
                           const char *headline_key,
                           const char *intro,
                           const char *intro_key,
                           const char *form_action,
                           const std::string &form_sections,
                           const bool network_active,
                           const bool device_active,
                           const bool audio_active,
                           const std::string &footer,
                           const bool media_active = false,
                           const bool aprs_active = false,
                           const bool signaling_active = false,
                           const bool home_page = false)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    const WifiConfigPortalPageState state = {
        .title = title,
        .headline = headline,
        .headline_key = headline_key,
        .intro = intro,
        .intro_key = intro_key,
        .form_action = form_action,
        .network_active = network_active,
        .device_active = device_active,
        .audio_active = audio_active,
        .media_active = media_active,
        .aprs_active = aprs_active,
        .signaling_active = signaling_active,
        .home_page = home_page,
        .footer = footer,
    };
    sendChunkedHtml(200, WifiConfigPortalView_BuildConfigPage(config, state, form_sections));
}

static std::string buildUpdatePageI18n(const char *headline,
                                       const char *headline_key,
                                       const char *intro,
                                       const char *intro_key)
{
    return WifiConfigPortalView_BuildUpdatePage(headline, headline_key, intro, intro_key);
}

static esp_err_t handleRoot(httpd_req_t *req)
{
    s_server.bind(req);
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " Config").c_str(),
                   "Configuration",
                   "homeTitle",
                   "Choose the settings page to open.",
                   "homeIntro",
                   "",
                   "",
                   false,
                   false,
                   false,
                   "",
                   false,
                   false,
                   false,
                   true);
    return ESP_OK;
}

static esp_err_t handleWifiPage(httpd_req_t *req)
{
    s_server.bind(req);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " WiFi Config").c_str(),
                   "WiFi Config",
                   "wifiHeadline",
                   "Set the WiFi network and server address used by the device.",
                   "wifiIntro",
                   "/save_wifi",
                   buildNetworkSection(config),
                   true,
                   false,
                   false,
                   "");
    return ESP_OK;
}

static esp_err_t handleNrlPage(httpd_req_t *req)
{
    s_server.bind(req);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " NRL Config").c_str(),
                   "NRL Config",
                   "nrlHeadline",
                   "Set radio identity and audio behavior.",
                   "nrlIntro",
                   "/save_nrl",
                   buildDeviceSections(config),
                   false,
                   true,
                   false,
                   "");
    return ESP_OK;
}

static esp_err_t handleAudioPage(httpd_req_t *req)
{
    s_server.bind(req);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " Audio Settings").c_str(),
                   "Audio Settings",
                   "audioHeadline",
                   "Set ES8311 output, volume, DRC, and EQ.",
                   "audioIntro",
                   "/save_nrl",
                   buildAudioSections(config),
                   false,
                   false,
                   true,
                   "");
    return ESP_OK;
}

static const char kFmoPage[] = R"FMO(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>FMO</title>
<link rel="stylesheet" href="/portal.css?v={{VERSION}}-assets1">
<style>
.fmo-row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.mono{word-break:break-all}
.ok{color:#087d37}.bad{color:#b42318}
</style></head><body><main class="shell">
<div class="topbar">
<div>
<h1 data-i18n="fmoHeadline">FMO-V4 Link</h1>
<p class="sub" data-i18n="fmoIntro">Servers are discovered automatically via APRS-IS (usually within minutes). An FMO identity certificate is not a TLS certificate; all three JSON files must belong to the same identity.</p>
</div>
<div class="status">
<div><span data-i18n="configAp">Config AP</span><strong class="mono">{{AP_IP}}</strong></div>
<div><span data-i18n="stationIp">Station IP</span><strong class="mono">{{STA_IP}}</strong></div>
<div class="language"><span class="lang-label" data-i18n="language">Language</span><div id="language-select" class="lang-radio"><label><input type="radio" name="lang" value="en">English</label><label><input type="radio" name="lang" value="zh">中文</label></div></div>
</div>
</div>
<p class="back-home"><a href="/" data-i18n="backHome">&larr; Back to home</a></p>
<section class="panel"><h2>连接与发射</h2><form id="cfg"><div class="grid"><label>FMO 服务器<select name="server_index" id="servers"><option value="">等待发现…</option></select></label>
<div><label class="fmo-row"><input type="checkbox" name="enabled" value="1" id="enabled">连接所选 FMO 服务器</label>
<label class="fmo-row"><input type="checkbox" name="transmit" value="1" id="transmit">将 PTT/SQL 麦克风上行切换到 FMO</label>
<label class="fmo-row" title="关闭后服务器会返回本客户端发布的消息，用于调试"><input type="checkbox" name="mqtt_no_local" value="1" id="mqtt_no_local">MQTT 5 No Local</label></div></div>
<input type="hidden" name="enabled_present" value="1"><button type="submit">保存并应用</button></form><p id="link" class="mono hint">加载中…</p></section>
<section class="panel"><h2>FMO 身份证书</h2><p id="cert" class="mono hint">加载中…</p><div class="grid">
<label>userCert JSON<input type="file" accept="application/json,.json" data-kind="user"></label>
<label>intermediateCert JSON<input type="file" accept="application/json,.json" data-kind="intermediate"></label>
<label>deviceKey JSON（含私钥种子）<input type="file" accept="application/json,.json" data-kind="devicekey"></label></div>
<p class="hint">deviceKey 会写入板载 LittleFS，不会从网页读回。请妥善保管原始文件，不要上传到公共服务。</p></section>
<section class="panel"><h2>当前服务器</h2><div id="current" class="mono hint">未选择</div></section><section class="panel"><h2>QSO 呼叫</h2><div class="grid"><label>对方呼号<input id="qso_peer" maxlength="9" placeholder="BG8LLD"></label><label>对方 UID（0=未知，QTHANS 自动学习）<input id="qso_uid" type="number" min="0" placeholder="0"></label></div><div class="fmo-row"><button type="button" id="qso_call">发起呼叫</button><button type="button" id="qso_answer">接听</button><button type="button" id="qso_reject">拒绝</button><button type="button" id="qso_cancel">取消/结束</button></div><p id="qso_stat" class="mono hint">加载中…</p><p class="hint">流程：QTHQRY 查询对方服务器 &rarr; 主叫自动跳台 &rarr; CALL &rarr; 对方人工接听。呼号可带 SSID；需要 APRS-IS 上行 verified。被叫振铃 60 秒无人接听自动结束。</p></section>
<section class="panel"><h2>服务器广播（FMO-V4 STATION）</h2><form id="bcfg"><div class="grid"><label class="fmo-row"><input type="checkbox" name="bcast_enabled" value="1" id="bc_enabled">启用 APRS-IS 周期广播</label><label>周期<select name="bcast_mode" id="bc_mode"><option value="2">5 分钟</option><option value="3">10 分钟</option><option value="4">60 分钟</option></select></label><label>国家码（2 字母，手填）<input name="bcast_country" id="bc_country" maxlength="2" placeholder="CN"></label><label>SSID（0-15，0=不带）<input name="bcast_ssid" id="bc_ssid" type="number" min="0" max="15" placeholder="0"></label><label>覆盖半径 km<input name="bcast_cover_km" id="bc_cover" type="number" min="0" max="5000"></label><label>服务器名称（线上 UTF-8）<input name="bcast_name" id="bc_name" maxlength="32" placeholder="留空=当前服务器名"></label><label>广播 host<input name="bcast_host" id="bc_host" maxlength="63" placeholder="留空=当前 FMO host"></label><label>广播端口<input name="bcast_port" id="bc_port" type="number" min="0" max="65535" placeholder="0=当前 FMO 端口"></label><label>在线/峰值（0=自动）<div class="fmo-row"><input name="bcast_online" id="bc_online" type="number" min="0"><input name="bcast_peak" id="bc_peak" type="number" min="0"></div><div class="hint" id="bc_auto">填 0 使用自动</div></label></div><input type="hidden" name="bcast_present" value="1"><button type="submit">保存广播配置</button></form><p id="bstat" class="mono hint">加载中…</p><p class="hint">门控：MQTT 已连接、实际登录角色为 super（admin 暂不等同）、服务器呼号==本机证书呼号、APRS-IS logresp verified、SNTP 已同步；另有 60 秒最小限速。坐标取当前位置（GPS 新鲜优先，否则默认坐标）。远程关停（SERVER_REMOTE_CONTROL）未实现。</p></section>
<section class="panel"><h2>个人信标（FMO-V4 BEACON）</h2><form id="ncfg"><div class="grid"><label class="fmo-row"><input type="checkbox" name="bcn_enabled" value="1" id="nb_enabled">启用个人信标（固定 10 分钟周期）</label><label>SSID（0-15，0=不带）<input name="bcn_ssid" id="nb_ssid" type="number" min="0" max="15" placeholder="0"></label><label>频率 MHz（20-500，4 位小数）<input name="bcn_freq" id="nb_freq" maxlength="10" placeholder="439.1625"></label><label>天线高度 m（0=不播 HEIGHT）<input name="bcn_height" id="nb_height" type="number" min="0" max="65535"></label><label>电台 RIG（≤16 字符，线上 UTF-8）<input name="bcn_rig" id="nb_rig" placeholder="留空=不播 RIG"></label><label>天线 ANT（≤16 字符，线上 UTF-8）<input name="bcn_ant" id="nb_ant" placeholder="留空=不播 ANT"></label><label>APRS 个性消息 APFMO2（≤64 字符）<input name="bcn_aprs_msg" id="nb_msg" placeholder="信标成功后跟发，留空=不发"></label><label>登录公告 APFMO1（≤128 字符）<input name="bcn_notice" id="nb_notice" placeholder="服务器广播成功后跟发，留空=不发"></label><label>QSO 消息（仅存储，≤128 字符）<input name="bcn_qso_msg" id="nb_qso" placeholder="传输机制待研究"></label></div><input type="hidden" name="bcn_present" value="1"><button type="submit">保存信标配置</button></form><p id="nstat" class="mono hint">加载中…</p><p class="hint">门控：APRS-IS logresp verified + 证书就绪 + 频率&gt;0；固定 10 分钟周期 + 60 秒最小限速，不依赖服务器/super 角色。文本禁英文逗号；线上 RIG/ANT/消息/公告为 UTF-8（与签名 TBS 内一致）。整条信标帧 ≤512 字符，超长放弃。APFMO1 公告的名称/在线/峰值沿用服务器广播生效值。</p></section>
<script>
const fmoI18n={en:{language:'Language',configAp:'Config AP',stationIp:'Station IP',backHome:'← Back to home',fmoHeadline:'FMO-V4 Link',fmoIntro:'Servers are discovered automatically via APRS-IS (usually within minutes). An FMO identity certificate is not a TLS certificate; all three JSON files must belong to the same identity.'},zh:{language:'语言',configAp:'配置热点',stationIp:'联网地址',backHome:'← 返回导航首页',fmoHeadline:'FMO‑V4 通联',fmoIntro:'服务器通过 APRS‑IS 自动发现（通常数分钟内出现）。FMO 身份证书不是 TLS 证书，三份 JSON 必须来自同一身份。'}};
function fmoLang(){const s=localStorage.getItem('nrl_lang');if(s==='zh'||s==='en')return s;return navigator.language&&navigator.language.toLowerCase().startsWith('zh')?'zh':'en';}
function fmoApply(l){document.documentElement.lang=l==='zh'?'zh-CN':'en';document.querySelectorAll('input[name="lang"]').forEach(r=>{r.checked=r.value===l;});document.querySelectorAll('[data-i18n]').forEach(el=>{const k=el.getAttribute('data-i18n');if(fmoI18n[l]&&fmoI18n[l][k])el.textContent=fmoI18n[l][k];});const h=document.querySelector('h1');if(h)document.title=h.textContent;}
document.querySelectorAll('input[name="lang"]').forEach(r=>{r.addEventListener('change',()=>{localStorage.setItem('nrl_lang',r.value);fmoApply(r.value);});});
fmoApply(fmoLang());
const esc=s=>String(s??'');let loaded=false;
async function refresh(){try{const r=await fetch('/fmo/status',{cache:'no-store'}),j=await r.json();
enabled.checked=j.config.enabled;transmit.checked=j.config.transmit;mqtt_no_local.checked=j.config.mqtt_no_local;
link.className='mono '+(j.link.connected?'ok':'hint');link.textContent=`${j.link.connected?'已连接':'未连接'} / MQTT Client ID ${j.link.client_id||'---'} / ${j.link.receiving?'接收 '+j.link.voice_callsign+' '+j.link.voice_codec:'空闲'} / RX ${j.link.rx_frames} / 解析错误 ${j.link.parse_errors} / last_error ${j.link.last_error}`;
cert.className='mono '+(j.identity.ready?'ok':'bad');cert.textContent=j.identity.ready?`可用：${j.identity.callsign}，UID ${j.identity.uid}，有效期至 ${new Date(j.identity.expires_at*1000).toLocaleString()}，指纹 ${j.identity.fingerprint}`:`未就绪：user=${j.identity.user_present} intermediate=${j.identity.intermediate_present} deviceKey=${j.identity.device_key_present} error=${j.identity.error}`;
current.textContent=j.config.server.host?`${j.config.server.name} / ${j.config.server.callsign} / UID ${j.config.server.uid} / ${j.config.server.host}:${j.config.server.port}`:'未选择服务器';
qso_stat.className='mono hint';qso_stat.textContent=`状态 ${j.qso.phase}${j.qso.peer?` / 对方 ${j.qso.peer} UID ${j.qso.peer_uid}${j.qso.outgoing?' (呼出)':''}`:''}${j.qso.detail?` / ${j.qso.detail}`:''}`;
bstat.className='mono '+(j.broadcast.status.gated?'bad':'ok');bstat.textContent=`角色 ${j.link.role||'---'} / 广播 ${j.broadcast.config.enabled?'开':'关'} / 门控 ${j.broadcast.status.gated?'阻塞':'通过'} / 已发 ${j.broadcast.status.tx_count} / 最近 ${j.broadcast.status.last_sent_epoch?new Date(j.broadcast.status.last_sent_epoch*1000).toLocaleString():'---'} / 拒因 ${j.broadcast.status.last_reject} / 坐标 ${j.position.lat} ${j.position.lon} ${j.position.gps?'(GPS)':'(默认)'}`;bc_auto.textContent=`填 0 使用自动（当前 在线 ${j.broadcast.auto.online} / 峰值 ${j.broadcast.auto.peak}，生效 ${j.broadcast.auto.effective_online}/${j.broadcast.auto.effective_peak}）`;nstat.className='mono '+(j.beacon.status.gated?'bad':'ok');nstat.textContent=`信标 ${j.beacon.config.enabled?'开':'关'} / 门控 ${j.beacon.status.gated?'阻塞':'通过'} / 已发 ${j.beacon.status.tx_count} / 最近 ${j.beacon.status.last_sent_epoch?new Date(j.beacon.status.last_sent_epoch*1000).toLocaleString():'---'} / 拒因 ${j.beacon.status.last_reject}`;if(!loaded){bc_enabled.checked=j.broadcast.config.enabled;bc_mode.value=String(j.broadcast.config.mode);bc_country.value=j.broadcast.config.country;bc_ssid.value=j.broadcast.config.ssid;bc_name.value=j.broadcast.config.name;bc_host.value=j.broadcast.config.host;bc_port.value=j.broadcast.config.port||'';bc_cover.value=j.broadcast.config.cover_km;bc_online.value=j.broadcast.config.online;bc_peak.value=j.broadcast.config.peak;nb_enabled.checked=j.beacon.config.enabled;nb_ssid.value=j.beacon.config.ssid;nb_freq.value=j.beacon.config.freq_x10000?j.beacon.config.freq:'';nb_height.value=j.beacon.config.height_m;nb_rig.value=j.beacon.config.rig;nb_ant.value=j.beacon.config.ant;nb_msg.value=j.beacon.config.aprs_msg;nb_notice.value=j.beacon.config.notice;nb_qso.value=j.beacon.config.qso_msg;}const old=servers.value;servers.innerHTML='<option value="">保留当前服务器</option>';j.servers.forEach((s,i)=>{const o=document.createElement('option');o.value=i;o.textContent=`${s.name} (${s.callsign}) ${s.host}:${s.port} 在线 ${s.online}/${s.total}`;servers.appendChild(o)});if(old&&servers.querySelector(`option[value="${old}"]`))servers.value=old;loaded=true;
}catch(e){link.className='mono bad';link.textContent='状态读取失败：'+e}}
cfg.onsubmit=async e=>{e.preventDefault();const body=new URLSearchParams(new FormData(cfg));try{const r=await fetch('/fmo/config',{method:'POST',body});const t=await r.text();if(!r.ok)throw Error(t);alert('FMO 配置已保存');refresh()}catch(e){alert('保存失败：'+e)}};
bcfg.onsubmit=async e=>{e.preventDefault();const body=new URLSearchParams(new FormData(bcfg));try{const r=await fetch('/fmo/config',{method:'POST',body});const t=await r.text();if(!r.ok)throw Error(t);alert('广播配置已保存');refresh()}catch(e){alert('保存失败：'+e)}};
ncfg.onsubmit=async e=>{e.preventDefault();const body=new URLSearchParams(new FormData(ncfg));try{const r=await fetch('/fmo/config',{method:'POST',body});const t=await r.text();if(!r.ok)throw Error(t);alert('信标配置已保存');refresh()}catch(e){alert('保存失败：'+e)}};
servers.onchange=()=>cfg.requestSubmit();
current.onclick=()=>{if(typeof servers.showPicker==='function')servers.showPicker();else servers.focus()};
document.querySelectorAll('input[type=file]').forEach(el=>el.onchange=async()=>{if(!el.files[0])return;try{const text=await el.files[0].text();const r=await fetch('/fmo/cert/'+el.dataset.kind,{method:'POST',headers:{'Content-Type':'application/json'},body:text});const t=await r.text();if(!r.ok)throw Error(t);alert('证书已验证并写入');el.value='';refresh()}catch(e){alert('证书写入失败：'+e)}});
async function qsoAct(a){const body=new URLSearchParams({action:a,peer:qso_peer.value,uid:qso_uid.value||'0'});try{const r=await fetch('/fmo/qso',{method:'POST',body});const t=await r.text();if(!r.ok)throw Error(t);refresh()}catch(e){alert('操作失败：'+e)}};
qso_call.onclick=()=>qsoAct('call');qso_answer.onclick=()=>qsoAct('answer');qso_reject.onclick=()=>qsoAct('reject');qso_cancel.onclick=()=>qsoAct('cancel');
refresh();setInterval(refresh,3000);
</script></main></body></html>)FMO";

static void replaceFmoToken(std::string &html, const char *token, const std::string &value)
{
    const size_t pos = html.find(token);
    if (pos != std::string::npos) {
        html.replace(pos, strlen(token), value);
    }
}

static esp_err_t handleFmoPage(httpd_req_t *req)
{
    s_server.bind(req);
    std::string html = kFmoPage;
    char ip_buf[16] = {};
    nrlIpToString(nrlWifiApIp(), ip_buf, sizeof(ip_buf));
    replaceFmoToken(html, "{{AP_IP}}", ip_buf);
    const uint32_t sta_ip = nrlNetworkIp();
    if (sta_ip != 0u) {
        nrlIpToString(sta_ip, ip_buf, sizeof(ip_buf));
    } else {
        snprintf(ip_buf, sizeof(ip_buf), "not connected");
    }
    replaceFmoToken(html, "{{STA_IP}}", ip_buf);
    replaceFmoToken(html, "{{VERSION}}", NRL_FIRMWARE_VERSION);
    sendChunkedHtml(200, html);
    return ESP_OK;
}

static std::string fingerprintHex(const uint8_t fingerprint[32])
{
    static const char digits[] = "0123456789abcdef";
    std::string result(64u, '0');
    for (size_t i = 0; i < 32u; ++i) {
        result[i * 2u] = digits[fingerprint[i] >> 4u];
        result[i * 2u + 1u] = digits[fingerprint[i] & 0x0fu];
    }
    return result;
}

static esp_err_t handleFmoStatus(httpd_req_t *req)
{
    s_server.bind(req);
    FmoConfig config = {};
    FmoLinkStatus link = {};
    FmoIdentityStatus identity = {};
    FMO_GetConfig(&config);
    FMO_GetLinkStatus(&link);
    const esp_err_t identity_error = FMO_CERT_GetStatus(&identity);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    std::string head = "{\"config\":{\"enabled\":";
    head += config.enabled ? "true" : "false";
    head += ",\"transmit\":";
    head += config.transmit ? "true" : "false";
    head += ",\"mqtt_no_local\":";
    head += config.mqtt_no_local ? "true" : "false";
    head += ",\"server\":{\"name\":\"" + jsonEscape(config.server.name) +
            "\",\"host\":\"" + jsonEscape(config.server.host) +
            "\",\"callsign\":\"" + jsonEscape(config.server.callsign) +
            "\",\"port\":" + std::to_string(config.server.port) +
            ",\"uid\":" + std::to_string(config.server.uid) + "}},";
    head += "\"link\":{\"connected\":";
    head += link.connected ? "true" : "false";
    head += ",\"role\":\"" + jsonEscape(link.role) + "\"";
    head += ",\"receiving\":";
    head += link.receiving ? "true" : "false";
    head += ",\"transmitting\":";
    head += link.transmitting ? "true" : "false";
    head += ",\"client_id\":\"" + jsonEscape(link.client_id) +
            "\",\"voice_callsign\":\"" + jsonEscape(link.voice_callsign) +
            "\",\"voice_codec\":\"" + jsonEscape(link.voice_codec) +
            "\",\"rx_frames\":" + std::to_string(link.rx_frames) +
            ",\"parse_errors\":" + std::to_string(link.parse_errors) +
            ",\"last_error\":" + std::to_string(link.last_error) + "},";
    head += "\"identity\":{\"ready\":";
    head += identity.ready ? "true" : "false";
    head += ",\"user_present\":";
    head += identity.user_present ? "true" : "false";
    head += ",\"intermediate_present\":";
    head += identity.intermediate_present ? "true" : "false";
    head += ",\"device_key_present\":";
    head += identity.device_key_present ? "true" : "false";
    head += ",\"callsign\":\"" + jsonEscape(identity.callsign) +
            "\",\"uid\":" + std::to_string(identity.uid) +
            ",\"expires_at\":" + std::to_string(identity.expires_at) +
            ",\"fingerprint\":\"" + (identity.ready ? fingerprintHex(identity.fingerprint) : "") +
            "\",\"error\":\"" + jsonEscape(esp_err_to_name(identity_error)) +
            "\"},";
    {
        FmoStationBroadcastConfig bcast = {};
        FmoStationBroadcastStatus bstat = {};
        FMO_STATION_BCAST_GetConfig(&bcast);
        FMO_STATION_BCAST_GetStatus(&bstat);
        uint32_t auto_online = 0u, auto_peak = 0u;
        FMO_STATION_BCAST_GetAutoCounts(&auto_online, &auto_peak);
        head += "\"broadcast\":{\"config\":{\"enabled\":";
        head += bcast.enabled ? "true" : "false";
        head += ",\"mode\":" + std::to_string(bcast.mode) +
                ",\"country\":\"" + jsonEscape(bcast.country) +
                "\",\"name\":\"" + jsonEscape(bcast.name) +
                "\",\"host\":\"" + jsonEscape(bcast.host) +
                "\",\"port\":" + std::to_string(bcast.port) +
                ",\"cover_km\":" + std::to_string(bcast.cover_km) +
                ",\"online\":" + std::to_string(bcast.online) +
                ",\"peak\":" + std::to_string(bcast.peak) +
                ",\"ssid\":" + std::to_string(bcast.ssid) +
                "},\"status\":{\"configured\":";
        head += bstat.configured ? "true" : "false";
        head += ",\"gated\":";
        head += bstat.gated ? "true" : "false";
        head += ",\"tx_count\":" + std::to_string(bstat.tx_count) +
                ",\"last_sent_epoch\":" + std::to_string(bstat.last_sent_epoch) +
                ",\"last_reject\":" + std::to_string(bstat.last_reject) +
                "},\"auto\":{\"online\":" + std::to_string(auto_online) +
                ",\"peak\":" + std::to_string(auto_peak) +
                ",\"effective_online\":" +
                std::to_string(FMO_STATION_CORE_EffectiveCount(bcast.online, auto_online)) +
                ",\"effective_peak\":" +
                std::to_string(FMO_STATION_CORE_EffectiveCount(bcast.peak, auto_peak)) +
                "}},";
    }
    {
        FmoBeaconConfig bcn = {};
        FmoBeaconStatus bstat = {};
        FMO_BEACON_GetConfig(&bcn);
        FMO_BEACON_GetStatus(&bstat);
        char freq[16];
        snprintf(freq, sizeof(freq), "%lu.%04lu",
                 static_cast<unsigned long>(bcn.freq_x10000 / 10000u),
                 static_cast<unsigned long>(bcn.freq_x10000 % 10000u));
        head += "\"beacon\":{\"config\":{\"enabled\":";
        head += bcn.enabled ? "true" : "false";
        head += ",\"ssid\":" + std::to_string(bcn.ssid) +
                ",\"freq\":\"" + std::string(freq) + "\"" +
                ",\"freq_x10000\":" + std::to_string(bcn.freq_x10000) +
                ",\"height_m\":" + std::to_string(bcn.height_m) +
                ",\"rig\":\"" + jsonEscape(bcn.rig) +
                "\",\"ant\":\"" + jsonEscape(bcn.ant) +
                "\",\"aprs_msg\":\"" + jsonEscape(bcn.aprs_msg) +
                "\",\"notice\":\"" + jsonEscape(bcn.notice) +
                "\",\"qso_msg\":\"" + jsonEscape(bcn.qso_msg) +
                "\"},\"status\":{\"gated\":";
        head += bstat.gated ? "true" : "false";
        head += ",\"tx_count\":" + std::to_string(bstat.tx_count) +
                ",\"last_sent_epoch\":" + std::to_string(bstat.last_sent_epoch) +
                ",\"last_reject\":" + std::to_string(bstat.last_reject) + "}},";
    }
    {
        FmoQsoSnapshot qso = {};
        FMO_QSO_GetSnapshot(&qso);
        head += "\"qso\":{\"phase\":\"" + jsonEscape(qso.phase_name) +
                "\",\"peer\":\"" + jsonEscape(qso.peer) +
                "\",\"peer_uid\":" + std::to_string(qso.peer_uid) +
                ",\"outgoing\":";
        head += qso.outgoing ? "true" : "false";
        head += ",\"detail\":\"" + jsonEscape(qso.detail) + "\"},";
    }
    {
        // The position that would go into the next broadcast; GPS wins
        // while fresh, otherwise the configured default coordinates.
        double lat = 0.0, lon = 0.0;
        const bool gps = APRS_SERVICE_GetOwnPosition(&lat, &lon, nullptr);
        char lat_str[10], lon_str[11];
        FMO_STATION_CORE_FormatLat(lat, lat_str);
        FMO_STATION_CORE_FormatLon(lon, lon_str);
        head += "\"position\":{\"lat\":\"" + std::string(lat_str) +
                "\",\"lon\":\"" + std::string(lon_str) + "\",\"gps\":";
        head += gps ? "true" : "false";
        head += "},\"servers\":[";
    }
    httpd_resp_send_chunk(req, head.c_str(), head.size());
    const size_t count = FMO_ServerCount();
    for (size_t i = 0; i < count; ++i) {
        FmoServer server = {};
        if (!FMO_GetServer(i, &server)) continue;
        std::string item = i == 0u ? "{" : ",{";
        item += "\"name\":\"" + jsonEscape(server.name) +
                "\",\"host\":\"" + jsonEscape(server.host) +
                "\",\"callsign\":\"" + jsonEscape(server.callsign) +
                "\",\"port\":" + std::to_string(server.port) +
                ",\"uid\":" + std::to_string(server.uid) +
                ",\"online\":" + std::to_string(server.online) +
                ",\"total\":" + std::to_string(server.total) + "}";
        httpd_resp_send_chunk(req, item.c_str(), item.size());
    }
    httpd_resp_send_chunk(req, "]}", 2u);
    httpd_resp_send_chunk(req, nullptr, 0u);
    return ESP_OK;
}

static esp_err_t handleFmoConfig(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
    }
    if (s_server.hasArg("bcast_present")) {
        // The broadcast form carries only bcast_* fields; it must not
        // touch the FMO link config (absent checkboxes would read off).
        FmoStationBroadcastConfig bcast = {};
        FMO_STATION_BCAST_GetConfig(&bcast);
        bcast.enabled = s_server.hasArg("bcast_enabled");
        bcast.mode = static_cast<uint8_t>(strtoul(
            s_server.arg("bcast_mode").c_str(), nullptr, 10));
        snprintf(bcast.country, sizeof(bcast.country), "%s",
                 s_server.arg("bcast_country").c_str());
        snprintf(bcast.name, sizeof(bcast.name), "%s",
                 s_server.arg("bcast_name").c_str());
        snprintf(bcast.host, sizeof(bcast.host), "%s",
                 s_server.arg("bcast_host").c_str());
        bcast.port = static_cast<uint16_t>(strtoul(
            s_server.arg("bcast_port").c_str(), nullptr, 10));
        bcast.cover_km = static_cast<uint32_t>(strtoul(
            s_server.arg("bcast_cover_km").c_str(), nullptr, 10));
        bcast.online = static_cast<uint32_t>(strtoul(
            s_server.arg("bcast_online").c_str(), nullptr, 10));
        bcast.peak = static_cast<uint32_t>(strtoul(
            s_server.arg("bcast_peak").c_str(), nullptr, 10));
        bcast.ssid = static_cast<uint8_t>(strtoul(
            s_server.arg("bcast_ssid").c_str(), nullptr, 10));
        if (!FMO_STATION_BCAST_SetConfig(&bcast, true)) {
            return httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST,
                bcast.enabled && !FMO_STATION_BCAST_GatesOk()
                    ? "broadcast gates unmet: MQTT connected + login role super + server callsign == certificate callsign"
                    : "incomplete broadcast config (country/host/port/name or period)");
        }
        return httpd_resp_sendstr(req, "OK");
    }
    if (s_server.hasArg("bcn_present")) {
        // The beacon form carries only bcn_* fields; it must not touch the
        // FMO link or STATION broadcast config.
        FmoBeaconConfig bcn = {};
        FMO_BEACON_GetConfig(&bcn);
        bcn.enabled = s_server.hasArg("bcn_enabled");
        bcn.ssid = static_cast<uint8_t>(strtoul(
            s_server.arg("bcn_ssid").c_str(), nullptr, 10));
        const double mhz = strtod(s_server.arg("bcn_freq").c_str(), nullptr);
        bcn.freq_x10000 =
            mhz > 0.0 ? static_cast<uint32_t>(mhz * 10000.0 + 0.5) : 0u;
        bcn.height_m = static_cast<uint16_t>(strtoul(
            s_server.arg("bcn_height").c_str(), nullptr, 10));
        snprintf(bcn.rig, sizeof(bcn.rig), "%s",
                 s_server.arg("bcn_rig").c_str());
        snprintf(bcn.ant, sizeof(bcn.ant), "%s",
                 s_server.arg("bcn_ant").c_str());
        snprintf(bcn.aprs_msg, sizeof(bcn.aprs_msg), "%s",
                 s_server.arg("bcn_aprs_msg").c_str());
        snprintf(bcn.notice, sizeof(bcn.notice), "%s",
                 s_server.arg("bcn_notice").c_str());
        snprintf(bcn.qso_msg, sizeof(bcn.qso_msg), "%s",
                 s_server.arg("bcn_qso_msg").c_str());
        if (!FMO_BEACON_SetConfig(&bcn, true)) {
            return httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST,
                "invalid beacon config (text char limit/ASCII comma, or freq outside 20-500 MHz)");
        }
        return httpd_resp_sendstr(req, "OK");
    }
    FmoConfig config = {};
    FMO_GetConfig(&config);
    config.enabled = s_server.hasArg("enabled");
    config.transmit = s_server.hasArg("transmit");
    config.mqtt_no_local = s_server.hasArg("mqtt_no_local");
    if (s_server.hasArg("server_index") && !s_server.arg("server_index").empty()) {
        char *end = nullptr;
        const unsigned long index = strtoul(s_server.arg("server_index").c_str(), &end, 10);
        FmoServer selected = {};
        if (end == nullptr || *end != '\0' || !FMO_GetServer(index, &selected)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid server selection");
        }
        config.server = selected;
    }
    if (!FMO_SetConfig(&config, true)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "select a valid FMO server before enabling");
    }
    return httpd_resp_sendstr(req, "OK");
}

// FMO QSO 呼叫信令: action=call（peer/uid）|answer|reject|cancel。
static esp_err_t handleFmoQso(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "form parse failed");
    }
    const std::string action = s_server.arg("action");
    if (action == "call") {
        const std::string peer = s_server.arg("peer");
        const uint32_t uid = static_cast<uint32_t>(
            strtoul(s_server.arg("uid").c_str(), nullptr, 10));
        char err[128];
        if (!FMO_QSO_StartCall(peer.c_str(), uid, err, sizeof(err))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        }
        return httpd_resp_sendstr(req, "OK");
    }
    if (action == "answer" || action == "reject") {
        if (!FMO_QSO_Answer(action == "answer")) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "no incoming call");
        }
        return httpd_resp_sendstr(req, "OK");
    }
    if (action == "cancel") {
        FMO_QSO_Cancel();
        return httpd_resp_sendstr(req, "OK");
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "unknown qso action");
}

static bool receiveRawBodyLimit(httpd_req_t *req, const size_t limit,
                                char **body, size_t *size)
{
    if (req->content_len == 0u || req->content_len > limit) return false;
    // NRL server JSON can be tens of KiB. Keep request/list payloads out of the
    // latency-sensitive internal SRAM and fail cleanly if PSRAM is unavailable.
    char *buffer = static_cast<char *>(heap_caps_malloc(
        req->content_len + 1u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        ESP_LOGW(TAG, "PSRAM allocation failed for %u-byte request",
                 static_cast<unsigned>(req->content_len));
        return false;
    }
    size_t received = 0u;
    while (received < req->content_len) {
        const int got = httpd_req_recv(req, buffer + received,
                                       req->content_len - received);
        if (got <= 0) {
            free(buffer);
            return false;
        }
        received += static_cast<size_t>(got);
    }
    buffer[received] = '\0';
    *body = buffer;
    *size = received;
    return true;
}

static esp_err_t handleNrlServersGet(httpd_req_t *req)
{
    uint8_t *payload = nullptr;
    size_t payload_size = 0u;
    if (!SERVER_LIST_STORE_Read(SERVER_LIST_NRL, &payload, &payload_size)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "NRL server cache is empty");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(
        req, reinterpret_cast<const char *>(payload), payload_size);
    free(payload);
    return result;
}

static esp_err_t handleNrlServersPut(httpd_req_t *req)
{
    char *body = nullptr;
    size_t size = 0u;
    if (!receiveRawBodyLimit(req, 64u * 1024u, &body, &size)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "empty or oversized server list");
    }
    const size_t count = SERVER_LIST_STORE_ValidateNrlJson(body, size);
    if (count == 0u || count > 512u) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid NRL server JSON");
    }
    const bool saved = SERVER_LIST_STORE_Write(SERVER_LIST_NRL, body, size);
    free(body);
    if (!saved) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "server-list filesystem write failed");
    }
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t handleNrlServersRefresh(httpd_req_t *req)
{
    const ExternalRadioConfig *radio = EXTERNAL_RADIO_GetConfig();
    if (radio == nullptr || radio->server_host[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "NRL server host is empty");
    }
    size_t count = 0u;
    if (!SERVER_LIST_STORE_RefreshNrlHttps(radio->server_host,
                                           radio->server_port, &count)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "NRL HTTPS directory refresh failed");
    }
    ESP_LOGI(TAG, "NRL Web directory refreshed: %u servers",
             static_cast<unsigned>(count));
    return handleNrlServersGet(req);
}

static bool receiveRawBody(httpd_req_t *req, char **body, size_t *size)
{
    return receiveRawBodyLimit(req, 24u * 1024u, body, size);
}

static esp_err_t handleFmoCertificate(httpd_req_t *req)
{
    FmoCertificateKind kind = FMO_CERT_USER;
    if (strcmp(req->uri, "/fmo/cert/intermediate") == 0) {
        kind = FMO_CERT_INTERMEDIATE;
    } else if (strcmp(req->uri, "/fmo/cert/devicekey") == 0) {
        kind = FMO_CERT_DEVICE_KEY;
    }
    char *body = nullptr;
    size_t size = 0u;
    if (!receiveRawBody(req, &body, &size)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "empty or oversized JSON");
    }
    char error[128] = {};
    const esp_err_t result = FMO_CERT_Put(kind, body, size, error, sizeof(error));
    free(body);
    if (result != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   error[0] != '\0' ? error : esp_err_to_name(result));
    }
    FmoConfig config = {};
    FMO_GetConfig(&config);
    (void)FMO_SetConfig(&config, false); // reconnect with the new identity
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t handleMediaPage(httpd_req_t *req)
{
    s_server.bind(req);
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " Media / Nanny").c_str(),
                   "Media / Nanny",
                   "mediaHeadline",
                   "Configure playback target, nanny beacon, net radio, and SMB network share.",
                   "mediaIntro",
                   "/save_media",
                   WifiConfigPortalView_BuildMediaSections(),
                   false,
                   false,
                   false,
                   "",
                   true);
    return ESP_OK;
}

static bool parseUIntArg(const std::string &text, unsigned long *out_value);

#if NRL_HAS_SIGNALING
static bool parseHexArg(const std::string &text, unsigned long *out_value)
{
    if (out_value == nullptr || text.empty()) return false;
    char *end = nullptr;
    const unsigned long value = strtoul(text.c_str(), &end, 16);
    if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
    *out_value = value;
    return true;
}
#endif // NRL_HAS_SIGNALING

static esp_err_t handleAprsPage(httpd_req_t *req)
{
    s_server.bind(req);
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " APRS").c_str(),
                   "APRS",
                   "aprsHeadline",
                   "GPS beacons to APRS-IS and AFSK over the radio; stations heard are listed below.",
                   "aprsIntro",
                   "/save_aprs",
                   WifiConfigPortalView_BuildAprsSections(),
                   false,
                   false,
                   false,
                   "",
                   false,
                   true);
    return ESP_OK;
}

#if NRL_HAS_SIGNALING
static esp_err_t handleSignalingPage(httpd_req_t *req)
{
    s_server.bind(req);
    sendConfigPage((std::string(NRL_FIRMWARE_NAME) + " Signaling").c_str(),
                   "CW / MDC1200 / DTMF / CTCSS",
                   "signalingHeadline",
                   "Configure CW and signaling decode sources and voice-tail transmit destinations.",
                   "signalingIntro",
                   "/save_signaling",
                   WifiConfigPortalView_BuildSignalingSections(),
                   false, false, false, "", false, false, true);
    return ESP_OK;
}

static void sendSignalingSavedJson(const bool ok)
{
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);
    char op[3], arg[3], unit_id[5];
    snprintf(op, sizeof(op), "%02X", cfg.mdc_opcode);
    snprintf(arg, sizeof(arg), "%02X", cfg.mdc_argument);
    snprintf(unit_id, sizeof(unit_id), "%04X", cfg.mdc_unit_id);
    std::string body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"fields\":{";
    auto append = [&body](const char *name, const char *value, bool first = false) {
        if (!first) body += ",";
        body += "\"" + std::string(name) + "\":\"" + jsonEscape(value) + "\"";
    };
    append("ctcss_rx_mic", cfg.ctcss_rx_mic ? "1" : "0", true);
    append("ctcss_rx_nrl", cfg.ctcss_rx_nrl ? "1" : "0");
    append("mdc_rx_mic", cfg.mdc_rx_mic ? "1" : "0");
    append("mdc_rx_nrl", cfg.mdc_rx_nrl ? "1" : "0");
    append("mdc_tx_nrl", cfg.mdc_tx_nrl ? "1" : "0");
    append("mdc_tx_speaker", cfg.mdc_tx_speaker ? "1" : "0");
    append("cw_rx_mic", cfg.cw_rx_mic ? "1" : "0");
    append("cw_rx_nrl", cfg.cw_rx_nrl ? "1" : "0");
    append("dtmf_rx_mic", cfg.dtmf_rx_mic ? "1" : "0");
    append("dtmf_rx_nrl", cfg.dtmf_rx_nrl ? "1" : "0");
    append("dtmf_tx_nrl", cfg.dtmf_tx_nrl ? "1" : "0");
    append("dtmf_tx_speaker", cfg.dtmf_tx_speaker ? "1" : "0");
    append("mdc_opcode", op);
    append("mdc_argument", arg);
    append("mdc_unit_id", unit_id);
    append("dtmf_digits", cfg.dtmf_digits);
    body += "}}";
    s_server.send(200, "application/json; charset=utf-8", body);
}

static esp_err_t handleSaveSignaling(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    bool ok = true;
    if (s_server.hasArg("ctcss_rx_mic_present")) {
        ok = SIGNALING_SetCtcssRoute(SIGNAL_ROUTE_RX_MIC,
                                     s_server.hasArg("ctcss_rx_mic"));
    }
    if (ok && s_server.hasArg("ctcss_rx_nrl_present")) {
        ok = SIGNALING_SetCtcssRoute(SIGNAL_ROUTE_RX_NRL,
                                     s_server.hasArg("ctcss_rx_nrl"));
    }
    if (ok && s_server.hasArg("cw_rx_mic_present")) {
        ok = SIGNALING_SetCwRoute(SIGNAL_ROUTE_RX_MIC, s_server.hasArg("cw_rx_mic"));
    }
    if (ok && s_server.hasArg("cw_rx_nrl_present")) {
        ok = SIGNALING_SetCwRoute(SIGNAL_ROUTE_RX_NRL, s_server.hasArg("cw_rx_nrl"));
    }
    struct RouteField { const char *present; const char *name; bool mdc; SignalingRoute route; };
    static const RouteField routes[] = {
        {"mdc_rx_mic_present", "mdc_rx_mic", true, SIGNAL_ROUTE_RX_MIC},
        {"mdc_rx_nrl_present", "mdc_rx_nrl", true, SIGNAL_ROUTE_RX_NRL},
        {"mdc_tx_nrl_present", "mdc_tx_nrl", true, SIGNAL_ROUTE_TX_NRL},
        {"mdc_tx_speaker_present", "mdc_tx_speaker", true, SIGNAL_ROUTE_TX_SPEAKER},
        {"dtmf_rx_mic_present", "dtmf_rx_mic", false, SIGNAL_ROUTE_RX_MIC},
        {"dtmf_rx_nrl_present", "dtmf_rx_nrl", false, SIGNAL_ROUTE_RX_NRL},
        {"dtmf_tx_nrl_present", "dtmf_tx_nrl", false, SIGNAL_ROUTE_TX_NRL},
        {"dtmf_tx_speaker_present", "dtmf_tx_speaker", false, SIGNAL_ROUTE_TX_SPEAKER},
    };
    for (const RouteField &field : routes) {
        if (!ok || !s_server.hasArg(field.present)) continue;
        const bool enabled = s_server.hasArg(field.name);
        ok = field.mdc ? SIGNALING_SetMdcRoute(field.route, enabled)
                       : SIGNALING_SetDtmfRoute(field.route, enabled);
    }
    if (ok && (s_server.hasArg("mdc_opcode") || s_server.hasArg("mdc_argument") ||
               s_server.hasArg("mdc_unit_id"))) {
        SignalingConfig cfg{};
        SIGNALING_GetConfig(&cfg);
        unsigned long op = cfg.mdc_opcode, arg = cfg.mdc_argument, unit_id = cfg.mdc_unit_id;
        if (s_server.hasArg("mdc_opcode")) ok = parseHexArg(s_server.arg("mdc_opcode"), &op) && op <= 0xFFUL;
        if (ok && s_server.hasArg("mdc_argument")) ok = parseHexArg(s_server.arg("mdc_argument"), &arg) && arg <= 0xFFUL;
        if (ok && s_server.hasArg("mdc_unit_id")) ok = parseHexArg(s_server.arg("mdc_unit_id"), &unit_id) && unit_id <= 0xFFFFUL;
        if (ok) ok = SIGNALING_SetMdcPacket(static_cast<uint8_t>(op), static_cast<uint8_t>(arg),
                                             static_cast<uint16_t>(unit_id));
    }
    if (ok && s_server.hasArg("dtmf_digits")) {
        ok = SIGNALING_SetDtmfDigits(s_server.arg("dtmf_digits").c_str());
    }
    if (!ok) ESP_LOGE(TAG, "signaling config save via web failed");
    sendSignalingSavedJson(ok);
    return ESP_OK;
}
#endif // NRL_HAS_SIGNALING

static void sendSerialSavedJson(const bool ok)
{
    SerialPortConfig serial{};
    SERIAL_PORT_CONFIG_Get(&serial);
    const ExternalRadioConfig *radio = EXTERNAL_RADIO_GetConfig();
    char body[512];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"fields\":{"
             "\"uart1_enabled\":\"%u\",\"uart2_enabled\":\"%u\","
             "\"uart1_rx_pin\":\"%d\",\"uart1_tx_pin\":\"%d\","
             "\"uart1_baud\":\"%lu\",\"uart1_data_bits\":\"%u\","
             "\"uart1_parity\":\"%c\",\"uart1_stop_bits\":\"%u\","
             "\"uart2_rx_pin\":\"%d\",\"uart2_tx_pin\":\"%d\","
             "\"uart2_baud\":\"%lu\",\"uart2_data_bits\":\"%u\","
             "\"uart2_parity\":\"%c\",\"uart2_stop_bits\":\"%u\"}}",
             ok ? "true" : "false",
             serial.uart1_enabled ? 1u : 0u, serial.uart2_enabled ? 1u : 0u,
             serial.uart1_rx_pin, serial.uart1_tx_pin,
             static_cast<unsigned long>(radio != nullptr ? radio->sci.baud : 9600u),
             static_cast<unsigned>(radio != nullptr ? radio->sci.data_bits : 8u),
             radio != nullptr ? radio->sci.parity : 'N',
             static_cast<unsigned>(radio != nullptr ? radio->sci.stop_bits : 1u),
             serial.uart2_rx_pin, serial.uart2_tx_pin,
             static_cast<unsigned long>(serial.uart2_baud),
             static_cast<unsigned>(serial.uart2_data_bits), serial.uart2_parity,
             static_cast<unsigned>(serial.uart2_stop_bits));
    s_server.send(ok ? 200 : 400, "application/json; charset=utf-8", body);
}

static esp_err_t handleSaveSerial(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }

    SerialPortConfig before{};
    SERIAL_PORT_CONFIG_Get(&before);
    SerialPortConfig updated = before;
    const ExternalRadioConfig *radio = EXTERNAL_RADIO_GetConfig();
    const SciSerialConfig old_sci = radio != nullptr ? radio->sci : SciSerialConfig{9600u, 8u, 'N', 1u};
    SciSerialConfig new_sci = old_sci;

    if (s_server.hasArg("uart1_enabled_present")) {
        updated.uart1_enabled = s_server.hasArg("uart1_enabled");
    }
    if (s_server.hasArg("uart2_enabled_present")) {
        updated.uart2_enabled = s_server.hasArg("uart2_enabled");
    }

    auto parseNumber = [](const std::string &text, unsigned long max, unsigned long *out) {
        return parseUIntArg(text, out) && *out <= max;
    };
    auto parsePin = [](const std::string &text, int *out) {
        if (out == nullptr || text.empty()) return false;
        char *end = nullptr;
        const long value = strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' || value < -1L || value > 127L) {
            return false;
        }
        *out = static_cast<int>(value);
        return true;
    };
    unsigned long value = 0u;
    bool ok = parsePin(s_server.arg("uart1_rx_pin"), &updated.uart1_rx_pin);
    if (ok) ok = parsePin(s_server.arg("uart1_tx_pin"), &updated.uart1_tx_pin);
    if (ok) ok = parseNumber(s_server.arg("uart1_baud"), 921600u, &value) && value >= 300u;
    if (ok) new_sci.baud = static_cast<uint32_t>(value);
    if (ok) ok = parseNumber(s_server.arg("uart1_data_bits"), 8u, &value) && value >= 5u;
    if (ok) new_sci.data_bits = static_cast<uint8_t>(value);
    if (ok) {
        const std::string parity = s_server.arg("uart1_parity");
        ok = parity.size() == 1u;
        if (ok) new_sci.parity = static_cast<char>(toupper(static_cast<unsigned char>(parity[0])));
    }
    if (ok) ok = parseNumber(s_server.arg("uart1_stop_bits"), 2u, &value) && value >= 1u;
    if (ok) new_sci.stop_bits = static_cast<uint8_t>(value);

    if (ok) ok = parsePin(s_server.arg("uart2_rx_pin"), &updated.uart2_rx_pin);
    if (ok) ok = parsePin(s_server.arg("uart2_tx_pin"), &updated.uart2_tx_pin);
    if (ok) ok = parseNumber(s_server.arg("uart2_baud"), 921600u, &value) && value >= 300u;
    if (ok) updated.uart2_baud = static_cast<uint32_t>(value);
    if (ok) ok = parseNumber(s_server.arg("uart2_data_bits"), 8u, &value) && value >= 5u;
    if (ok) updated.uart2_data_bits = static_cast<uint8_t>(value);
    if (ok) {
        const std::string parity = s_server.arg("uart2_parity");
        ok = parity.size() == 1u;
        if (ok) updated.uart2_parity = static_cast<char>(toupper(static_cast<unsigned char>(parity[0])));
    }
    if (ok) ok = parseNumber(s_server.arg("uart2_stop_bits"), 2u, &value) && value >= 1u;
    if (ok) updated.uart2_stop_bits = static_cast<uint8_t>(value);

    if (ok) ok = SERIAL_PORT_CONFIG_Validate(&updated);
    if (ok) ok = EXTERNAL_RADIO_SetSciConfig(new_sci.baud, new_sci.data_bits,
                                               new_sci.parity, new_sci.stop_bits, false);
    if (ok) ok = SERIAL_PORT_CONFIG_Set(&updated, true);
    if (ok) ok = EXTERNAL_RADIO_SaveConfig();
    // Reload UART1 unconditionally because its GPIOs may have changed even
    // when baud/data/parity/stop are unchanged.
    if (ok) ok = SERIAL_PORT_CONFIG_ReloadDrivers();
    if (!ok) {
        (void)EXTERNAL_RADIO_SetSciConfig(old_sci.baud, old_sci.data_bits,
                                          old_sci.parity, old_sci.stop_bits, false);
        (void)EXTERNAL_RADIO_SaveConfig();
        (void)SERIAL_PORT_CONFIG_Set(&before, true);
        (void)SERIAL_PORT_CONFIG_ReloadDrivers();
        ESP_LOGE(TAG, "serial config save via web failed (invalid/conflicting GPIO or UART params)");
    }
    sendSerialSavedJson(ok);
    return ESP_OK;
}

static void sendAprsSavedJson(const bool ok)
{
    AprsConfig cfg;
    APRS_SERVICE_GetConfig(&cfg);
    std::string body;
    body.reserve(384);
    body += "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"fields\":{";
    auto appendField = [&body](const char *name, const std::string &value, const bool first = false) {
        if (!first) {
            body += ",";
        }
        body += "\"";
        body += name;
        body += "\":\"";
        body += jsonEscape(value);
        body += "\"";
    };
    appendField("aprs_enabled", cfg.enabled ? "1" : "0", true);
    appendField("aprs_net", cfg.net_enabled ? "1" : "0");
    appendField("aprs_tx", cfg.rf_tx_enabled ? "1" : "0");
    appendField("aprs_rx", cfg.rf_rx_enabled ? "1" : "0");
    appendField("aprs_auto", cfg.auto_interval ? "1" : "0");
    appendField("aprs_fixed", cfg.fixed_beacon_without_gps ? "1" : "0");
    appendField("aprs_nrl_tx", cfg.nrl_tx_enabled ? "1" : "0");
    appendField("aprs_nrl_rx", cfg.nrl_rx_enabled ? "1" : "0");
    appendField("aprs_fwd_rf_is", cfg.fwd[APRS_FWD_RF_TO_IS] ? "1" : "0");
    appendField("aprs_fwd_is_rf", cfg.fwd[APRS_FWD_IS_TO_RF] ? "1" : "0");
    appendField("aprs_fwd_nrl_is", cfg.fwd[APRS_FWD_NRL_TO_IS] ? "1" : "0");
    appendField("aprs_fwd_is_nrl", cfg.fwd[APRS_FWD_IS_TO_NRL] ? "1" : "0");
    appendField("aprs_fwd_rf_nrl", cfg.fwd[APRS_FWD_RF_TO_NRL] ? "1" : "0");
    appendField("aprs_fwd_nrl_rf", cfg.fwd[APRS_FWD_NRL_TO_RF] ? "1" : "0");
    char lat[16] = {};
    char lon[16] = {};
    APRS_SERVICE_FormatAprsCoord(static_cast<double>(cfg.default_lat_e6) / 1e6,
                                 true, lat, sizeof(lat));
    APRS_SERVICE_FormatAprsCoord(static_cast<double>(cfg.default_lon_e6) / 1e6,
                                 false, lon, sizeof(lon));
    appendField("aprs_lat", lat);
    appendField("aprs_lon", lon);
    body += "}}";
    s_server.send(200, "application/json; charset=utf-8", body);
}

static esp_err_t handleSaveAprs(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    bool ok = true;

    if (s_server.hasArg("aprs_present")) {
        ok = APRS_SERVICE_SetEnabled(s_server.hasArg("aprs_enabled"));
    }
    if (ok && s_server.hasArg("aprs_net_present")) {
        ok = APRS_SERVICE_SetNetEnabled(s_server.hasArg("aprs_net"));
    }
    if (ok && s_server.hasArg("aprs_tx_present")) {
        ok = APRS_SERVICE_SetRfTxEnabled(s_server.hasArg("aprs_tx"));
    }
    if (ok && s_server.hasArg("aprs_rx_present")) {
        ok = APRS_SERVICE_SetRfRxEnabled(s_server.hasArg("aprs_rx"));
    }
    if (ok && s_server.hasArg("aprs_auto_present")) {
        ok = APRS_SERVICE_SetAutoInterval(s_server.hasArg("aprs_auto"));
    }
    if (ok && s_server.hasArg("aprs_fixed_present")) {
        ok = APRS_SERVICE_SetFixedBeaconWithoutGps(s_server.hasArg("aprs_fixed"));
    }
    if (ok && s_server.hasArg("aprs_nrl_tx_present")) {
        ok = APRS_SERVICE_SetNrlTxEnabled(s_server.hasArg("aprs_nrl_tx"));
    }
    if (ok && s_server.hasArg("aprs_nrl_rx_present")) {
        ok = APRS_SERVICE_SetNrlRxEnabled(s_server.hasArg("aprs_nrl_rx"));
    }
    if (ok && s_server.hasArg("aprs_fwd_rf_is_present")) {
        ok = APRS_SERVICE_SetFwdEnabled(APRS_FWD_RF_TO_IS, s_server.hasArg("aprs_fwd_rf_is"));
    }
    if (ok && s_server.hasArg("aprs_fwd_is_rf_present")) {
        ok = APRS_SERVICE_SetFwdEnabled(APRS_FWD_IS_TO_RF, s_server.hasArg("aprs_fwd_is_rf"));
    }
    if (ok && s_server.hasArg("aprs_fwd_nrl_is_present")) {
        ok = APRS_SERVICE_SetFwdEnabled(APRS_FWD_NRL_TO_IS, s_server.hasArg("aprs_fwd_nrl_is"));
    }
    if (ok && s_server.hasArg("aprs_fwd_is_nrl_present")) {
        ok = APRS_SERVICE_SetFwdEnabled(APRS_FWD_IS_TO_NRL, s_server.hasArg("aprs_fwd_is_nrl"));
    }
    if (ok && s_server.hasArg("aprs_fwd_rf_nrl_present")) {
        ok = APRS_SERVICE_SetFwdEnabled(APRS_FWD_RF_TO_NRL, s_server.hasArg("aprs_fwd_rf_nrl"));
    }
    if (ok && s_server.hasArg("aprs_fwd_nrl_rf_present")) {
        ok = APRS_SERVICE_SetFwdEnabled(APRS_FWD_NRL_TO_RF, s_server.hasArg("aprs_fwd_nrl_rf"));
    }
    if (ok && s_server.hasArg("aprs_server")) {
        unsigned long port = 14580UL;
        ok = parseUIntArg(s_server.arg("aprs_port"), &port) && port > 0UL && port <= 65535UL &&
             APRS_SERVICE_SetServer(s_server.arg("aprs_server").c_str(),
                                    static_cast<uint16_t>(port));
    }
    if (ok && s_server.hasArg("aprs_ssid")) {
        unsigned long ssid = 0UL;
        ok = parseUIntArg(s_server.arg("aprs_ssid"), &ssid) && ssid <= 15UL &&
             APRS_SERVICE_SetSsid(static_cast<uint8_t>(ssid));
    }
    if (ok && s_server.hasArg("aprs_symbol")) {
        const std::string symbol = s_server.arg("aprs_symbol");
        ok = symbol.size() == 2u && APRS_SERVICE_SetSymbol(symbol[0], symbol[1]);
    }
    if (ok && s_server.hasArg("aprs_interval")) {
        unsigned long seconds = 0UL;
        ok = parseUIntArg(s_server.arg("aprs_interval"), &seconds) && seconds <= 65535UL &&
             APRS_SERVICE_SetBeaconInterval(static_cast<uint16_t>(seconds));
    }
    if (ok && s_server.hasArg("aprs_lat") && s_server.hasArg("aprs_lon")) {
        double lat = 0.0;
        double lon = 0.0;
        ok = APRS_SERVICE_ParseAprsCoord(s_server.arg("aprs_lat").c_str(), true, &lat) &&
             APRS_SERVICE_ParseAprsCoord(s_server.arg("aprs_lon").c_str(), false, &lon) &&
             APRS_SERVICE_SetDefaultPosition(lat, lon);
    }
    if (ok && s_server.hasArg("aprs_path")) {
        ok = APRS_SERVICE_SetPath(s_server.arg("aprs_path").c_str());
    }
    if (ok && s_server.hasArg("aprs_comment")) {
        ok = APRS_SERVICE_SetComment(s_server.arg("aprs_comment").c_str());
    }
    if (ok && s_server.hasArg("aprs_beacon")) {
        ok = APRS_SERVICE_SendBeaconNow();
    }
    if (ok && s_server.hasArg("aprs_msg")) {
        // Manual text message: both fields required, the service validates
        // the addressee charset and the message length/encoding.
        ok = s_server.hasArg("aprs_msg_dest") && s_server.hasArg("aprs_msg_text") &&
             APRS_SERVICE_SendMessage(s_server.arg("aprs_msg_dest").c_str(),
                                      s_server.arg("aprs_msg_text").c_str());
    }

    if (!ok) {
        ESP_LOGE(TAG, "APRS config save via web failed (invalid params)");
    }
    sendAprsSavedJson(ok);
    return ESP_OK;
}

// JSON feed for the stations-heard table: full APRS info per station plus
// service status, polled by the /aprs page every few seconds.
static esp_err_t handleAprsStations(httpd_req_t *req)
{
    s_server.bind(req);
    // Filled only while handling a GET; keep it in PSRAM, not internal RAM.
    NRL_PSRAM_BSS static AprsStationInfo stations[32];
    const size_t count = APRS_SERVICE_GetStations(stations, 32);
    AprsGpsInfo gps{};
    APRS_SERVICE_GetGpsInfo(&gps);

    std::string body;
    body.reserve(4096);
    body += "{\"net\":";
    body += APRS_SERVICE_IsNetConnected() ? "true" : "false";
    body += ",\"gps\":";
    body += gps.has_fix ? "true" : "false";
    body += ",\"rx\":" + std::to_string(APRS_SERVICE_GetRxCount());
    body += ",\"tx\":" + std::to_string(APRS_SERVICE_GetTxCount());
    body += ",\"gps_info\":{\"enabled\":";
    body += gps.uart_enabled ? "true" : "false";
    body += ",\"connected\":";
    body += gps.connected ? "true" : "false";
    body += ",\"fix\":";
    body += gps.has_fix ? "true" : "false";
    body += ",\"quality\":" + std::to_string(gps.fix_quality);
    body += ",\"age_ms\":" + std::to_string(gps.age_ms);
    if (gps.satellites >= 0) {
        body += ",\"satellites\":" + std::to_string(gps.satellites);
    }
    if (gps.visible_satellites >= 0) {
        body += ",\"visible_satellites\":" + std::to_string(gps.visible_satellites);
        body += ",\"gsv_age_ms\":" + std::to_string(gps.gsv_age_ms);
    }
    char num[128];
    if (gps.has_fix && isfinite(gps.latitude) && isfinite(gps.longitude)) {
        snprintf(num, sizeof(num), ",\"lat\":%.6f,\"lon\":%.6f",
                 gps.latitude, gps.longitude);
        body += num;
    }
    if (isfinite(gps.altitude_m)) {
        snprintf(num, sizeof(num), ",\"alt\":%.1f", gps.altitude_m);
        body += num;
    }
    if (isfinite(gps.speed_kmh)) {
        snprintf(num, sizeof(num), ",\"speed\":%.1f",
                 static_cast<double>(gps.speed_kmh));
        body += num;
    }
    if (gps.course_valid) {
        body += ",\"course\":" + std::to_string(gps.course_deg);
    }
    if (isfinite(gps.hdop)) {
        snprintf(num, sizeof(num), ",\"hdop\":%.1f",
                 static_cast<double>(gps.hdop));
        body += num;
    }
    body += ",\"satellite_signals\":[";
    for (size_t i = 0; i < gps.satellite_detail_count; ++i) {
        const AprsGpsSatelliteInfo &satellite = gps.satellite_details[i];
        if (i > 0) body += ",";
        snprintf(num, sizeof(num),
                 "{\"talker\":\"%.2s\",\"prn\":%u,\"elevation\":%d,"
                 "\"azimuth\":%d,\"snr\":%d}",
                 satellite.talker, static_cast<unsigned>(satellite.prn),
                 static_cast<int>(satellite.elevation_deg),
                 static_cast<int>(satellite.azimuth_deg),
                 static_cast<int>(satellite.snr_dbhz));
        body += num;
    }
    body += "]";
    body += "}";
    body += ",\"stations\":[";
    for (size_t i = 0; i < count; ++i) {
        const AprsStationInfo &s = stations[i];
        if (i > 0) {
            body += ",";
        }
        body += "{\"name\":\"" + jsonEscape(s.name) + "\"";
        snprintf(num, sizeof(num), ",\"lat\":%.5f,\"lon\":%.5f",
                 static_cast<double>(s.lat), static_cast<double>(s.lon));
        body += num;
        if (!isnan(s.altitude_m)) {
            snprintf(num, sizeof(num), ",\"alt\":%.0f", static_cast<double>(s.altitude_m));
            body += num;
        }
        if (!isnan(s.distance_km)) {
            snprintf(num, sizeof(num), ",\"dist\":%.1f,\"brg\":%u",
                     static_cast<double>(s.distance_km), static_cast<unsigned>(s.bearing_deg));
            body += num;
        }
        if (!isnan(s.speed_kmh)) {
            snprintf(num, sizeof(num), ",\"spd\":%.1f", static_cast<double>(s.speed_kmh));
            body += num;
        }
        if (!isnan(s.derived_speed_kmh)) {
            snprintf(num, sizeof(num), ",\"dspd\":%.1f", static_cast<double>(s.derived_speed_kmh));
            body += num;
        }
        if (s.course_deg > 0) {
            snprintf(num, sizeof(num), ",\"crs\":%u", static_cast<unsigned>(s.course_deg));
            body += num;
        }
        body += ",\"rf\":";
        body += s.via_rf ? "true" : "false";
        body += ",\"age\":" + std::to_string(s.age_s);
        body += ",\"pkts\":" + std::to_string(s.pkt_count);
        body += ",\"cmt\":\"" + jsonEscape(s.comment) + "\"";
        body += ",\"sym\":\"" + jsonEscape(s.symbol) + "\"}";
    }
    body += "]}";
    s_server.send(200, "application/json; charset=utf-8", body);
    return ESP_OK;
}

static esp_err_t handleScan(httpd_req_t *req)
{
    s_server.bind(req);
    // Serve the cache captured before the AP started. No live scan -- a live
    // scan hops channels and drops the portal client that is connected now.
    std::string json = "[";
    for (int i = 0; i < s_wifi_scan_count; ++i) {
        if (i != 0) {
            json += ",";
        }
        const std::string escaped_ssid = jsonEscape(s_wifi_scan_cache[i].ssid);
        json += "{\"ssid\":\"";
        json += escaped_ssid;
        json += "\",\"label\":\"";
        json += escaped_ssid;
        json += " (";
        json += std::to_string(s_wifi_scan_cache[i].rssi);
        json += " dBm)\"}";
    }
    json += "]";
    s_server.send(200, "application/json; charset=utf-8", json);
    return ESP_OK;
}

static bool parseUIntArg(const std::string &text, unsigned long *out_value)
{
    if (out_value == nullptr || text.empty()) {
        return false;
    }

    char *end = nullptr;
    const unsigned long value = strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != '\0')) {
        return false;
    }

    *out_value = value;
    return true;
}

static bool parseMicPcmGainArg(const std::string &text, uint16_t *out_milli)
{
    if (out_milli == nullptr || text.empty()) return false;
    char *end = nullptr;
    const float gain = strtof(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr || *end != '\0' || !isfinite(gain) ||
        gain < 0.1f || gain > 5.0f) {
        return false;
    }
    const long milli = lroundf(gain * 1000.0f);
    if (milli < 100l || milli > 5000l) return false;
    *out_milli = static_cast<uint16_t>(milli);
    return true;
}

static bool parseIpArg(const std::string &text, uint32_t *out_value)
{
    if (out_value == nullptr || text.empty()) {
        return false;
    }
    struct in_addr addr = {};
    if (inet_aton(text.c_str(), &addr) == 0) {
        return false;
    }
    *out_value = addr.s_addr;
    return true;
}

static esp_err_t handleSaveWifi(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    // Diagnostic: log Content-Type, body size and parsed param count so we
    // can tell whether the browser is sending urlencoded (parsed) or
    // multipart (one giant unparsed blob). Drop once form parsing is stable.
    ESP_LOGI(TAG, "save_wifi: ct=\"%s\" body=%u parsed=%d args:",
             s_server.ct_header,
             static_cast<unsigned>(req->content_len),
             s_server.args());
    for (int i = 0; i < s_server.args(); ++i) {
        ESP_LOGI(TAG, "  [%d] %s = (%u chars)",
                 i,
                 s_server.argName(i).c_str(),
                 static_cast<unsigned>(s_server.arg(s_server.argName(i).c_str()).size()));
    }
    bool ok = true;

    ExternalRadioConfig before_snapshot = {};
    bool have_snapshot = false;
    if (const ExternalRadioConfig *p = EXTERNAL_RADIO_GetConfig()) {
        before_snapshot = *p;
        have_snapshot = true;
    }

    bool wifi_profiles_changed = false;
    if (s_server.hasArg("wifi_profile_index")) {
        const std::string index_text = s_server.arg("wifi_profile_index");
        char *end = nullptr;
        const unsigned long index = strtoul(index_text.c_str(), &end, 10);
        const bool valid_index = end != nullptr && *end == '\0' &&
                                 index < EXTERNAL_RADIO_MAX_WIFI_PROFILES;
        if (valid_index && s_server.hasArg("wifi_delete")) {
            ok = EXTERNAL_RADIO_RemoveWifiProfile(static_cast<size_t>(index), false);
        } else if (valid_index && s_server.hasArg("wifi_move_up")) {
            ok = EXTERNAL_RADIO_MoveWifiProfile(static_cast<size_t>(index), -1, false);
        } else if (valid_index && s_server.hasArg("wifi_move_down")) {
            ok = EXTERNAL_RADIO_MoveWifiProfile(static_cast<size_t>(index), 1, false);
        } else {
            ok = false;
        }
        wifi_profiles_changed = ok;
    } else if (s_server.hasArg("wifi_ssid")) {
        const std::string password = s_server.hasArg("wifi_password")
                                         ? s_server.arg("wifi_password") : std::string();
        ok = EXTERNAL_RADIO_AddWifiProfile(s_server.arg("wifi_ssid").c_str(),
                                           password.c_str(), false);
        wifi_profiles_changed = ok;
    }
    if (ok && s_server.hasArg("wifi_dhcp_present")) {
        ok = EXTERNAL_RADIO_SetWifiDhcpEnabled(s_server.hasArg("wifi_dhcp_enabled"), false);
    }
    if (ok && s_server.hasArg("wifi_ip") && s_server.arg("wifi_ip").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_ip"), &value) &&
             EXTERNAL_RADIO_SetWifiIp(value, false);
    }
    if (ok && s_server.hasArg("wifi_mask") && s_server.arg("wifi_mask").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_mask"), &value) &&
             EXTERNAL_RADIO_SetWifiNetmask(value, false);
    }
    if (ok && s_server.hasArg("wifi_gateway") && s_server.arg("wifi_gateway").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_gateway"), &value) &&
             EXTERNAL_RADIO_SetWifiGateway(value, false);
    }
    if (ok && s_server.hasArg("wifi_dns") && s_server.arg("wifi_dns").length() > 0) {
        uint32_t value = 0u;
        ok = parseIpArg(s_server.arg("wifi_dns"), &value) &&
             EXTERNAL_RADIO_SetWifiDns(value, false);
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SaveConfig();
    }

    const ExternalRadioConfig *after = EXTERNAL_RADIO_GetConfig();
    if (ok && after != nullptr && have_snapshot) {
        logChangedFields(&before_snapshot, after);
        const bool restart_wifi = wifi_profiles_changed ||
                                  strcmp(before_snapshot.wifi_ssid, after->wifi_ssid) != 0 ||
                                  strcmp(before_snapshot.wifi_password, after->wifi_password) != 0 ||
                                  before_snapshot.wifi_dhcp_enabled != after->wifi_dhcp_enabled ||
                                  before_snapshot.wifi_ip != after->wifi_ip ||
                                  before_snapshot.wifi_netmask != after->wifi_netmask ||
                                  before_snapshot.wifi_gateway != after->wifi_gateway ||
                                  before_snapshot.wifi_dns != after->wifi_dns;
        if (restart_wifi) {
            ESP_LOGI(TAG,"[CFG] WiFi credentials changed, reconnecting to \"%s\"\n", after->wifi_ssid);
            NRLAudioBridge_ApplyConfig(true, true);
        }
    } else if (!ok) {
        ESP_LOGE(TAG, "WiFi config save via web failed (invalid params or EEPROM write error)");
    }

    sendSavedFieldsJson(ok);
    return ESP_OK;
}

static esp_err_t handleSaveNrl(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    bool ok = true;

    ExternalRadioConfig before_snapshot = {};
    bool have_snapshot = false;
    if (const ExternalRadioConfig *p = EXTERNAL_RADIO_GetConfig()) {
        before_snapshot = *p;
        have_snapshot = true;
    }

    const bool reset_audio_defaults = s_server.hasArg("audio_reset_defaults");
    if (ok && reset_audio_defaults) {
        ok = EXTERNAL_RADIO_ResetAudioConfig(false);
    }
    if (ok && !reset_audio_defaults) {
    if (ok && s_server.hasArg("server_host")) {
        ok = EXTERNAL_RADIO_SetServerHost(s_server.arg("server_host").c_str(), false);
    }
    if (ok && s_server.hasArg("server_port")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("server_port"), &value) &&
             value > 0UL && value <= 65535UL &&
             EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(value), false);
    }
    if (ok && s_server.hasArg("channel")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("channel"), &value) &&
             value <= 7UL &&
             EXTERNAL_RADIO_SetChannel(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("callsign")) {
        ok = EXTERNAL_RADIO_SetCallsign(s_server.arg("callsign").c_str(), false);
    }
    if (ok && s_server.hasArg("callsign_ssid")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("callsign_ssid"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("ptt_timeout")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("ptt_timeout"), &value) &&
             value >= 5UL && value <= 3600UL &&
             EXTERNAL_RADIO_SetPttTimeout(static_cast<uint16_t>(value), false);
    }
    if (ok && s_server.hasArg("voice_payload_bytes")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("voice_payload_bytes"), &value) &&
             value >= 160UL && value <= 500UL &&
             EXTERNAL_RADIO_SetVoicePayloadBytes(static_cast<uint16_t>(value), false);
    }
    if (ok && s_server.hasArg("voice_codec")) {
        // NRL TX codec sits in the Radio form. Fails (and rolls back to G.711)
        // when Opus pre-allocation cannot get RAM; surfaced as a failed save.
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("voice_codec"), &value) && value <= 1UL &&
             NRLAudioBridge_SetVoiceCodec(static_cast<uint8_t>(value));
    }
    if (ok && s_server.hasArg("tail_suppress_ms")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("tail_suppress_ms"), &value) &&
             value <= 5000UL &&
             EXTERNAL_RADIO_SetTailSuppressMs(static_cast<uint16_t>(value), false);
    }
#if NRL_BOARD_IS_GEZIPAI_FAMILY
    // Auto-calibrate the battery sense from a multimeter reading. Empty value
    // skips it (so the form can be submitted with only the manual scale).
    if (ok && s_server.hasArg("battery_actual_mv") && !s_server.arg("battery_actual_mv").empty()) {
        unsigned long actual_mv = 0UL;
        const int raw_mv = Display_GetBatteryRawMv();
        if (parseUIntArg(s_server.arg("battery_actual_mv"), &actual_mv) &&
            actual_mv >= 1000UL && actual_mv <= 9000UL && raw_mv > 0) {
            const unsigned long scale = (actual_mv * 1000UL + static_cast<unsigned long>(raw_mv) / 2UL) /
                                        static_cast<unsigned long>(raw_mv);
            ok = scale >= 500UL && scale <= 2000UL &&
                 EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(scale), false);
        } else {
            ok = false;
        }
    }
    if (ok && s_server.hasArg("battery_cal_milli")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("battery_cal_milli"), &value) &&
             value >= 500UL && value <= 2000UL &&
             EXTERNAL_RADIO_SetBatteryCalibration(static_cast<uint16_t>(value), false);
    }
#endif
    if (ok && s_server.hasArg("mic_volume")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("mic_volume"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("mic_pcm_gain")) {
        uint16_t gain_milli = 0u;
        ok = parseMicPcmGainArg(s_server.arg("mic_pcm_gain"), &gain_milli) &&
             EXTERNAL_RADIO_SetMicPcmGain(gain_milli, false);
    }
    if (ok && s_server.hasArg("line_out_volume")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("line_out_volume"), &value) &&
             value <= 255UL &&
             EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("hp_drive_present")) {
        ok = EXTERNAL_RADIO_SetHpDriveEnabled(s_server.hasArg("hp_drive_enabled"), false);
    }
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    if (ok && s_server.hasArg("aec_present")) {
        ok = EXTERNAL_RADIO_SetAecEnabled(s_server.hasArg("aec_enabled"), false);
    }
    if (ok && s_server.hasArg("aec_reference_source")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("aec_reference_source"), &value) &&
             value <= EXTERNAL_RADIO_AEC_REF_MIC &&
             EXTERNAL_RADIO_SetAecReferenceSource(static_cast<uint8_t>(value), false);
    }
#endif
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    if (ok && s_server.hasArg("ai_noise_present")) {
        ok = EXTERNAL_RADIO_SetAiNoiseEnabled(s_server.hasArg("ai_noise_enabled"), false);
    }
#endif
    if (ok && s_server.hasArg("mic_hpf_enabled_present")) {
        ok = EXTERNAL_RADIO_SetMicHpfEnabled(s_server.hasArg("mic_hpf_enabled"), false);
    }
    if (ok && s_server.hasArg("drc_present")) {
        ok = EXTERNAL_RADIO_SetDrcEnabled(s_server.hasArg("drc_enabled"), false);
    }
    if (ok && s_server.hasArg("drc_winsize")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("drc_winsize"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDrcWinsize(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("drc_maxlevel")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("drc_maxlevel"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDrcMaxlevel(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("drc_minlevel")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("drc_minlevel"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDrcMinlevel(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("dac_ramprate")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("dac_ramprate"), &value) &&
             value <= 15UL &&
             EXTERNAL_RADIO_SetDacRamprate(static_cast<uint8_t>(value), false);
    }
    if (ok && s_server.hasArg("dac_eq_bypass_present")) {
        ok = EXTERNAL_RADIO_SetDacEqBypass(s_server.hasArg("dac_eq_bypass"), false);
    }
    if (ok && (s_server.hasArg("daceq_b0") || s_server.hasArg("daceq_b1") || s_server.hasArg("daceq_a1"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint32_t b0 = current != nullptr ? current->daceq_b0 : 0u;
        uint32_t b1 = current != nullptr ? current->daceq_b1 : 0u;
        uint32_t a1 = current != nullptr ? current->daceq_a1 : 0u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("daceq_b0")) {
            ok = parseUIntArg(s_server.arg("daceq_b0"), &value) && value <= kDacEqCoefficientMax;
            b0 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("daceq_b1")) {
            ok = parseUIntArg(s_server.arg("daceq_b1"), &value) && value <= kDacEqCoefficientMax;
            b1 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("daceq_a1")) {
            ok = parseUIntArg(s_server.arg("daceq_a1"), &value) && value <= kDacEqCoefficientMax;
            a1 = static_cast<uint32_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetDacEqCoefficients(b0, b1, a1, false);
        }
    }
    if (ok && (s_server.hasArg("adc_dmic_present") ||
               s_server.hasArg("adc_linsel_present") ||
               s_server.hasArg("adc_pga_gain"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool dmic = current != nullptr && current->adc_dmic_enabled;
        bool linsel = current == nullptr || current->adc_linsel;
        uint8_t pga = current != nullptr ? current->adc_pga_gain : 10u;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_dmic_enabled")) {
            dmic = s_server.arg("adc_dmic_enabled") == "1";
        } else if (s_server.hasArg("adc_dmic_present")) {
            dmic = false;
        }
        if (s_server.hasArg("adc_linsel")) {
            linsel = s_server.arg("adc_linsel") == "1";
        } else if (s_server.hasArg("adc_linsel_present")) {
            linsel = false;
        }
        if (ok && s_server.hasArg("adc_pga_gain")) {
            ok = parseUIntArg(s_server.arg("adc_pga_gain"), &value) && value <= 10UL;
            pga = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcSystemConfig(dmic, linsel, pga, false);
        }
    }
    if (ok && (s_server.hasArg("adc_dmic_sense_present") || s_server.hasArg("adc_ramprate"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint8_t ramprate = current != nullptr ? current->adc_ramprate : 4u;
        bool dmic_sense = current != nullptr && current->adc_dmic_sense;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_ramprate")) {
            ok = parseUIntArg(s_server.arg("adc_ramprate"), &value) && value <= 15UL;
            ramprate = static_cast<uint8_t>(value);
        }
        if (s_server.hasArg("adc_dmic_sense")) {
            dmic_sense = s_server.arg("adc_dmic_sense") == "1";
        } else if (s_server.hasArg("adc_dmic_sense_present")) {
            dmic_sense = false;
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcRampConfig(ramprate, dmic_sense, false);
        }
    }
    if (ok && (s_server.hasArg("adc_sync_present") ||
               s_server.hasArg("adc_inv_present") ||
               s_server.hasArg("adc_ramclr_present") ||
               s_server.hasArg("adc_scale"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool sync = current == nullptr || current->adc_sync;
        bool inv = current != nullptr && current->adc_inv;
        bool ramclr = current != nullptr && current->adc_ramclr;
        uint8_t scale = current != nullptr ? current->adc_scale : 4u;
        unsigned long value = 0UL;
        if (s_server.hasArg("adc_sync")) {
            sync = s_server.arg("adc_sync") == "1";
        } else if (s_server.hasArg("adc_sync_present")) {
            sync = false;
        }
        if (s_server.hasArg("adc_inv")) {
            inv = s_server.arg("adc_inv") == "1";
        } else if (s_server.hasArg("adc_inv_present")) {
            inv = false;
        }
        if (s_server.hasArg("adc_ramclr")) {
            ramclr = s_server.arg("adc_ramclr") == "1";
        } else if (s_server.hasArg("adc_ramclr_present")) {
            ramclr = false;
        }
        if (ok && s_server.hasArg("adc_scale")) {
            ok = parseUIntArg(s_server.arg("adc_scale"), &value) && value <= 7UL;
            scale = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcScaleConfig(sync, inv, ramclr, scale, false);
        }
    }
    if (ok && (s_server.hasArg("alc_enabled_present") ||
               s_server.hasArg("adc_automute_enabled_present") ||
               s_server.hasArg("alc_winsize") ||
               s_server.hasArg("alc_maxlevel") ||
               s_server.hasArg("alc_minlevel"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        bool alc = current != nullptr && current->alc_enabled;
        bool automute = current != nullptr && current->adc_automute_enabled;
        uint8_t winsize = current != nullptr ? current->alc_winsize : 0u;
        uint8_t maxlevel = current != nullptr ? current->alc_maxlevel : 0u;
        uint8_t minlevel = current != nullptr ? current->alc_minlevel : 0u;
        unsigned long value = 0UL;
        if (s_server.hasArg("alc_enabled")) {
            alc = s_server.arg("alc_enabled") == "1";
        } else if (s_server.hasArg("alc_enabled_present")) {
            alc = false;
        }
        if (s_server.hasArg("adc_automute_enabled")) {
            automute = s_server.arg("adc_automute_enabled") == "1";
        } else if (s_server.hasArg("adc_automute_enabled_present")) {
            automute = false;
        }
        if (ok && s_server.hasArg("alc_winsize")) {
            ok = parseUIntArg(s_server.arg("alc_winsize"), &value) && value <= 15UL;
            winsize = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("alc_maxlevel")) {
            ok = parseUIntArg(s_server.arg("alc_maxlevel"), &value) && value <= 15UL;
            maxlevel = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("alc_minlevel")) {
            ok = parseUIntArg(s_server.arg("alc_minlevel"), &value) && value <= 15UL;
            minlevel = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAlcConfig(alc, automute, winsize, maxlevel, minlevel, false);
        }
    }
    if (ok && (s_server.hasArg("adc_automute_winsize") ||
               s_server.hasArg("adc_automute_noise_gate") ||
               s_server.hasArg("adc_automute_volume"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint8_t winsize = current != nullptr ? current->adc_automute_winsize : 0u;
        uint8_t noise_gate = current != nullptr ? current->adc_automute_noise_gate : 0u;
        uint8_t volume = current != nullptr ? current->adc_automute_volume : 0u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("adc_automute_winsize")) {
            ok = parseUIntArg(s_server.arg("adc_automute_winsize"), &value) && value <= 15UL;
            winsize = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("adc_automute_noise_gate")) {
            ok = parseUIntArg(s_server.arg("adc_automute_noise_gate"), &value) && value <= 15UL;
            noise_gate = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("adc_automute_volume")) {
            ok = parseUIntArg(s_server.arg("adc_automute_volume"), &value) && value <= 7UL;
            volume = static_cast<uint8_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcAutomuteConfig(winsize, noise_gate, volume, false);
        }
    }
    if (ok && (s_server.hasArg("adc_eq_bypass_present") ||
               s_server.hasArg("adc_hpf_present") ||
               s_server.hasArg("adc_hpfs1") ||
               s_server.hasArg("adc_hpfs2"))) {
        // Each REG1C-affecting control posts only its own _present marker
        // and its own checkbox; the other two stay in the saved config and
        // are preserved via init-from-current. The previous design had both
        // EQ-bypass and Dynamic-HPF forms posting a shared adc_hpf_present
        // plus stale cross-form hidden mirrors of the other checkbox, which
        // could revert a freshly-unchecked checkbox the next time its
        // sibling was toggled.
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint8_t hpfs1 = current != nullptr ? current->adc_hpfs1 : 10u;
        bool eq_bypass = current == nullptr || current->adc_eq_bypass;
        bool hpf = current == nullptr || current->adc_hpf;
        uint8_t hpfs2 = current != nullptr ? current->adc_hpfs2 : 10u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("adc_hpfs1")) {
            ok = parseUIntArg(s_server.arg("adc_hpfs1"), &value) && value <= 31UL;
            hpfs1 = static_cast<uint8_t>(value);
        }
        if (ok && s_server.hasArg("adc_hpfs2")) {
            ok = parseUIntArg(s_server.arg("adc_hpfs2"), &value) && value <= 31UL;
            hpfs2 = static_cast<uint8_t>(value);
        }
        if (s_server.hasArg("adc_eq_bypass")) {
            eq_bypass = s_server.arg("adc_eq_bypass") == "1";
        } else if (s_server.hasArg("adc_eq_bypass_present")) {
            eq_bypass = false;
        }
        if (s_server.hasArg("adc_hpf")) {
            hpf = s_server.arg("adc_hpf") == "1";
        } else if (s_server.hasArg("adc_hpf_present")) {
            hpf = false;
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcHpfConfig(hpfs1, eq_bypass, hpf, hpfs2, false);
        }
    }
    if (ok && (s_server.hasArg("adceq_b0") ||
               s_server.hasArg("adceq_a1") ||
               s_server.hasArg("adceq_a2") ||
               s_server.hasArg("adceq_b1") ||
               s_server.hasArg("adceq_b2"))) {
        const ExternalRadioConfig *current = EXTERNAL_RADIO_GetConfig();
        uint32_t b0 = current != nullptr ? current->adceq_b0 : 0u;
        uint32_t a1 = current != nullptr ? current->adceq_a1 : 0u;
        uint32_t a2 = current != nullptr ? current->adceq_a2 : 0u;
        uint32_t b1 = current != nullptr ? current->adceq_b1 : 0u;
        uint32_t b2 = current != nullptr ? current->adceq_b2 : 0u;
        unsigned long value = 0UL;
        if (ok && s_server.hasArg("adceq_b0")) {
            ok = parseUIntArg(s_server.arg("adceq_b0"), &value) && value <= kDacEqCoefficientMax;
            b0 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_a1")) {
            ok = parseUIntArg(s_server.arg("adceq_a1"), &value) && value <= kDacEqCoefficientMax;
            a1 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_a2")) {
            ok = parseUIntArg(s_server.arg("adceq_a2"), &value) && value <= kDacEqCoefficientMax;
            a2 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_b1")) {
            ok = parseUIntArg(s_server.arg("adceq_b1"), &value) && value <= kDacEqCoefficientMax;
            b1 = static_cast<uint32_t>(value);
        }
        if (ok && s_server.hasArg("adceq_b2")) {
            ok = parseUIntArg(s_server.arg("adceq_b2"), &value) && value <= kDacEqCoefficientMax;
            b2 = static_cast<uint32_t>(value);
        }
        if (ok) {
            ok = EXTERNAL_RADIO_SetAdcEqCoefficients(b0, a1, a2, b1, b2, false);
        }
    }
    }
    if (ok) {
        ok = EXTERNAL_RADIO_SaveConfig();
    }

    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (ok && config != nullptr && have_snapshot) {
        logChangedFields(&before_snapshot, config);
#if defined(NRL_AUDIO_CODEC_ES8311) && NRL_AUDIO_CODEC_ES8311
        ES8311_ApplyAudioConfig(config->mic_volume,
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
#endif
        const bool restart_udp = strcmp(before_snapshot.server_host, config->server_host) != 0 ||
                                 before_snapshot.server_port != config->server_port;
        if (restart_udp) {
            ESP_LOGI(TAG,"[CFG] server address changed, rebuilding UDP connection to %s:%u\n",
                          config->server_host, static_cast<unsigned>(config->server_port));
            NRLAudioBridge_ApplyConfig(false, true);
        }
    } else if (!ok) {
        ESP_LOGE(TAG, "NRL config save via web failed (invalid params or EEPROM write error)");
    }

    sendSavedFieldsJson(ok);
    return ESP_OK;
}

// Echo the full media config back as {"ok":..,"fields":{..}} from live
// state, so the page always reflects on-device truth after a save (the
// media fields live outside ExternalRadioConfig, so sendSavedFieldsJson
// can't serve them).
static void sendMediaSavedJson(const bool ok)
{
    char beacon_path[128] = {};
    uint32_t beacon_interval = 0;
    const bool beacon_armed = NANNY_GetBeacon(beacon_path, sizeof(beacon_path), &beacon_interval);
    char radio_url[256] = {};
    MUSIC_GetRadioUrl(radio_url, sizeof(radio_url));
    char smb_server[64] = {};
    char smb_share[64] = {};
    char smb_user[32] = {};
    char smb_pass[64] = {};
    (void)STORAGE_SmbGetConfig(smb_server, sizeof(smb_server), smb_share, sizeof(smb_share),
                               smb_user, sizeof(smb_user), smb_pass, sizeof(smb_pass));

    std::string body;
    body.reserve(768);
    body += "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"fields\":{";
    auto appendField = [&body](const char *name, const std::string &value, const bool first = false) {
        if (!first) {
            body += ",";
        }
        body += "\"";
        body += name;
        body += "\":\"";
        body += jsonEscape(value);
        body += "\"";
    };
    appendField("music_target", std::to_string(MUSIC_GetTarget()), true);
    appendField("voice_codec", std::to_string(NRLAudioBridge_GetVoiceCodec()));
    appendField("espnow_enabled", ESPNOW_LINK_IsEnabled() ? "1" : "0");
    appendField("espnow_rx", ESPNOW_LINK_IsRxEnabled() ? "1" : "0");
    appendField("espnow_codec", std::to_string(ESPNOW_LINK_GetTxCodec()));
    appendField("ptt_mode", std::to_string(ESPNOW_LINK_GetPttMode()));
    appendField("beacon_enabled", beacon_armed ? "1" : "0");
    appendField("beacon_path", beacon_path);
    appendField("beacon_interval", beacon_armed ? std::to_string(beacon_interval) : std::string(""));
    appendField("radio_url", radio_url);
    appendField("smb_server", smb_server);
    appendField("smb_share", smb_share);
    appendField("smb_user", smb_user);
    appendField("smb_password", smb_pass);
    body += "}";
    // Favorite stations ride along on every media save so the page can
    // re-render the list after an add/delete/tune without a reload.
    body += ",\"favs\":[";
    const size_t fav_count = RADIO_FAV_Count();
    for (size_t i = 0; i < fav_count; ++i) {
        char fav_name[RADIO_FAV_NAME_SIZE] = {};
        char fav_url[RADIO_FAV_URL_SIZE] = {};
        if (!RADIO_FAV_Get(i, fav_name, sizeof(fav_name), fav_url, sizeof(fav_url))) {
            break;
        }
        if (i > 0u) {
            body += ",";
        }
        body += "{\"name\":\"" + jsonEscape(fav_name) +
                "\",\"url\":\"" + jsonEscape(fav_url) + "\"}";
    }
    body += "],\"fav_cur\":" + std::to_string(RADIO_FAV_CurrentIndex());
    body += "}";
    s_server.send(ok ? 200 : 400, "application/json; charset=utf-8", body);
}

static esp_err_t handleSaveMedia(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    bool ok = true;

    if (s_server.hasArg("music_target")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("music_target"), &value) &&
             value <= MUSIC_TARGET_BOTH;
        if (ok) {
            MUSIC_SetTarget(static_cast<int>(value));
            ESP_LOGI(TAG, "media: playback target=%lu", value);
        }
    }
    if (ok && s_server.hasArg("music_output")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("music_output"), &value) &&
             value <= MUSIC_OUTPUT_BT;
        if (ok) {
            MUSIC_SetOutput(static_cast<int>(value));
        }
    }
    if (ok && s_server.hasArg("voice_codec")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("voice_codec"), &value) && value <= 1UL;
        if (ok) {
            // Fails (and rolls back to G.711) when Opus pre-allocation cannot
            // get RAM; surface that as a failed save so the form shows truth.
            ok = NRLAudioBridge_SetVoiceCodec(static_cast<uint8_t>(value));
            ESP_LOGI(TAG, "media: voice codec=%s%s", value == 1UL ? "opus" : "g711",
                     ok ? "" : " (opus alloc failed, kept g711)");
        }
    }
    if (ok && s_server.hasArg("ptt_mode")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("ptt_mode"), &value) && value <= 2UL;
        if (ok) {
            if (value == 1UL && !ESPNOW_LINK_IsEnabled()) {
                ok = ESPNOW_LINK_SetEnabled(true);
            }
            if (ok && value == 2UL) {
                FmoConfig fmo = {};
                FMO_GetConfig(&fmo);
                ok = fmo.enabled;
                if (ok && !fmo.transmit) {
                    fmo.transmit = true;
                    ok = FMO_SetConfig(&fmo, true);
                }
            }
            if (ok) {
                ESPNOW_LINK_SetPttMode(static_cast<uint8_t>(value));
            }
            ESP_LOGI(TAG, "media: ptt mode=%s",
                     value == 2UL ? "fmo" : value == 1UL ? "espnow" : "nrl");
        }
    }
    if (ok && s_server.hasArg("espnow_present")) {
        // Fails when enabling while WiFi is still down -- surfaced to the page.
        ok = ESPNOW_LINK_SetEnabled(s_server.hasArg("espnow_enabled"));
    }
    if (ok && s_server.hasArg("espnow_rx_present")) {
        ESPNOW_LINK_SetRxEnabled(s_server.hasArg("espnow_rx"));
    }
    if (ok && s_server.hasArg("espnow_codec")) {
        unsigned long value = 0UL;
        ok = parseUIntArg(s_server.arg("espnow_codec"), &value) && value <= 1UL;
        if (ok) {
            // Fails (and rolls back to G.711) when Opus pre-allocation cannot
            // get RAM; surface that as a failed save so the form shows truth.
            ok = ESPNOW_LINK_SetTxCodec(static_cast<uint8_t>(value));
            ESP_LOGI(TAG, "media: espnow codec=%s%s", value == 1UL ? "opus" : "g711",
                     ok ? "" : " (opus alloc failed, kept g711)");
        }
    }
    if (ok && s_server.hasArg("ai_present")) {
        const std::string ai_url = s_server.arg("ai_url");
        if (!s_server.hasArg("ai_enabled")) {
            ok = AI_SetEnabled(false);
        } else if (!ai_url.empty()) {
            ok = AI_Configure(ai_url.c_str(), s_server.arg("ai_token").c_str());
        } else {
            ok = false; // enabling needs a URL
        }
    }
    if (ok && s_server.hasArg("beacon_present")) {
        if (s_server.hasArg("beacon_enabled")) {
            unsigned long minutes = 0UL;
            ok = parseUIntArg(s_server.arg("beacon_interval"), &minutes) &&
                 NANNY_SetBeacon(s_server.arg("beacon_path").c_str(),
                                 static_cast<uint32_t>(minutes));
        } else {
            NANNY_DisableBeacon();
        }
    }
    if (ok && s_server.hasArg("smb_clear")) {
        STORAGE_SmbClear();
        ESP_LOGI(TAG, "media: SMB config cleared");
    } else if (ok && s_server.hasArg("smb_server")) {
        const std::string server = s_server.arg("smb_server");
        const std::string share = s_server.arg("smb_share");
        if (server.empty() && share.empty()) {
            STORAGE_SmbClear();
        } else {
            ok = STORAGE_SmbConfigure(server.c_str(), share.c_str(),
                                      s_server.arg("smb_user").c_str(),
                                      s_server.arg("smb_password").c_str());
        }
    }
    if (ok && s_server.hasArg("radio_url")) {
        ok = MUSIC_SetRadioUrl(s_server.arg("radio_url").c_str());
    }
    if (ok && s_server.hasArg("radio_play")) {
        char url[256] = {};
        MUSIC_GetRadioUrl(url, sizeof(url));
        ok = url[0] != '\0' && MUSIC_PlayFile(url);
    }
    if (ok && s_server.hasArg("radio_stop")) {
        MUSIC_Stop();
    }
    if (ok && s_server.hasArg("fav_add")) {
        ok = RADIO_FAV_Set(-1, s_server.arg("fav_name").c_str(),
                           s_server.arg("fav_url").c_str(), nullptr);
    }
    if (ok && s_server.hasArg("fav_del")) {
        unsigned long index = 0UL;
        ok = parseUIntArg(s_server.arg("fav_del"), &index) &&
             RADIO_FAV_Remove(index);
    }
    if (ok && s_server.hasArg("fav_play")) {
        unsigned long index = 0UL;
        ok = parseUIntArg(s_server.arg("fav_play"), &index) &&
             RADIO_FAV_PlayIndex(index);
    }

    if (!ok) {
        ESP_LOGE(TAG, "media config save via web failed (invalid params)");
    }
    sendMediaSavedJson(ok);
    return ESP_OK;
}

constexpr size_t kWebPlaylistPageSize = 64u;

static const char *pathBasename(const char *path)
{
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return (slash != nullptr) ? slash + 1 : path;
}

static void sendPlaylistJson(const bool ok, size_t offset, const bool include_entries)
{
    const bool scanning = PLAYLIST_ClientIsScanning(PLAYLIST_CLIENT_WEB);
    const size_t track_count = PLAYLIST_ClientCount(PLAYLIST_CLIENT_WEB);
    const size_t dir_count = PLAYLIST_ClientDirCount(PLAYLIST_CLIENT_WEB);
    const size_t total = dir_count + track_count;
    if (offset >= total && total > 0u) {
        offset = ((total - 1u) / kWebPlaylistPageSize) * kWebPlaylistPageSize;
    }
    const size_t end = (offset + kWebPlaylistPageSize < total)
                           ? offset + kWebPlaylistPageSize
                           : total;
    const char *playing_path = MUSIC_CurrentPath();
    const bool playing = MUSIC_IsPlaying();

    std::string body;
    body.reserve(include_entries ? 16384u : 768u);
    body += "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"scanning\":";
    body += scanning ? "true" : "false";
    body += ",\"scan_ok\":";
    body += PLAYLIST_ClientLastScanOk(PLAYLIST_CLIENT_WEB) ? "true" : "false";
    body += ",\"root\":";
    body += PLAYLIST_ClientAtRoot(PLAYLIST_CLIENT_WEB) ? "true" : "false";
    body += ",\"dir\":\"" + jsonEscape(PLAYLIST_ClientCurrentDir(PLAYLIST_CLIENT_WEB)) + "\"";
    body += ",\"playing\":";
    body += playing ? "true" : "false";
    body += ",\"playing_path\":\"" + jsonEscape(playing_path) + "\"";
    body += ",\"repeat\":" + std::to_string(static_cast<int>(PLAYLIST_GetRepeatMode()));
    body += ",\"offset\":" + std::to_string(offset);
    body += ",\"page_size\":" + std::to_string(kWebPlaylistPageSize);
    body += ",\"total\":" + std::to_string(total);
    body += ",\"favorite_supported\":";
    body += STORAGE_SdMounted() ? "true" : "false";

    if (include_entries) {
        body += ",\"dirs\":[";
        const size_t dir_begin = offset < dir_count ? offset : dir_count;
        const size_t dir_end = end < dir_count ? end : dir_count;
        for (size_t i = dir_begin; i < dir_end; ++i) {
            const char *name = PLAYLIST_ClientGetDirName(PLAYLIST_CLIENT_WEB, i);
            if (i > dir_begin) {
                body += ",";
            }
            body += "{\"index\":" + std::to_string(i) +
                    ",\"name\":\"" + jsonEscape(name != nullptr ? name : "(dir)") + "\"}";
        }
        body += "],\"tracks\":[";
        bool first = true;
        const size_t track_begin = offset > dir_count ? offset - dir_count : 0u;
        const size_t track_end = end > dir_count
                                     ? ((end - dir_count < track_count)
                                            ? end - dir_count : track_count)
                                     : 0u;
        for (size_t i = track_begin; i < track_end; ++i) {
            const char *path = PLAYLIST_ClientGetPath(PLAYLIST_CLIENT_WEB, i);
            if (path == nullptr) {
                continue;
            }
            if (!first) {
                body += ",";
            }
            first = false;
            body += "{\"index\":" + std::to_string(i) +
                    ",\"name\":\"" + jsonEscape(pathBasename(path)) +
                    "\",\"favorite\":" + (PLAYLIST_IsFavorite(path) ? "true" : "false") +
                    ",\"current\":" +
                    ((playing && playing_path != nullptr && strcmp(path, playing_path) == 0)
                         ? "true" : "false") + "}";
        }
        body += "]";
    }
    body += "}";
    s_server.send(ok ? 200 : 400, "application/json; charset=utf-8", body);
}

static esp_err_t handlePlaylistControl(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }

    const std::string action = s_server.arg("action");
    unsigned long parsed_offset = 0UL;
    if (s_server.hasArg("offset")) {
        (void)parseUIntArg(s_server.arg("offset"), &parsed_offset);
    }
    size_t offset = static_cast<size_t>(parsed_offset);
    bool ok = true;

    const bool navigation_action = action == "enter" || action == "up" ||
                                   action == "refresh";
    if (PLAYLIST_ClientIsScanning(PLAYLIST_CLIENT_WEB) && !navigation_action &&
        action != "snapshot" && action != "status") {
        ok = false;
    } else if (action == "enter" || action == "play" || action == "favorite") {
        unsigned long index = 0UL;
        ok = parseUIntArg(s_server.arg("index"), &index);
        if (ok && action == "enter") {
            ok = PLAYLIST_ClientEnterDirAsync(PLAYLIST_CLIENT_WEB, static_cast<size_t>(index));
            offset = 0u;
        } else if (ok && action == "play") {
            ok = PLAYLIST_ClientPlayIndex(PLAYLIST_CLIENT_WEB, static_cast<size_t>(index));
        } else if (ok) {
            const char *path = PLAYLIST_ClientGetPath(PLAYLIST_CLIENT_WEB, static_cast<size_t>(index));
            ok = path != nullptr && PLAYLIST_ToggleFavorite(path);
        }
    } else if (action == "up") {
        ok = PLAYLIST_ClientUpAsync(PLAYLIST_CLIENT_WEB);
        offset = 0u;
    } else if (action == "refresh") {
        ok = PLAYLIST_ClientScanAsync(PLAYLIST_CLIENT_WEB);
        offset = 0u;
    } else if (action == "prev") {
        ok = PLAYLIST_Prev();
    } else if (action == "next") {
        ok = PLAYLIST_Next();
    } else if (action == "stop") {
        MUSIC_Stop();
    } else if (action == "repeat") {
        (void)PLAYLIST_ToggleRepeatMode();
    } else if (action != "snapshot" && action != "status") {
        ok = false;
    }

    if (!ok) {
        ESP_LOGW(TAG, "web playlist action failed: %s", action.c_str());
    }
    sendPlaylistJson(ok, offset, action != "status");
    return ESP_OK;
}

static esp_err_t handleUpdatePage(httpd_req_t *req)
{
    s_server.bind(req);
    sendChunkedHtml(200,
                    buildUpdatePageI18n("Firmware Update",
                                        "updateHeadline",
                                        "Upload a firmware file over this WiFi connection.",
                                        "updateIntro"));
    return ESP_OK;
}

static esp_err_t handleOtaStatus(httpd_req_t *req)
{
    s_server.bind(req);
    NrlOtaStatus status = {};
    OtaService_GetStatus(&status);
    std::string body = "{\"server_url\":\"" + jsonEscape(status.server_url) +
                       "\",\"configured\":" + (status.configured ? "true" : "false") +
                       ",\"checking\":" + (status.checking ? "true" : "false") +
                       ",\"updating\":" + (status.updating ? "true" : "false") +
                       ",\"update_bytes\":" + std::to_string(status.update_bytes) +
                       ",\"update_size\":" + std::to_string(status.update_size) +
                       ",\"update_percent\":" + std::to_string(status.update_percent) +
                       ",\"last_check_ms\":" + std::to_string(status.last_check_ms) +
                       ",\"latest_version\":\"" + jsonEscape(status.latest_version) +
                       "\",\"last_error\":\"" + jsonEscape(status.last_error) +
                       "\",\"releases\":[";
    for (size_t i = 0; i < status.release_count; ++i) {
        if (i > 0u) body += ",";
        body += "{\"version\":\"" + jsonEscape(status.releases[i].version) +
                "\",\"notes\":\"" + jsonEscape(status.releases[i].notes) + "\"}";
    }
    body += "]}";
    s_server.send(200, "application/json; charset=utf-8", body);
    return ESP_OK;
}

static esp_err_t handleOtaConfig(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    // An empty token means no device authentication. The configuration is kept
    // in its own NVS namespace, independent from the radio EEPROM structure.
    const bool ok = s_server.hasArg("server_url") && s_server.hasArg("device_token") &&
                    OtaService_SetConfig(s_server.arg("server_url").c_str(),
                                         s_server.arg("device_token").c_str());
    s_server.send(ok ? 200 : 400, "application/json; charset=utf-8",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Use an HTTP or HTTPS server URL\"}");
    return ESP_OK;
}

static esp_err_t handleOtaCheck(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    const bool ok = OtaService_CheckNow();
    s_server.send(ok ? 202 : 400, "application/json; charset=utf-8",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Configure the OTA server first\"}");
    return ESP_OK;
}

static esp_err_t handleOtaInstall(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    const bool ok = OtaService_UpdateVersion(s_server.arg("version").c_str());
    s_server.send(ok ? 202 : 400, "application/json; charset=utf-8",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Select a released version\"}");
    return ESP_OK;
}

// OTA upload handler. Reads the raw octet-stream body in chunks, streaming
// each chunk into esp_ota_write. Multipart uploads are no longer supported --
// wifi_update_portal.js sends application/octet-stream; refusing multipart
// keeps the body parser trivial. Stale clients see a 415.
constexpr uint32_t kLocalOtaBlockPaceMs = 3u;

static esp_err_t handleUpdate(httpd_req_t *req)
{
    s_server.bind(req);

    char content_type[96] = {};
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    if (strstr(content_type, "multipart/") != nullptr) {
        ESP_LOGW(TAG, "OTA upload rejected: multipart no longer supported (Content-Type=%s)",
                 content_type);
        DISPLAY_NOTICE_Post("OTA UPDATE FAILED", DISPLAY_NOTICE_ERROR, 10000u);
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Multipart not supported -- refresh the update page", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const size_t content_len = req->content_len;
    ESP_LOGI(TAG, "OTA upload start (content-length=%u)", static_cast<unsigned>(content_len));
    DISPLAY_NOTICE_Post("OTA UPLOADING...", DISPLAY_NOTICE_WARNING, 0u);

    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        ESP_LOGE(TAG, "no OTA target partition");
        DISPLAY_NOTICE_Post("OTA UPDATE FAILED", DISPLAY_NOTICE_ERROR, 10000u);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_OK;
    }
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target,
                                  content_len > 0u ? content_len : OTA_SIZE_UNKNOWN,
                                  &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        DISPLAY_NOTICE_Post("OTA UPDATE FAILED", DISPLAY_NOTICE_ERROR, 10000u);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    char buf[1536];
    size_t total = 0u;
    bool ok = true;
    unsigned consecutive_timeouts = 0u;
    while (ok) {
        const int got = httpd_req_recv(req, buf, sizeof(buf));
        if (got > 0) {
            err = esp_ota_write(ota_handle, buf, static_cast<size_t>(got));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed after %u bytes: %s",
                         static_cast<unsigned>(total), esp_err_to_name(err));
                ok = false;
                break;
            }
            total += static_cast<size_t>(got);
            consecutive_timeouts = 0u;
            // The HTTP worker and flash writer otherwise remain runnable for
            // the entire upload and can pin both cores near 100%. Pace each
            // block so audio, UI and Wi-Fi maintenance retain idle headroom.
            vTaskDelay(pdMS_TO_TICKS(kLocalOtaBlockPaceMs));
            continue;
        }
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            // Slow client / flash stall -- retry a few times, then abort so a
            // stalled client cannot hold the OTA handle and an httpd socket
            // forever (recv_wait_timeout is 30s per attempt).
            if (++consecutive_timeouts >= 5u) {
                ESP_LOGE(TAG, "OTA upload stalled after %u bytes, aborting",
                         static_cast<unsigned>(total));
                ok = false;
                break;
            }
            continue;
        }
        if (got == 0) {
            break;  // end of body
        }
        ESP_LOGE(TAG, "httpd_req_recv error %d after %u bytes",
                 got, static_cast<unsigned>(total));
        ok = false;
        break;
    }

    if (ok) {
        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            ok = false;
        } else {
            err = esp_ota_set_boot_partition(target);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
                ok = false;
            }
        }
    } else {
        esp_ota_abort(ota_handle);
    }

    if (ok) {
        ESP_LOGI(TAG, "OTA upload complete: %u bytes -> %s",
                 static_cast<unsigned>(total), target->label);
        DISPLAY_NOTICE_Post("OTA COMPLETE - REBOOTING", DISPLAY_NOTICE_SUCCESS, 0u);
        s_update_reboot_pending = true;
        s_update_reboot_at_ms = nowMsCfg() + 1000UL;
    } else {
        ESP_LOGE(TAG, "OTA upload failed after %u bytes", static_cast<unsigned>(total));
        DISPLAY_NOTICE_Post("OTA UPDATE FAILED", DISPLAY_NOTICE_ERROR, 10000u);
    }

    sendChunkedHtml(ok ? 200 : 500,
                    buildUpdatePageI18n(ok ? "Update Complete" : "Update Failed",
                                        ok ? "updateDoneHeadline" : "updateFailHeadline",
                                        ok ? "Firmware was written successfully. Rebooting shortly."
                                           : "Firmware upload failed. Check that the file is a valid firmware.bin.",
                                        ok ? "updateDoneIntro" : "updateFailIntro"));
    return ESP_OK;
}

static void sendChunkedAsset(httpd_req_t *req, const char *content_type,
                             const char *body, const size_t length)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, content_type);
    // Development builds are often flashed repeatedly without changing the
    // firmware version. Revalidate assets so a normal page refresh cannot keep
    // an older portal.js that does not match the newly flashed HTML.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    constexpr size_t kChunkSize = 1024;
    size_t remaining = length;
    const char *cursor = body;
    while (remaining > 0) {
        const size_t n = (remaining < kChunkSize) ? remaining : kChunkSize;
        httpd_resp_send_chunk(req, cursor, n);
        cursor += n;
        remaining -= n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t handlePortalCss(httpd_req_t *req)
{
    sendChunkedAsset(req, "text/css; charset=utf-8", kWifiConfigPortalCss, sizeof(kWifiConfigPortalCss) - 1);
    return ESP_OK;
}

static esp_err_t handlePortalJs(httpd_req_t *req)
{
    sendChunkedAsset(req, "application/javascript; charset=utf-8", kWifiConfigPortalJs, sizeof(kWifiConfigPortalJs) - 1);
    return ESP_OK;
}

static esp_err_t handleUpdateCss(httpd_req_t *req)
{
    sendChunkedAsset(req, "text/css; charset=utf-8", kWifiUpdatePortalCss, sizeof(kWifiUpdatePortalCss) - 1);
    return ESP_OK;
}

static esp_err_t handleUpdateJs(httpd_req_t *req)
{
    sendChunkedAsset(req, "application/javascript; charset=utf-8", kWifiUpdatePortalJs, sizeof(kWifiUpdatePortalJs) - 1);
    return ESP_OK;
}

static esp_err_t handlePing(httpd_req_t *req)
{
    s_server.bind(req);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/plain", "ok\n");
    return ESP_OK;
}

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF
static esp_err_t handleSensorStatus(httpd_req_t *req)
{
    s_server.bind(req);
    EnvironmentSensorSnapshot sensor = {};
    (void)ENV_SENSORS_GetSnapshot(&sensor);
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "sensor JSON allocation failed");
    }
    cJSON_AddNumberToObject(root, "updated_ms", sensor.updated_ms);
    cJSON_AddBoolToObject(root, "aht20_available", sensor.aht20_available);
    cJSON_AddStringToObject(root, "aht20_status",
                            sensor.bme280_humidity_valid
                                ? "humidity_from_bme280"
                                : sensor.aht20_available
                                      ? "ready"
                                      : "address_conflict_with_touch");
    auto addValue = [root](const char *name, const bool valid, const double value) {
        if (valid) cJSON_AddNumberToObject(root, name, value);
        else cJSON_AddNullToObject(root, name);
    };
    addValue("temperature_c", sensor.bmp280_valid, sensor.temperature_c);
    addValue("aht20_temperature_c", sensor.aht20_valid, sensor.aht20_temperature_c);
    addValue("pressure_hpa", sensor.bmp280_valid, sensor.pressure_hpa);
    addValue("humidity_percent", sensor.aht20_valid || sensor.bme280_humidity_valid,
           sensor.humidity_percent);
    addValue("illuminance_lux", sensor.bh1750_valid, sensor.illuminance_lux);
    addValue("magnetic_x_ut", sensor.qmc5883l_valid, sensor.magnetic_x_ut);
    addValue("magnetic_y_ut", sensor.qmc5883l_valid, sensor.magnetic_y_ut);
    addValue("magnetic_z_ut", sensor.qmc5883l_valid, sensor.magnetic_z_ut);
    addValue("heading_deg", sensor.qmc5883l_valid, sensor.heading_deg);
    cJSON_AddBoolToObject(root, "compass_calibrated", sensor.compass_calibrated);
    I2CDiscoveredDevice devices[32] = {};
    uint32_t i2c_revision = 0u;
    const size_t i2c_count = I2C_DEVICE_DISCOVERY_GetSnapshot(
        devices, sizeof(devices) / sizeof(devices[0]), &i2c_revision);
    cJSON_AddNumberToObject(root, "i2c_revision", i2c_revision);
    cJSON *i2c_devices = cJSON_AddArrayToObject(root, "i2c_devices");
    const size_t shown = i2c_count < (sizeof(devices) / sizeof(devices[0]))
                             ? i2c_count : (sizeof(devices) / sizeof(devices[0]));
    for (size_t i = 0u; i < shown && i2c_devices != nullptr; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == nullptr) break;
        cJSON_AddNumberToObject(item, "address_7bit", devices[i].address_7bit);
        cJSON_AddNumberToObject(item, "write_address_8bit", devices[i].write_address_8bit);
        cJSON_AddNumberToObject(item, "read_address_8bit", devices[i].read_address_8bit);
        cJSON_AddStringToObject(item, "model",
                                I2C_DEVICE_DISCOVERY_ModelName(devices[i].model));
        if (devices[i].identity_valid) {
            cJSON_AddNumberToObject(item, "identity", devices[i].identity);
        } else {
            cJSON_AddNullToObject(item, "identity");
        }
        cJSON_AddItemToArray(i2c_devices, item);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "sensor JSON serialization failed");
    }
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "application/json; charset=utf-8", body);
    cJSON_free(body);
    return ESP_OK;
}

static esp_err_t handleI2cScan(httpd_req_t *req)
{
    s_server.bind(req);
    if (!I2C_DEVICE_DISCOVERY_Scan()) {
        // esp_http_server's httpd_resp_send_err() has no 409 enum value;
        // set the status line explicitly so the page JS still sees !r.ok.
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "I2C scan is already running",
                               HTTPD_RESP_USE_STRLEN);
    }
    (void)BH4TDV_RF_IO_Init();
    return handleSensorStatus(req);
}

static esp_err_t handleSensorsPage(httpd_req_t *req)
{
    s_server.bind(req);
    static const char page[] = R"HTML(<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BH4TDV-RF 传感器</title><link rel="stylesheet" href="/portal.css"></head><body><main class="shell">
<p class="back-home"><a href="/">&larr; 返回首页</a></p><section class="panel">
<div class="section-head"><h2>传感器实时数据</h2><span class="hint mono" id="stamp">--</span></div>
<div class="status"><div><span>温度·气压模块</span><strong id="temp">--</strong></div>
<div><span>温度·温湿度模块</span><strong id="temp2">--</strong></div>
<div><span>气压</span><strong id="pressure">--</strong></div>
<div><span>湿度</span><strong id="humidity">--</strong></div>
<div><span>照度</span><strong id="lux">--</strong></div>
<div><span>航向</span><strong id="heading">--</strong></div>
<div><span>磁场 X/Y/Z</span><strong class="mono" id="mag">--</strong></div></div>
<p class="notice" id="aht">AHT20：正在读取状态</p>
<p class="hint">罗盘航向未完成安装方向及硬铁/软铁校准前仅供调试。</p></section>
<section class="panel"><div class="section-head"><h2>I2C Devices</h2>
<button type="button" id="i2c-scan">Scan 0x00-0xFF</button></div>
<p class="hint">Shows 7-bit address and the corresponding 8-bit write/read bytes.</p>
<pre class="mono" id="i2c-list">Loading...</pre></section></main>
<script>const f=(v,d,u)=>v==null?'--':Number(v).toFixed(d)+u;
const hx=v=>'0x'+Number(v).toString(16).toUpperCase().padStart(2,'0');
function showI2c(s){const a=Array.isArray(s.i2c_devices)?s.i2c_devices:[];
i2cList.textContent=a.length?a.map(d=>hx(d.address_7bit)+'  '+hx(d.write_address_8bit)+'/'+hx(d.read_address_8bit)+'  '+d.model+(d.identity==null?'':'  ID='+hx(d.identity))).join('\n'):'No I2C devices found';}
async function refresh(){try{const r=await fetch('/sensors/status',{cache:'no-store'});const s=await r.json();
temp2.textContent=f(s.aht20_temperature_c,1,' °C');temp.textContent=f(s.temperature_c,1,' °C');pressure.textContent=f(s.pressure_hpa,1,' hPa');
humidity.textContent=f(s.humidity_percent,1,' %RH');lux.textContent=f(s.illuminance_lux,0,' lx');
heading.textContent=f(s.heading_deg,1,'°');mag.textContent=[s.magnetic_x_ut,s.magnetic_y_ut,s.magnetic_z_ut].map(v=>f(v,1,'')).join(' / ')+' µT';
aht.textContent=s.aht20_status==='humidity_from_bme280'?'湿度来自 BME280（AHT20 与触摸 0x38 冲突，仍禁用）':(s.aht20_available?'AHT20：可用':'AHT20：与触摸屏地址 0x38 冲突，已安全禁用');
showI2c(s);stamp.textContent='更新 '+new Date().toLocaleTimeString();}catch(e){stamp.textContent='读取失败';}}
i2cScan.onclick=async()=>{i2cScan.disabled=true;i2cList.textContent='Scanning...';try{const r=await fetch('/sensors/i2c-scan',{method:'POST'});if(!r.ok)throw new Error();showI2c(await r.json());}catch(e){i2cList.textContent='Scan failed or already running';}finally{i2cScan.disabled=false;}};
refresh();setInterval(refresh,2000);</script></body></html>)HTML";
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/html; charset=utf-8", page);
    return ESP_OK;
}

#endif // NRL_BOARD == NRL_BOARD_BH4TDV_RF

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF
static void addRadioToneJson(cJSON *root, const char *name, const RadioTone *tone)
{
    char text[16] = {};
    RADIO_CONFIG_FormatTone(tone, text, sizeof(text));
    cJSON_AddStringToObject(root, name, text);
}

static esp_err_t handleRadioStatus(httpd_req_t *req)
{
    s_server.bind(req);
    const RadioModuleConfig *config = RADIO_CONFIG_Get();
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "radio JSON allocation failed");
    }
    cJSON_AddBoolToObject(root, "ready", SR110U_IsReady());
    cJSON_AddNumberToObject(root, "rssi", SR110U_GetCachedRssi());
    cJSON_AddNumberToObject(root, "rssi_dbm",
                            SR110U_RssiToDbm(SR110U_GetCachedRssi()));
    cJSON_AddStringToObject(root, "version", SR110U_GetVersion());
    cJSON_AddBoolToObject(root, "en", config->enabled);
    char text[16] = {};
    RADIO_CONFIG_FormatFreqMHz(config->rx_freq_hz, text, sizeof(text));
    cJSON_AddStringToObject(root, "rxf", text);
    RADIO_CONFIG_FormatFreqMHz(config->tx_freq_hz, text, sizeof(text));
    cJSON_AddStringToObject(root, "txf", text);
    addRadioToneJson(root, "rxct", &config->rx_tone);
    addRadioToneJson(root, "txct", &config->tx_tone);
    cJSON_AddNumberToObject(root, "sql", config->squelch);
    cJSON_AddNumberToObject(root, "mic", config->mic_level);
    cJSON_AddNumberToObject(root, "tot", config->tot);
    cJSON_AddNumberToObject(root, "scram", config->scramble);
    cJSON_AddBoolToObject(root, "comp", config->compander);
    cJSON_AddNumberToObject(root, "vol", config->volume);
    cJSON_AddBoolToObject(root, "sav", config->power_save);
    cJSON_AddNumberToObject(root, "vox", config->vox);
    cJSON_AddBoolToObject(root, "bclo", config->busy_lockout);
    cJSON_AddBoolToObject(root, "nb", config->narrowband);
    cJSON_AddBoolToObject(root, "lp", config->low_power);
    cJSON_AddNumberToObject(root, "rt", config->radio_type);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "radio JSON serialization failed");
    }
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "application/json; charset=utf-8", body);
    cJSON_free(body);
    return ESP_OK;
}

static esp_err_t handleSaveRadio(httpd_req_t *req)
{
    s_server.bind(req);
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    RadioModuleConfig config = *RADIO_CONFIG_Get();
    bool ok = true;
    const char *bad_field = "";
    if (s_server.hasArg("en_present")) {
        config.enabled = s_server.hasArg("en");
    }
    if (ok && s_server.hasArg("rxf")) {
        ok = RADIO_CONFIG_ParseFreqMHz(s_server.arg("rxf").c_str(),
                                       &config.rx_freq_hz);
        if (!ok) bad_field = "rxf";
    }
    if (ok && s_server.hasArg("txf")) {
        ok = RADIO_CONFIG_ParseFreqMHz(s_server.arg("txf").c_str(),
                                       &config.tx_freq_hz);
        if (!ok) bad_field = "txf";
    }
    if (ok && s_server.hasArg("rxct")) {
        ok = RADIO_CONFIG_ParseTone(s_server.arg("rxct").c_str(),
                                    &config.rx_tone);
        if (!ok) bad_field = "rxct";
    }
    if (ok && s_server.hasArg("txct")) {
        ok = RADIO_CONFIG_ParseTone(s_server.arg("txct").c_str(),
                                    &config.tx_tone);
        if (!ok) bad_field = "txct";
    }
    auto uintField = [&](const char *name, uint8_t RadioModuleConfig::*member,
                         const unsigned long hi) {
        if (!ok || !s_server.hasArg(name)) return;
        unsigned long value = 0u;
        ok = parseUIntArg(s_server.arg(name), &value) && value <= hi;
        if (ok) config.*member = static_cast<uint8_t>(value);
        else bad_field = name;
    };
    uintField("sql", &RadioModuleConfig::squelch, 8u);
    uintField("mic", &RadioModuleConfig::mic_level, 8u);
    uintField("tot", &RadioModuleConfig::tot, 9u);
    uintField("scram", &RadioModuleConfig::scramble, 7u);
    uintField("vox", &RadioModuleConfig::vox, 8u);
    if (ok && s_server.hasArg("vol")) {
        unsigned long value = 0u;
        ok = parseUIntArg(s_server.arg("vol"), &value) &&
             value >= 1u && value <= 9u;
        if (ok) config.volume = static_cast<uint8_t>(value);
        else bad_field = "vol";
    }
    if (s_server.hasArg("comp_present")) {
        config.compander = s_server.hasArg("comp");
    }
    if (s_server.hasArg("sav_present")) {
        config.power_save = s_server.hasArg("sav");
    }
    if (s_server.hasArg("bclo_present")) {
        config.busy_lockout = s_server.hasArg("bclo");
    }
    if (ok && s_server.hasArg("nb")) {
        config.narrowband = s_server.arg("nb") == "1";
    }
    if (ok && s_server.hasArg("lp")) {
        config.low_power = s_server.arg("lp") == "1";
    }
    if (ok && s_server.hasArg("rt")) {
        unsigned long value = 0u;
        ok = parseUIntArg(s_server.arg("rt"), &value) && value <= 1u;
        if (ok) config.radio_type = static_cast<uint8_t>(value);
        else bad_field = "rt";
    }

    if (!ok || !RADIO_CONFIG_Set(&config, true)) {
        if (bad_field[0] == '\0') bad_field = "config";
        char detail[64] = {};
        snprintf(detail, sizeof(detail), "invalid radio config: %s", bad_field);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, detail);
        return ESP_OK;
    }
    const bool applied = RADIO_CONFIG_ApplyToModule();
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/plain; charset=utf-8",
                  applied ? "saved+applied" : "saved (module apply deferred)");
    return ESP_OK;
}

static esp_err_t handleRadioPage(httpd_req_t *req)
{
    s_server.bind(req);
    static const char page[] = R"HTML(<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BH4TDV-RF 射频模块</title><link rel="stylesheet" href="/portal.css"></head><body><main class="shell">
<p class="back-home"><a href="/">&larr; 返回首页</a></p><section class="panel">
<div class="section-head"><h2>SR-110U 射频模块</h2><span class="hint mono" id="state">--</span></div>
<form id="cfg"><div class="grid">
<label class="fmo-row"><input type="checkbox" name="en" value="1" id="en">启用射频模块（关闭即断电）</label>
<label>接收频率 MHz<input name="rxf" id="rxf" maxlength="9" placeholder="450.02500"></label>
<label>发射频率 MHz<input name="txf" id="txf" maxlength="9" placeholder="450.02500"></label>
<label>接收哑音<input name="rxct" id="rxct" list="tonelist" maxlength="6" placeholder="OFF / 67.0 / D023N"></label>
<label>发射哑音<input name="txct" id="txct" list="tonelist" maxlength="6" placeholder="OFF / 88.5 / D023I"></label>
<label>静噪等级 SQL<select name="sql" id="sql"></select></label>
<label>MIC 灵敏度<select name="mic" id="mic"></select></label>
<label>发射定时 TOT<select name="tot" id="tot"></select></label>
<label>扰频<select name="scram" id="scram"></select></label>
<label>音量<select name="vol" id="vol"></select></label>
<label>VOX<select name="vox" id="vox"></select></label>
<label>带宽<select name="nb" id="nb"><option value="0">宽带</option><option value="1">窄带</option></select></label>
<label>发射功率<select name="lp" id="lp"><option value="0">高功率</option><option value="1">低功率</option></select></label>
<label>电台类型<select name="rt" id="rt"><option value="0">其它电台</option><option value="1">YAESU/MOTO</option></select></label>
<label class="fmo-row"><input type="checkbox" name="comp" value="1" id="comp">压扩</label>
<label class="fmo-row"><input type="checkbox" name="sav" value="1" id="sav">接收省电</label>
<label class="fmo-row"><input type="checkbox" name="bclo" value="1" id="bclo">遇忙禁发</label>
</div>
<input type="hidden" name="en_present" value="1"><input type="hidden" name="comp_present" value="1">
<input type="hidden" name="sav_present" value="1"><input type="hidden" name="bclo_present" value="1">
<button type="submit">保存并下发到模块</button></form>
<p class="hint" id="msg"></p>
<p class="hint">哑音格式：OFF 关闭；CTCSS 填频率如 67.0；CDCSS 填 D023N（正极性）/D023I（负极性）。
频率范围 400.00000-480.00000 MHz，2.5/6.25 kHz 步进。参数写入模块断电记忆；保存时若模块不可达，会在下次开机时应用。</p></section>
<script>
const $=id=>document.getElementById(id);let loaded=false;
function fill(id,lo,hi,label){const e=$(id);for(let i=lo;i<=hi;i++){const o=document.createElement('option');o.value=i;o.textContent=label?label(i):i;e.appendChild(o);}}
fill('sql',0,8);fill('mic',0,8);
fill('tot',0,9,i=>i===0?'关闭':i+' 分钟');
fill('scram',0,7,i=>i===0?'关闭':i);
fill('vol',1,9);
fill('vox',0,8,i=>i===0?'关闭':i);
async function refresh(){try{const r=await fetch('/radio/status',{cache:'no-store'});const s=await r.json();
$('state').textContent=(s.ready?'模块在线':'模块离线')+(s.version?' '+s.version:'')+' / RSSI '+(s.rssi>=0?s.rssi_dbm+'dBm ('+s.rssi+')':'--');
if(loaded)return;loaded=true;
$('en').checked=s.en;$('rxf').value=s.rxf;$('txf').value=s.txf;$('rxct').value=s.rxct;$('txct').value=s.txct;
$('sql').value=s.sql;$('mic').value=s.mic;$('tot').value=s.tot;$('scram').value=s.scram;
$('vol').value=s.vol;$('vox').value=s.vox;$('nb').value=s.nb?'1':'0';$('lp').value=s.lp?'1':'0';$('rt').value=String(s.rt??0);
$('comp').checked=s.comp;$('sav').checked=s.sav;$('bclo').checked=s.bclo;}catch(e){$('state').textContent='读取失败';}}
const CTCSS=['67.0','69.3','71.9','74.4','77.0','79.7','82.5','85.4','88.5','91.5','94.8','97.4','100.0','103.5','107.2','110.9','114.8','118.8','123.0','127.3','131.8','136.5','141.3','146.2','151.4','156.7','159.8','162.2','167.9','173.8','179.9','183.5','186.2','189.9','192.8','196.6','199.5','203.5','206.5','210.7','218.1','225.7','229.1','233.6','241.8','250.3','254.1'];
const CDCSS=['023','025','026','031','032','036','043','047','051','053','054','065','071','072','073','074','114','115','116','122','125','131','132','134','143','145','152','155','156','162','165','172','174','205','212','223','225','226','243','244','245','246','251','252','255','261','263','265','266','271','274','306','311','315','325','331','332','343','346','351','356','364','365','371','411','412','413','423','431','432','445','446','452','454','455','462','464','465','466','503','506','516','523','526','532','546','565','606','612','624','627','631','632','654','662','664','703','712','723','731','732','734','743','754'];
{const dl=document.createElement('datalist');dl.id='tonelist';['OFF',...CTCSS,...CDCSS.map(c=>'D'+c+'N'),...CDCSS.map(c=>'D'+c+'I')].forEach(v=>{const o=document.createElement('option');o.value=v;dl.appendChild(o);});document.body.appendChild(dl);}
$('cfg').onsubmit=async e=>{e.preventDefault();const body=new URLSearchParams(new FormData(e.target));
try{const r=await fetch('/save_radio',{method:'POST',body});const t=await r.text();
$('msg').textContent=r.ok?'已保存：'+t:'保存失败：'+t;if(r.ok)loaded=false;}catch(err){$('msg').textContent='保存失败：'+err;}refresh();};
refresh();setInterval(refresh,3000);
</script></main></body></html>)HTML";
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "text/html; charset=utf-8", page);
    return ESP_OK;
}
#endif // NRL_BOARD == NRL_BOARD_BH4TDV_RF

// Machine-readable local AT endpoint used by the mini program. This avoids
// scraping the device's HTML UI and executes the same command parser as the
// USB serial console. LAN access is intentionally equivalent to opening the
// unauthenticated configuration portal, so it should only be used on a
// trusted local network.
static esp_err_t handleLocalAt(httpd_req_t *req)
{
    if (!s_server.bindPost(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form parse failed");
        return ESP_OK;
    }
    const std::string command = s_server.arg("command");
    if (command.empty() || command.size() > 255u || command.rfind("AT+", 0u) != 0u) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid AT command");
        return ESP_OK;
    }

    uint8_t payload[257] = {};
    payload[0] = 0x01u;
    memcpy(payload + 1u, command.data(), command.size());
    NrlAtCommandResult result = {};
    NRL_AT_HandlePayload(payload, command.size() + 1u,
                         NRL_AT_SOURCE_SERIAL, &result);

    if (result.restart_wifi || result.restart_udp) {
        NRLAudioBridge_ApplyConfig(result.restart_wifi, result.restart_udp);
    }
    if (result.reboot) {
        s_update_reboot_pending = true;
        s_update_reboot_at_ms = nowMsCfg() + 500UL;
    }

    std::string reply;
    if (result.should_reply && result.payload_size > 1u) {
        reply.assign(reinterpret_cast<const char *>(result.payload + 1u),
                     result.payload_size - 1u);
    }
    const std::string body = std::string("{\"ok\":") +
        (result.should_reply ? "true" : "false") +
        ",\"reply\":\"" + jsonEscape(reply) + "\"}";
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(result.should_reply ? 200 : 400,
                  "application/json; charset=utf-8", body.c_str());
    return ESP_OK;
}

static esp_err_t handleCaptiveRedirect(httpd_req_t *req)
{
    s_server.bind(req);
    redirectToPortal();
    return ESP_OK;
}

static esp_err_t handleNotFound(httpd_req_t *req, httpd_err_code_t /*err*/)
{
    s_server.bind(req);
    redirectToPortal();
    return ESP_OK;
}

static void ensureServerRunning()
{
    if (s_server_started) {
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 56; // kRoutes on BH4TDV_RF is 49 entries; leave headroom
    // The IDF default allows seven HTTP clients and uses another three
    // sockets internally. With the default LWIP socket pool that can consume
    // every descriptor before APRS, NRL, SMB playback or OTA opens one.
    // Three browser sessions are enough for the embedded portal; LRU purge
    // below replaces stale keep-alive sessions when another client arrives.
    cfg.max_open_sockets = 3;
    // Templated audio config page builds via std::string concatenation can
    // exceed the default 4 KB worker stack.
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    // OTA upload can be 2 MB, takes ~30 s. Default 5 s recv timeout would
    // abort mid-stream when a flash erase block stalls the read.
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    struct UriEntry {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    };
    static const UriEntry kRoutes[] = {
        { "/",                     HTTP_GET,  handleRoot },
        { "/wifi",                 HTTP_GET,  handleWifiPage },
        { "/nrl",                  HTTP_GET,  handleNrlPage },
        { "/nrl/servers",          HTTP_GET,  handleNrlServersGet },
        { "/nrl/servers",          HTTP_POST, handleNrlServersPut },
        { "/nrl/servers/refresh",  HTTP_GET,  handleNrlServersRefresh },
        { "/audio",                HTTP_GET,  handleAudioPage },
        { "/fmo",                  HTTP_GET,  handleFmoPage },
        { "/fmo/status",           HTTP_GET,  handleFmoStatus },
        { "/fmo/config",           HTTP_POST, handleFmoConfig },
        { "/fmo/qso",              HTTP_POST, handleFmoQso },
        { "/fmo/cert/user",        HTTP_POST, handleFmoCertificate },
        { "/fmo/cert/intermediate",HTTP_POST, handleFmoCertificate },
        { "/fmo/cert/devicekey",   HTTP_POST, handleFmoCertificate },
        { "/scan",                 HTTP_GET,  handleScan },
        { "/save_wifi",            HTTP_POST, handleSaveWifi },
        { "/save_nrl",             HTTP_POST, handleSaveNrl },
        { "/media",                HTTP_GET,  handleMediaPage },
        { "/save_media",           HTTP_POST, handleSaveMedia },
        { "/media/playlist",       HTTP_POST, handlePlaylistControl },
        { "/aprs",                 HTTP_GET,  handleAprsPage },
        { "/save_aprs",            HTTP_POST, handleSaveAprs },
        { "/aprs/stations",        HTTP_GET,  handleAprsStations },
#if NRL_HAS_SIGNALING
        { "/signaling",            HTTP_GET,  handleSignalingPage },
        { "/save_signaling",       HTTP_POST, handleSaveSignaling },
#endif
        { "/save_serial",          HTTP_POST, handleSaveSerial },
        { "/update",               HTTP_GET,  handleUpdatePage },
        { "/update",               HTTP_POST, handleUpdate },
        { "/ota/status",           HTTP_GET,  handleOtaStatus },
        { "/ota/config",           HTTP_POST, handleOtaConfig },
        { "/ota/check",            HTTP_POST, handleOtaCheck },
        { "/ota/install",          HTTP_POST, handleOtaInstall },
        { "/portal.css",           HTTP_GET,  handlePortalCss },
        { "/portal.js",            HTTP_GET,  handlePortalJs },
#if NRL_BOARD == NRL_BOARD_BH4TDV_RF
        { "/sensors",             HTTP_GET,  handleSensorsPage },
        { "/sensors/status",      HTTP_GET,  handleSensorStatus },
        { "/sensors/i2c-scan",    HTTP_POST, handleI2cScan },
        { "/radio",               HTTP_GET,  handleRadioPage },
        { "/radio/status",        HTTP_GET,  handleRadioStatus },
        { "/save_radio",          HTTP_POST, handleSaveRadio },
#endif
        { "/update.css",           HTTP_GET,  handleUpdateCss },
        { "/update.js",            HTTP_GET,  handleUpdateJs },
        { "/ping",                 HTTP_GET,  handlePing },
        { "/api/at",               HTTP_POST, handleLocalAt },
        { "/favicon.ico",          HTTP_GET,  handleFavicon },
        { "/generate_204",         HTTP_GET,  handleCaptiveRedirect },
        { "/hotspot-detect.html",  HTTP_GET,  handleCaptiveRedirect },
        { "/connecttest.txt",      HTTP_GET,  handleCaptiveRedirect },
        { "/ncsi.txt",             HTTP_GET,  handleCaptiveRedirect },
    };
    for (const UriEntry &r : kRoutes) {
        httpd_uri_t uri = {};
        uri.uri = r.uri;
        uri.method = r.method;
        uri.handler = r.handler;
        uri.user_ctx = nullptr;
        if (httpd_register_uri_handler(s_httpd, &uri) != ESP_OK) {
            ESP_LOGW(TAG, "register %s failed", r.uri);
        }
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, handleNotFound);

    s_server_started = true;
    ESP_LOGI(TAG, "HTTP config server started on port 80");
}

} // namespace

void WifiConfigPortal_GetApSsid(char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0u) {
        return;
    }
    const std::string ssid = buildApSsid();
    snprintf(out, out_size, "%s", ssid.c_str());
}

bool WifiConfigPortal_Init(void)
{
    EXTERNAL_RADIO_Init();
    if (NRL_PIN_BOOT_BUTTON >= 0) {
        gpio_reset_pin((gpio_num_t)NRL_PIN_BOOT_BUTTON);
        gpio_set_direction((gpio_num_t)NRL_PIN_BOOT_BUTTON, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)NRL_PIN_BOOT_BUTTON, GPIO_PULLUP_ONLY);
    }
    s_ap_should_run = true;
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    return true;
}

void WifiConfigPortal_Poll(void)
{
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    pollBootResetGesture();
    manageApLifecycle();
    if (s_dns_started) {
        NRL_CaptiveDNS_Poll();
    }
    // esp_http_server runs its own task; no per-loop polling needed.
    if (s_update_reboot_pending && (nowMsCfg() - s_update_reboot_at_ms) < 0x80000000UL) {
        ESP_LOGI(TAG, "OTA reboot now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

void WifiConfigPortal_EnterFallbackMode(void)
{
    s_ap_should_run = true;
    s_sta_was_connected = false;
    s_ap_close_scheduled = false;
    s_sta_disconnect_started_ms = 0UL;
    ensureApRunning();
    ensureDnsRunning();
    ensureServerRunning();
    char ip_buf[16] = {};
    nrlIpToString(nrlWifiApIp(), ip_buf, sizeof(ip_buf));
    ESP_LOGI(TAG, "Fallback config AP active: ssid=%s ip=%s",
             buildApSsid().c_str(), ip_buf);
}
