#include "ble_config.h"

#include "sdkconfig.h"

// BLE Wi-Fi provisioning. Two host implementations share the same command
// protocol and NUS GATT service:
//   * NimBLE (ESP32-S3 boards) -- lightweight BLE-only host
//   * Bluedroid (ESP32-S31 boards) -- dual-mode host also running Classic BT
//     HFP/A2DP; BLE provisioning is a fallback when no network is available.
#if defined(CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_BT_BLUEDROID_ENABLED)

#include "nrl_audio_bridge.h"
#include "nrl_net_compat.h"
#include "nrl_version.h"
#include "nrl_wifi.h"

#include "../app/driver/external_radio.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED)
#include <NimBLEDevice.h>
#endif

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

static const char *TAG = "BLECFG";

namespace {

constexpr char kBleDeviceName[] = "NRL-ESP32-CFG";
constexpr char kServiceUuid[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kRxUuid[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kTxUuid[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr size_t kCommandBufferSize = 160;

// --- Common state (both NimBLE and Bluedroid transports) ---
bool s_initialized = false;
bool s_client_connected = false;
bool s_advertising = false;
char s_command_buffer[kCommandBufferSize] = {};
size_t s_command_size = 0;
bool s_reboot_pending = false;
uint32_t s_reboot_at_ms = 0;
// WiFi list/scan work is deferred out of the GATT write callback into
// BLEConfig_Poll() (mainLoopTask). The host task stack is limited and a
// blocking SCAN would risk a BLE supervision timeout.
bool s_scan_pending = false;   // fresh scan requested
bool s_list_pending = false;   // cached-list dump requested
bool s_wifi_transaction = false;
bool s_wifi_transaction_has_ssid = false;
char s_staged_wifi_ssid[33] = {};
char s_staged_wifi_password[65] = {};
volatile bool s_restart_wifi_pending = false;
volatile bool s_restart_udp_pending = false;
volatile bool s_wifi_result_pending = false;

static inline uint32_t nowMsBle()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// Forward declaration: each transport provides its own implementation.
static void sendLine(const char *line);

#if defined(CONFIG_BT_NIMBLE_ENABLED)
// --- NimBLE-specific constants and state ---
constexpr size_t kBleHostStackBytes =
#if defined(CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE)
    CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE;
#elif defined(CONFIG_BT_NIMBLE_TASK_STACK_SIZE)
    CONFIG_BT_NIMBLE_TASK_STACK_SIZE;
#else
    4096u;
#endif
constexpr size_t kBleMinLargestInternalBlock = kBleHostStackBytes + 32768u;
constexpr size_t kBleMinFreeInternal = 96u * 1024u;

NimBLEServer *s_server = nullptr;
NimBLECharacteristic *s_tx = nullptr;
NimBLECharacteristic *s_rx = nullptr;

// BT/WiFi coexistence management. The single radio is shared; while the BT
// controller is up, coexist time-slices airtime and bunches voice packets.
// We keep BLE up only while the WiFi STA is down (provisioning fallback) and
// tear it down once the STA has been solidly connected for a grace period.
constexpr uint32_t kBleStopAfterStaUpMs = 4000u; // STA stable this long -> drop BLE
// Longer than the 15 s WiFi connect timeout: configured devices skip BLE at
// boot, and this grace keeps the fallback from flapping BLE up mid-connect on
// every normal boot (or during a brief AP hiccup).
constexpr uint32_t kBleStartAfterStaDownMs = 30000u; // STA down this long -> bring BLE back
bool s_ble_auto_stopped = false;     // true once we tore BLE down due to WiFi
uint32_t s_sta_up_since_ms = 0u;     // 0 = STA not currently up
uint32_t s_sta_down_since_ms = 0u;   // 0 = STA not currently down
#endif // CONFIG_BT_NIMBLE_ENABLED

#if defined(CONFIG_BT_NIMBLE_ENABLED)
static void sendLine(const char *line)
{
    if (line == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "%s", line);
    if (s_tx == nullptr || !s_client_connected) {
        return;
    }

    s_tx->setValue(reinterpret_cast<const uint8_t *>(line), strlen(line));
    s_tx->notify();
}
#endif // CONFIG_BT_NIMBLE_ENABLED

static void sendConfig(void)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    if (config == nullptr) {
        sendLine("ERR CONFIG");
        return;
    }

    char line[128];
    snprintf(line, sizeof(line), "WIFI_SSID=%s", config->wifi_ssid);
    sendLine(line);
    snprintf(line, sizeof(line), "SERVER_HOST=%s", config->server_host);
    sendLine(line);
    snprintf(line, sizeof(line), "SERVER_PORT=%u", static_cast<unsigned>(config->server_port));
    sendLine(line);
    snprintf(line, sizeof(line), "CHANNEL=%u", static_cast<unsigned>(config->channel));
    sendLine(line);
    snprintf(line, sizeof(line), "CALLSIGN=%s", config->callsign);
    sendLine(line);
    snprintf(line, sizeof(line), "CALL_SSID=%u", static_cast<unsigned>(config->callsign_ssid));
    sendLine(line);
    sendLine("OK GET");
}

static bool parseUnsigned(const char *text, unsigned long *out)
{
    if (text == nullptr || out == nullptr || *text == '\0') {
        return false;
    }

    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || (end != nullptr && *end != '\0')) {
        return false;
    }

    *out = value;
    return true;
}

static bool startsWith(const char *text, const char *prefix)
{
    return text != nullptr &&
           prefix != nullptr &&
           strncmp(text, prefix, strlen(prefix)) == 0;
}

static void trimInPlace(char *text)
{
    if (text == nullptr) {
        return;
    }

    char *start = text;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
}

static bool setField(const char *key, const char *value, bool *restart_wifi, bool *restart_udp)
{
    if (key == nullptr || value == nullptr || restart_wifi == nullptr || restart_udp == nullptr) {
        return false;
    }

    if (strcmp(key, "WIFI_SSID") == 0 || strcmp(key, "SSID") == 0) {
        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const char *old_value = before != nullptr ? before->wifi_ssid : "";
        const bool changed = strcmp(old_value, value) != 0;
        if (!EXTERNAL_RADIO_SetWifiSsid(value, false)) {
            return false;
        }
        *restart_wifi = *restart_wifi || changed;
        return true;
    }

    if (strcmp(key, "WIFI_PASS") == 0 || strcmp(key, "WIFI_PASSWORD") == 0 || strcmp(key, "PASS") == 0) {
        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const char *old_value = before != nullptr ? before->wifi_password : "";
        const bool changed = strcmp(old_value, value) != 0;
        if (!EXTERNAL_RADIO_SetWifiPassword(value, false)) {
            return false;
        }
        *restart_wifi = *restart_wifi || changed;
        return true;
    }

    if (strcmp(key, "SERVER_HOST") == 0 || strcmp(key, "D_IP") == 0) {
        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const char *old_value = before != nullptr ? before->server_host : "";
        const bool changed = strcmp(old_value, value) != 0;
        if (!EXTERNAL_RADIO_SetServerHost(value, false)) {
            return false;
        }
        *restart_udp = *restart_udp || changed;
        return true;
    }

    if (strcmp(key, "SERVER_PORT") == 0 || strcmp(key, "D_PORT") == 0) {
        unsigned long port = 0;
        if (!parseUnsigned(value, &port) || port == 0 || port > 65535) {
            return false;
        }

        const ExternalRadioConfig *before = EXTERNAL_RADIO_GetConfig();
        const uint16_t old_value = before != nullptr ? before->server_port : 0;
        const bool changed = old_value != static_cast<uint16_t>(port);
        if (!EXTERNAL_RADIO_SetServerPort(static_cast<uint16_t>(port), false)) {
            return false;
        }
        *restart_udp = *restart_udp || changed;
        return true;
    }

    if (strcmp(key, "CHANNEL") == 0 || strcmp(key, "CH") == 0) {
        unsigned long channel = 0;
        return parseUnsigned(value, &channel) &&
               channel <= 7 &&
               EXTERNAL_RADIO_SetChannel(static_cast<uint8_t>(channel), false);
    }

    if (strcmp(key, "CALLSIGN") == 0 || strcmp(key, "CALL") == 0) {
        return EXTERNAL_RADIO_SetCallsign(value, false);
    }

    if (strcmp(key, "CALL_SSID") == 0 || strcmp(key, "CALLSIGN_SSID") == 0) {
        unsigned long ssid = 0;
        return parseUnsigned(value, &ssid) &&
               ssid >= 1 && ssid <= 99 &&
               EXTERNAL_RADIO_SetCallsignSsid(static_cast<uint8_t>(ssid), false);
    }

    return false;
}

static void sendHelp(void)
{
    sendLine("NRL BLE config commands:");
    sendLine("GET");
    sendLine("SET WIFI_SSID=<ssid>");
    sendLine("SET WIFI_PASS=<password>");
    sendLine("SET SERVER_HOST=<host_or_ip>");
    sendLine("SET SERVER_PORT=<1..65535>");
    sendLine("SET CHANNEL=<0..7>");
    sendLine("SET CALLSIGN=<callsign>");
    sendLine("SET CALL_SSID=<0..255>");
    sendLine("LIST");
    sendLine("SCAN");
    sendLine("BEGIN");
    sendLine("SAVE");
    sendLine("APPLY");
    sendLine("RESET_NET");
    sendLine("REBOOT");
}

static void beginWifiTransaction(void)
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    snprintf(s_staged_wifi_ssid, sizeof(s_staged_wifi_ssid), "%s",
             config != nullptr ? config->wifi_ssid : "");
    snprintf(s_staged_wifi_password, sizeof(s_staged_wifi_password), "%s",
             config != nullptr ? config->wifi_password : "");
    s_wifi_transaction_has_ssid = false;
    s_wifi_transaction = true;
}

static bool stageWifiField(const char *key, const char *value)
{
    if (strcmp(key, "WIFI_SSID") == 0 || strcmp(key, "SSID") == 0) {
        if (value[0] == '\0' || strlen(value) > 32u) return false;
        if (strcmp(s_staged_wifi_ssid, value) != 0) {
            // Never carry the previous hotspot's password into a new SSID.
            s_staged_wifi_password[0] = '\0';
        }
        snprintf(s_staged_wifi_ssid, sizeof(s_staged_wifi_ssid), "%s", value);
        s_wifi_transaction_has_ssid = true;
        return true;
    }
    if (strcmp(key, "WIFI_PASS") == 0 || strcmp(key, "WIFI_PASSWORD") == 0 ||
        strcmp(key, "PASS") == 0) {
        if (strlen(value) > 64u) return false;
        snprintf(s_staged_wifi_password, sizeof(s_staged_wifi_password), "%s", value);
        return true;
    }
    return false;
}

static bool commitWifiTransaction(void)
{
    if (!s_wifi_transaction || !s_wifi_transaction_has_ssid ||
        s_staged_wifi_ssid[0] == '\0') {
        return false;
    }
    if (!EXTERNAL_RADIO_AddWifiProfile(s_staged_wifi_ssid,
                                       s_staged_wifi_password, false)) {
        return false;
    }

    // Keep all saved hotspots, but make the one selected during provisioning
    // the first connection priority.
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    size_t selected = EXTERNAL_RADIO_MAX_WIFI_PROFILES;
    if (config != nullptr) {
        for (size_t i = 0; i < EXTERNAL_RADIO_MAX_WIFI_PROFILES; ++i) {
            if (strcmp(config->wifi_profiles[i].ssid, s_staged_wifi_ssid) == 0) {
                selected = i;
                break;
            }
        }
    }
    while (selected > 0u && selected < EXTERNAL_RADIO_MAX_WIFI_PROFILES) {
        if (!EXTERNAL_RADIO_MoveWifiProfile(selected, -1, false)) return false;
        --selected;
    }
    if (!EXTERNAL_RADIO_SetWifiEnabled(true, false) ||
        !EXTERNAL_RADIO_SaveConfig()) {
        return false;
    }
    s_wifi_transaction = false;
    s_wifi_transaction_has_ssid = false;
    s_restart_wifi_pending = true;
    s_restart_udp_pending = true;
    s_wifi_result_pending = true;
    return true;
}

// Stream the unique SSIDs currently in `results[0..got)` back over BLE as
// "WIFI=<ssid>,<rssi>" lines, then "OK <tag>".
static void reportWifiResults(const NrlWifiScanResult *results, size_t got, const char *ok_tag)
{
    constexpr size_t kMaxReport = 24u;
    char line[160];
    size_t reported = 0;
    for (size_t i = 0; i < got && reported < kMaxReport; ++i) {
        if (results[i].ssid[0] == '\0') {
            continue; // skip hidden / empty SSIDs
        }
        // De-duplicate: the same SSID can appear on several APs/channels.
        bool dup = false;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(results[i].ssid, results[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        snprintf(line, sizeof(line), "WIFI=%s,%d",
                 results[i].ssid, static_cast<int>(results[i].rssi));
        sendLine(line);
        ++reported;
    }
    sendLine(ok_tag);
}

// Return the WiFi list the device already scanned (e.g. the pre-scan done
// when the config-portal AP comes up). Reads the cached results only -- no
// new scan -- so it is safe to call directly from the GATT callback and
// returns instantly. Empty if the device hasn't scanned yet (client can SCAN).
static void reportCachedWifi(void)
{
    constexpr size_t kMaxReport = 24u;
    static NrlWifiScanResult results[kMaxReport]; // off-stack; poll task only
    const size_t got = nrlWifiScanGetCache(results, kMaxReport);
    reportWifiResults(results, got, "OK LIST");
}

// Run a fresh blocking WiFi scan and stream the SSIDs. The scan blocks ~1.5-2s
// so this is called from BLEConfig_Poll(), never from the BLE host callback.
static void runWifiScanAndReport(void)
{
    constexpr size_t kMaxReport = 24u;
    static NrlWifiScanResult results[kMaxReport]; // off-stack; poll task only

    if (!nrlWifiScanStartBlocking(6000u)) {
        sendLine("ERR SCAN");
        return;
    }
    const size_t got = nrlWifiScanGetCache(results, kMaxReport);
    reportWifiResults(results, got, "OK SCAN");
}

static void handleCommand(char *command)
{
    trimInPlace(command);
    if (command[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "rx: %s", command);

    if (strcasecmp(command, "HELP") == 0 || strcmp(command, "?") == 0) {
        sendHelp();
        return;
    }

    if (strcasecmp(command, "GET") == 0) {
        sendConfig();
        return;
    }

    if (strcasecmp(command, "LIST") == 0) {
        // Cached list from the device's earlier scan (e.g. config-portal
        // pre-scan). Deferred to the poll task: the report buffers are too
        // large for the 4 KB NimBLE host task stack at GATT-callback depth.
        s_list_pending = true;
        return;
    }

    if (strcasecmp(command, "SCAN") == 0) {
        // Defer to the poll loop; the scan blocks too long for this callback.
        s_scan_pending = true;
        return;
    }

    if (strcasecmp(command, "BEGIN") == 0) {
        beginWifiTransaction();
        sendLine("OK BEGIN");
        return;
    }

    if (strcasecmp(command, "SAVE") == 0) {
        sendLine(EXTERNAL_RADIO_SaveConfig() ? "OK SAVE" : "ERR SAVE");
        return;
    }

    if (strcasecmp(command, "APPLY") == 0) {
        if (s_wifi_transaction) {
            if (!commitWifiTransaction()) {
                sendLine("ERR APPLY");
                return;
            }
            sendLine("OK APPLY");
            sendLine("WIFI_STATE=CONNECTING");
        } else {
            s_restart_wifi_pending = true;
            s_restart_udp_pending = true;
            sendLine("OK APPLY");
        }
        return;
    }

    if (strcasecmp(command, "RESET_NET") == 0) {
        if (EXTERNAL_RADIO_ResetNetworkConfig()) {
            s_restart_wifi_pending = true;
            s_restart_udp_pending = true;
            sendLine("OK RESET_NET");
        } else {
            sendLine("ERR RESET_NET");
        }
        return;
    }

    if (strcasecmp(command, "REBOOT") == 0) {
        s_reboot_pending = true;
        s_reboot_at_ms = nowMsBle() + 500u;
        sendLine("OK REBOOT");
        return;
    }

    if (startsWith(command, "SET ")) {
        char *assignment = command + 4;
        trimInPlace(assignment);
        char *equals = strchr(assignment, '=');
        if (equals == nullptr) {
            sendLine("ERR SET");
            return;
        }

        *equals = '\0';
        char *key = assignment;
        char *value = equals + 1;
        trimInPlace(key);
        trimInPlace(value);
        for (char *p = key; *p != '\0'; ++p) {
            *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
        }

        bool restart_wifi = false;
        bool restart_udp = false;
        if (s_wifi_transaction &&
            (strcmp(key, "WIFI_SSID") == 0 || strcmp(key, "SSID") == 0 ||
             strcmp(key, "WIFI_PASS") == 0 || strcmp(key, "WIFI_PASSWORD") == 0 ||
             strcmp(key, "PASS") == 0)) {
            sendLine(stageWifiField(key, value) ? "OK SET" : "ERR SET");
            return;
        }
        if (!setField(key, value, &restart_wifi, &restart_udp) || !EXTERNAL_RADIO_SaveConfig()) {
            sendLine("ERR SET");
            return;
        }

        if (restart_wifi || restart_udp) {
            s_restart_wifi_pending = s_restart_wifi_pending || restart_wifi;
            s_restart_udp_pending = s_restart_udp_pending || restart_udp;
        }
        sendLine("OK SET");
        return;
    }

    sendLine("ERR UNKNOWN");
}

static void appendCommandData(const uint8_t *data, const size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }

    for (size_t i = 0; i < size; ++i) {
        const char ch = static_cast<char>(data[i]);
        if (ch == '\n' || ch == '\r') {
            if (s_command_size > 0) {
                s_command_buffer[s_command_size] = '\0';
                handleCommand(s_command_buffer);
                s_command_size = 0;
            }
            continue;
        }

        if (s_command_size + 1 >= sizeof(s_command_buffer)) {
            s_command_size = 0;
            sendLine("ERR LINE_TOO_LONG");
            continue;
        }

        s_command_buffer[s_command_size++] = ch;
    }
}

#if defined(CONFIG_BT_NIMBLE_ENABLED)
class ConfigServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer * /*pServer*/, NimBLEConnInfo & /*connInfo*/) override
    {
        s_client_connected = true;
        s_advertising = false;
        ESP_LOGI(TAG, "client connected");
        sendLine(NRL_FIRMWARE_BANNER " BLE config ready. Send HELP.");
    }

    void onDisconnect(NimBLEServer * /*pServer*/, NimBLEConnInfo & /*connInfo*/, int reason) override
    {
        s_client_connected = false;
        s_advertising = false;
        if (s_wifi_transaction) {
            s_wifi_transaction = false;
            s_wifi_transaction_has_ssid = false;
        }
        ESP_LOGI(TAG, "client disconnected reason=%d", reason);
        // NimBLE doesn't auto-restart advertising; kick it off explicitly via
        // the BLEConfig_Poll() path (s_advertising = false triggers restart).
    }
};

class ConfigRxCallbacks final : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo & /*connInfo*/) override
    {
        if (characteristic == nullptr) {
            return;
        }
        const NimBLEAttValue value = characteristic->getValue();
        appendCommandData(value.data(), value.size());
    }
};
#endif // CONFIG_BT_NIMBLE_ENABLED

} // namespace

// ============================================================================
// NimBLE transport (ESP32-S3 boards)
// ============================================================================
#if defined(CONFIG_BT_NIMBLE_ENABLED)

bool BLEConfig_Init(void)
{
    if (s_initialized) {
        return true;
    }

    const size_t free_internal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_internal < kBleMinFreeInternal ||
        largest_internal < kBleMinLargestInternalBlock) {
        ESP_LOGW(TAG, "BLE skipped: internal free=%u largest=%u, need free>=%u largest>=%u",
                 static_cast<unsigned>(free_internal),
                 static_cast<unsigned>(largest_internal),
                 static_cast<unsigned>(kBleMinFreeInternal),
                 static_cast<unsigned>(kBleMinLargestInternalBlock));
        return false;
    }

    NimBLEDevice::init(kBleDeviceName);
    // The shared BLE/WiFi radio cannot afford to lose modem-sleep cycles; keep
    // the default TX power. Larger advertising payloads cap out around 31 B
    // anyway for legacy 4.2 advertising.

    s_server = NimBLEDevice::createServer();
    if (s_server == nullptr) {
        ESP_LOGE(TAG, "server create failed");
        return false;
    }
    s_server->setCallbacks(new ConfigServerCallbacks());

    NimBLEService *service = s_server->createService(kServiceUuid);
    if (service == nullptr) {
        ESP_LOGE(TAG, "service create failed");
        return false;
    }

    s_tx = service->createCharacteristic(kTxUuid,
                                         NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
    s_rx = service->createCharacteristic(kRxUuid,
                                         NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    if (s_tx == nullptr || s_rx == nullptr) {
        ESP_LOGE(TAG, "characteristic create failed");
        return false;
    }

    s_rx->setCallbacks(new ConfigRxCallbacks());
    s_tx->setValue(NRL_FIRMWARE_BANNER " BLE config");

    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(kServiceUuid);
    advertising->enableScanResponse(true);
    // NimBLE 2.x no longer copies the GAP device name into advertising data
    // automatically. The WeChat mini program filters on NRL-ESP32-CFG, so an
    // unnamed advertisement is effectively invisible even though its UART
    // service UUID is present. Put the name in the scan response explicitly.
    if (!advertising->setName(kBleDeviceName)) {
        ESP_LOGE(TAG, "failed to add device name to BLE scan response");
        NimBLEDevice::deinit(true);
        s_server = nullptr;
        s_tx = nullptr;
        s_rx = nullptr;
        return false;
    }
    if (!advertising->start()) {
        ESP_LOGE(TAG, "BLE advertising start failed");
        NimBLEDevice::deinit(true);
        s_server = nullptr;
        s_tx = nullptr;
        s_rx = nullptr;
        return false;
    }

    s_initialized = true;
    s_advertising = true;
    ESP_LOGI(TAG, "advertising as %s", kBleDeviceName);
    return true;
}

bool BLEConfig_IsReady(void)
{
    return s_initialized;
}

void BLEConfig_ReportWifiResult(bool connected)
{
    if (!s_wifi_result_pending) return;
    if (connected) {
        char ip[16] = {};
        nrlIpToString(nrlWifiStaIp(), ip, sizeof(ip));
        char line[48] = {};
        snprintf(line, sizeof(line), "WIFI_STATE=GOT_IP,%s", ip);
        sendLine(line);
    } else {
        sendLine("WIFI_STATE=FAILED");
    }
    s_wifi_result_pending = false;
}

void BLEConfig_Stop(void)
{
    if (!s_initialized) {
        return;
    }
    // deinit(true) stops advertising, closes any connection, shuts the NimBLE
    // host down and disables/deinits the BT controller -- which is what
    // actually drops WiFi/BT coexistence and frees the radio for WiFi. All
    // server/characteristic objects are freed, so reset our pointers; a later
    // BLEConfig_Init() rebuilds the GATT table from scratch.
    NimBLEDevice::deinit(true);
    s_server = nullptr;
    s_tx = nullptr;
    s_rx = nullptr;
    s_initialized = false;
    s_advertising = false;
    s_client_connected = false;
    s_wifi_transaction = false;
    s_wifi_transaction_has_ssid = false;
    ESP_LOGI(TAG, "BLE stopped (radio freed for WiFi)");
}

// Bring BLE up or down based on how long the WiFi STA has been up/down, so the
// shared radio is dedicated to WiFi during normal voice operation but BLE
// provisioning returns automatically when WiFi is unavailable.
static void manageCoexistence(void)
{
    const uint32_t now = nowMsBle();
    const bool sta_up = nrlWifiStaConnected();

    if (sta_up) {
        s_sta_down_since_ms = 0u;
        if (s_sta_up_since_ms == 0u) {
            s_sta_up_since_ms = now;
        }
        // Don't yank BLE out from under an in-progress provisioning session.
        if (!s_ble_auto_stopped && s_initialized && !s_client_connected &&
            (now - s_sta_up_since_ms) >= kBleStopAfterStaUpMs) {
            ESP_LOGI(TAG, "WiFi STA stable, stopping BLE to end coexistence");
            BLEConfig_Stop();
            s_ble_auto_stopped = true;
        }
    } else {
        s_sta_up_since_ms = 0u;
        if (s_sta_down_since_ms == 0u) {
            s_sta_down_since_ms = now;
        }
        // Fallback provisioning: covers both "BLE auto-stopped after WiFi came
        // up" and "boot skipped BLE because WiFi credentials exist" (the boot
        // path only starts BLE on unconfigured devices to keep its ~66 KB of
        // internal DRAM available for the audio init). On failure (e.g. RAM
        // pressure), restart the grace period instead of retrying every poll.
        if (!s_initialized && (now - s_sta_down_since_ms) >= kBleStartAfterStaDownMs) {
            ESP_LOGI(TAG, "WiFi STA down, starting BLE for fallback provisioning");
            if (!BLEConfig_Init()) {
                ESP_LOGW(TAG, "BLE fallback init failed; retrying after grace period");
            }
            s_sta_down_since_ms = now;
            s_ble_auto_stopped = false;
        }
    }
}

void BLEConfig_Poll(void)
{
    manageCoexistence();

    if (!s_initialized) {
        return;
    }

    if (s_restart_wifi_pending || s_restart_udp_pending) {
        const bool restart_wifi = s_restart_wifi_pending;
        const bool restart_udp = s_restart_udp_pending;
        s_restart_wifi_pending = false;
        s_restart_udp_pending = false;
        NRLAudioBridge_ApplyConfig(restart_wifi, restart_udp);
    }

    if (s_wifi_result_pending && nrlWifiStaConnected()) {
        BLEConfig_ReportWifiResult(true);
    }

    if (!s_client_connected && !s_advertising) {
        NimBLEDevice::startAdvertising();
        s_advertising = true;
    }

    if (s_list_pending) {
        s_list_pending = false;
        if (s_client_connected) {
            reportCachedWifi();
        }
    }

    if (s_scan_pending) {
        s_scan_pending = false;
        if (s_client_connected) {
            runWifiScanAndReport();
        }
    }

    if (s_reboot_pending && static_cast<int32_t>(nowMsBle() - s_reboot_at_ms) >= 0) {
        ESP_LOGI(TAG, "reboot now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

bool BLEConfig_IsControllerUp(void) { return false; } // NimBLE owns no shared controller

// ============================================================================
// Bluedroid BLE transport (ESP32-S31 boards -- dual-mode Classic + BLE)
// ============================================================================
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)

#include "nrl_bt_hfp.h"

#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatt_common_api.h>
#include <esp_gatts_api.h>

// --- Bluedroid BLE state ---
namespace {

constexpr uint8_t kAppId = 0x4E; // "N"
// The S31 controller's ESP_ERR_NO_MEM cleanup path dereferences a partially
// initialized HCI transport. Avoid entering the controller when the full app
// has already fragmented internal RAM; Wi-Fi/AP provisioning remains active.
constexpr size_t kBluedroidMinFreeInternal = 96u * 1024u;
constexpr size_t kBluedroidMinLargestInternalBlock = 48u * 1024u;

// 128-bit NUS UUIDs (little-endian byte order for esp_bt_uuid_t).
const uint8_t kNusServiceUuid128[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e};
const uint8_t kNusRxUuid128[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e};
const uint8_t kNusTxUuid128[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e};

esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
uint16_t s_service_handle = 0;
uint16_t s_tx_val_handle = 0;
uint16_t s_conn_id = 0;
bool s_controller_up = false;
bool s_adv_data_configured = false;

// Coexistence: bring BLE up when no network (Ethernet+WiFi+cellular) is
// available; tear it down once any interface connects.
constexpr uint32_t kBleStopAfterNetUpMs = 4000u;
constexpr uint32_t kBleStartAfterNetDownMs = 30000u;
bool s_ble_auto_stopped = false;
uint32_t s_net_up_since_ms = 0u;
uint32_t s_net_down_since_ms = 0u;

static void sendLine(const char *line)
{
    if (line == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "%s", line);
    if (s_tx_val_handle == 0 || !s_client_connected) {
        return;
    }
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_tx_val_handle,
                                static_cast<uint16_t>(strlen(line)),
                                const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(line)),
                                false);
}

// --- Advertising parameters ---
static esp_ble_adv_params_t s_adv_params = {};
static void initAdvParams(void)
{
    s_adv_params.adv_int_min = 0x20;
    s_adv_params.adv_int_max = 0x40;
    s_adv_params.adv_type = ADV_TYPE_IND;
    s_adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    s_adv_params.channel_map = ADV_CHNL_ALL;
    s_adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
}

static void configAdvData(void)
{
    // Service UUID in advertising data.
    esp_ble_adv_data_t adv_data = {};
    adv_data.set_scan_rsp = false;
    adv_data.include_name = false;
    adv_data.include_txpower = true;
    adv_data.service_uuid_len = 16;
    adv_data.p_service_uuid = const_cast<uint8_t *>(kNusServiceUuid128);
    esp_ble_gap_config_adv_data(&adv_data);

    // Device name in scan response.
    esp_ble_adv_data_t scan_rsp = {};
    scan_rsp.set_scan_rsp = true;
    scan_rsp.include_name = true;
    esp_ble_gap_config_adv_data(&scan_rsp);
}

// --- GATT event handler ---
static void gattsEventHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                              esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name(kBleDeviceName);
        // Build the NUS service attribute table.
        esp_gatt_srvc_id_t srvc_id = {};
        srvc_id.id.inst_id = 0;
        srvc_id.id.uuid.len = ESP_UUID_LEN_128;
        memcpy(srvc_id.id.uuid.uuid.uuid128, kNusServiceUuid128, 16);
        srvc_id.is_primary = true;
        esp_ble_gatts_create_service(gatts_if, &srvc_id, 4);
        break;
    }
    case ESP_GATTS_CREATE_EVT: {
        s_service_handle = param->create.service_handle;
        // RX characteristic (write)
        esp_bt_uuid_t rx_uuid;
        rx_uuid.len = ESP_UUID_LEN_128;
        memcpy(rx_uuid.uuid.uuid128, kNusRxUuid128, 16);
        esp_attr_control_t rx_ctrl = {.auto_rsp = ESP_GATT_AUTO_RSP};
        esp_ble_gatts_add_char(s_service_handle, &rx_uuid,
                               ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_WRITE |
                               ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                               nullptr, &rx_ctrl);
        break;
    }
    case ESP_GATTS_ADD_CHAR_EVT: {
        // Determine which char was added by comparing UUIDs.
        if (param->add_char.char_uuid.len == ESP_UUID_LEN_128 &&
            memcmp(param->add_char.char_uuid.uuid.uuid128, kNusRxUuid128, 16) == 0) {
            // RX added; now add TX characteristic (notify + read).
            esp_bt_uuid_t tx_uuid;
            tx_uuid.len = ESP_UUID_LEN_128;
            memcpy(tx_uuid.uuid.uuid128, kNusTxUuid128, 16);
            esp_attr_control_t tx_ctrl = {.auto_rsp = ESP_GATT_AUTO_RSP};
            esp_ble_gatts_add_char(s_service_handle, &tx_uuid,
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY |
                                   ESP_GATT_CHAR_PROP_BIT_READ,
                                   nullptr, &tx_ctrl);
        } else {
            // TX added; store its value handle and start the service.
            s_tx_val_handle = param->add_char.attr_handle + 1;
            esp_ble_gatts_start_service(s_service_handle);
        }
        break;
    }
    case ESP_GATTS_CONNECT_EVT: {
        s_client_connected = true;
        s_advertising = false;
        s_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "client connected");
        sendLine(NRL_FIRMWARE_BANNER " BLE config ready. Send HELP.");
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT: {
        s_client_connected = false;
        s_advertising = false;
        if (s_wifi_transaction) {
            s_wifi_transaction = false;
            s_wifi_transaction_has_ssid = false;
        }
        ESP_LOGI(TAG, "client disconnected reason=%d", param->disconnect.reason);
        break;
    }
    case ESP_GATTS_WRITE_EVT: {
        if (!param->write.is_prep) {
            appendCommandData(param->write.value, param->write.len);
        }
        break;
    }
    default:
        break;
    }
}

// --- GAP event handler (advertising lifecycle) ---
static void gapEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        // Main adv data set; scan response will follow.
        s_adv_data_configured = true;
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        // Both adv data and scan response configured; start advertising.
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_advertising = true;
            ESP_LOGI(TAG, "advertising as %s", kBleDeviceName);
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_advertising = false;
        break;
    default:
        break;
    }
}

// --- Coexistence: manage BLE based on overall network connectivity ---
static void manageCoexistence(void)
{
    const uint32_t now = nowMsBle();
    const bool net_up = nrlNetworkConnected();

    if (net_up) {
        s_net_down_since_ms = 0u;
        if (s_net_up_since_ms == 0u) {
            s_net_up_since_ms = now;
        }
        if (!s_ble_auto_stopped && s_initialized && !s_client_connected &&
            (now - s_net_up_since_ms) >= kBleStopAfterNetUpMs) {
            ESP_LOGI(TAG, "network stable, stopping BLE provisioning");
            BLEConfig_Stop();
            s_ble_auto_stopped = true;
        }
    } else {
        s_net_up_since_ms = 0u;
        if (s_net_down_since_ms == 0u) {
            s_net_down_since_ms = now;
        }
        // Do not start BLE if HFP is using the BT controller.
        if (!s_initialized && (now - s_net_down_since_ms) >= kBleStartAfterNetDownMs) {
            if (NRL_BtHfp_IsEnabled()) {
                ESP_LOGI(TAG, "network down but HFP active; skipping BLE fallback");
            } else {
                ESP_LOGI(TAG, "network down, starting BLE for fallback provisioning");
                if (!BLEConfig_Init()) {
                    ESP_LOGW(TAG, "BLE fallback init failed; retrying after grace period");
                }
            }
            s_net_down_since_ms = now;
            s_ble_auto_stopped = false;
        }
    }
}

} // namespace

// --- Bluedroid public API ---

bool BLEConfig_Init(void)
{
    if (s_initialized) {
        return true;
    }

    // Bring up the BT controller in dual mode if not already running.
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        const size_t free_internal =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t largest_internal =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (free_internal < kBluedroidMinFreeInternal ||
            largest_internal < kBluedroidMinLargestInternalBlock) {
            ESP_LOGW(TAG,
                     "BLE skipped: internal free=%u largest=%u, need free>=%u largest>=%u",
                     static_cast<unsigned>(free_internal),
                     static_cast<unsigned>(largest_internal),
                     static_cast<unsigned>(kBluedroidMinFreeInternal),
                     static_cast<unsigned>(kBluedroidMinLargestInternalBlock));
            return false;
        }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#pragma GCC diagnostic pop
        esp_err_t err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "controller enable BTDM: %s", esp_err_to_name(err));
            esp_bt_controller_deinit();
            return false;
        }
        s_controller_up = true;
    }

    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
            esp_bluedroid_deinit();
        }
        esp_err_t err = esp_bluedroid_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bluedroid init: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_bluedroid_enable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(err));
            esp_bluedroid_deinit();
            return false;
        }
    }

    // Register GAP and GATTS callbacks.
    esp_ble_gap_register_callback(gapEventHandler);
    esp_ble_gatts_register_callback(gattsEventHandler);
    esp_ble_gatts_app_register(kAppId);

    // Configure advertising data (name + NUS service UUID).
    esp_ble_gap_set_device_name(kBleDeviceName);
    initAdvParams();
    configAdvData();

    s_initialized = true;
    ESP_LOGI(TAG, "BLE provisioning started (Bluedroid dual-mode)");
    return true;
}

bool BLEConfig_IsReady(void)
{
    return s_initialized;
}

bool BLEConfig_IsControllerUp(void)
{
    return s_controller_up && s_initialized;
}

void BLEConfig_ReportWifiResult(bool connected)
{
    if (!s_wifi_result_pending) return;
    if (connected) {
        char ip[16] = {};
        nrlIpToString(nrlWifiStaIp(), ip, sizeof(ip));
        char line[48] = {};
        snprintf(line, sizeof(line), "WIFI_STATE=GOT_IP,%s", ip);
        sendLine(line);
    } else {
        sendLine("WIFI_STATE=FAILED");
    }
    s_wifi_result_pending = false;
}

void BLEConfig_Stop(void)
{
    if (!s_initialized) {
        return;
    }
    if (s_gatts_if != ESP_GATT_IF_NONE) {
        esp_ble_gatts_app_unregister(s_gatts_if);
    }
    esp_ble_gap_stop_advertising();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    if (s_controller_up) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_controller_up = false;
    }
    s_gatts_if = ESP_GATT_IF_NONE;
    s_service_handle = 0;
    s_tx_val_handle = 0;
    s_conn_id = 0;
    s_adv_data_configured = false;
    s_initialized = false;
    s_advertising = false;
    s_client_connected = false;
    s_wifi_transaction = false;
    s_wifi_transaction_has_ssid = false;
    ESP_LOGI(TAG, "BLE stopped (Bluedroid + controller released)");
}

void BLEConfig_Poll(void)
{
    manageCoexistence();

    if (!s_initialized) {
        return;
    }

    if (s_restart_wifi_pending || s_restart_udp_pending) {
        const bool restart_wifi = s_restart_wifi_pending;
        const bool restart_udp = s_restart_udp_pending;
        s_restart_wifi_pending = false;
        s_restart_udp_pending = false;
        NRLAudioBridge_ApplyConfig(restart_wifi, restart_udp);
    }

    if (s_wifi_result_pending && nrlWifiStaConnected()) {
        BLEConfig_ReportWifiResult(true);
    }

    if (!s_client_connected && !s_advertising) {
        esp_ble_gap_start_advertising(&s_adv_params);
    }

    if (s_list_pending) {
        s_list_pending = false;
        if (s_client_connected) {
            reportCachedWifi();
        }
    }

    if (s_scan_pending) {
        s_scan_pending = false;
        if (s_client_connected) {
            runWifiScanAndReport();
        }
    }

    if (s_reboot_pending && static_cast<int32_t>(nowMsBle() - s_reboot_at_ms) >= 0) {
        ESP_LOGI(TAG, "reboot now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

#else // No BT host available at all.

bool BLEConfig_Init(void) { return false; }
void BLEConfig_Poll(void) {}
bool BLEConfig_IsReady(void) { return false; }
void BLEConfig_ReportWifiResult(bool) {}
void BLEConfig_Stop(void) {}
bool BLEConfig_IsControllerUp(void) { return false; }

#endif // CONFIG_BT_NIMBLE_ENABLED / CONFIG_BT_BLUEDROID_ENABLED

#endif // CONFIG_BT_NIMBLE_ENABLED || CONFIG_BT_BLUEDROID_ENABLED
