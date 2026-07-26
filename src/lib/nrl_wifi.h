#pragma once

#include <stddef.h>
#include <stdint.h>

// Pure-IDF WiFi facade. Replaces the Arduino WiFi class for both STA setup
// (audio bridge) and AP/scan management (config portal).

enum WifiConnResult : uint8_t {
    WIFI_CONN_OK = 0,          // STA connected to the target SSID
    WIFI_ALREADY_ON_SSID,      // STA already on the target SSID before this call
    WIFI_CONN_TIMEOUT,         // Timeout waiting for the L2 + IP handshake
    WIFI_CONN_FAILED,          // Bad parameters / init failure
};

struct NetProbeResult {
    bool reachable = false;
    uint32_t rtt_ms = 0;
    const char *method = "";
};

struct NrlWifiScanResult {
    char ssid[33];  // includes the trailing NUL
    int8_t rssi;
    bool secured;  // true if the AP requires authentication
};

// Initialize the WiFi driver (NVS + netif + event loop + esp_wifi_init). The
// radio starts in WIFI_MODE_NULL — call nrlWifiApStart/wifiEnsureConnected to
// bring up AP/STA. Idempotent; subsequent calls are a no-op.
bool nrlWifiInit();

// True once STA has a valid IPv4 address (i.e. DHCP/static-IP completed).
bool wifiIsConnected();

// Synchronous STA connect with timeout. Preserves AP mode if already running
// (switches to APSTA in that case). Reads static-IP config from
// ExternalRadioConfig (set wifi_dhcp_enabled to fall back to DHCP).
WifiConnResult wifiEnsureConnected(const char *ssid, const char *pass, uint32_t timeout_ms = 15000);

// Start/stop AP. The IP/gateway/mask are lwIP-byte-order uint32_t (the same
// representation as nrlWifiStaIp() / config->wifi_ip). channel=0 means "let
// the driver pick"; pass nullptr password for open network.
bool nrlWifiApStart(const char *ssid, uint8_t channel, uint8_t max_conn,
                    uint32_t ap_ip, uint32_t gw, uint32_t mask);
bool nrlWifiApStop();
// Stop the whole Wi-Fi radio (STA+AP -> WIFI_MODE_NULL / esp_wifi_stop), freeing
// the shared radio and Wi-Fi RAM for Bluetooth A2DP. wifiEnsureConnected() restores.
bool nrlWifiStopRadio();
size_t nrlWifiApGetStationCount();

// Synchronous scan. Fills the internal cache; consumers retrieve via
// nrlWifiScanGetCache. Returns false on timeout/error.
bool nrlWifiScanStartBlocking(uint32_t timeout_ms);
size_t nrlWifiScanGetCache(NrlWifiScanResult *out, size_t max_count);

// Reachability probe. Tries TCP 223.5.5.5:53 first; if that fails, HTTP GET
// http://www.baidu.com/ and inspect the status line. Returns the first hit.
NetProbeResult internetProbeCN(uint32_t timeout_ms = 5000);
