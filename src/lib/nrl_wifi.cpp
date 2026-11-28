#include "nrl_wifi.h"

#include "ble_config.h"
#include "nrl_bt_hfp.h"
#include "nrl_hostname.h"
#include "nrl_mdns.h"
#include "nrl_net_compat.h"

#include "../app/driver/external_radio.h"

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>

#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "WiFi";

namespace
{

    constexpr size_t kScanCacheMax = 24u;
    constexpr uint16_t kScanFetchMax = 32u;

    portMUX_TYPE s_mutex_init_spin = portMUX_INITIALIZER_UNLOCKED;
    StaticSemaphore_t s_wifi_mutex_buf;
    SemaphoreHandle_t s_wifi_mutex = nullptr;
    StaticSemaphore_t s_scan_done_buf;
    SemaphoreHandle_t s_scan_done = nullptr;
    StaticSemaphore_t s_scan_mutex_buf;
    SemaphoreHandle_t s_scan_mutex = nullptr;

    bool s_inited = false;
    bool s_started = false;
    esp_netif_t *s_sta_netif = nullptr;
    esp_netif_t *s_ap_netif = nullptr;

    volatile bool s_sta_active = false; // STA mode requested
    volatile bool s_ap_active = false;  // AP mode requested

    volatile bool s_sta_associated = false;
    volatile bool s_sta_has_ip = false;
    volatile uint8_t s_last_disconnect_reason = 0u;
    std::atomic<uint32_t> s_disconnect_count{0u};
    std::atomic<uint32_t> s_connect_abort_gen{0u};

    // Last successfully applied STA connection parameters, used to ensure
    // WIFI_ALREADY_ON_SSID is not returned when password or IP config changed.
    bool s_last_sta_valid = false;
    char s_last_sta_ssid[33] = {};
    char s_last_sta_pass[65] = {};
    bool s_last_sta_dhcp = true;
    uint32_t s_last_sta_ip = 0u;
    uint32_t s_last_sta_mask = 0u;
    uint32_t s_last_sta_gw = 0u;
    uint32_t s_last_sta_dns = 0u;

    NrlWifiScanResult s_scan_cache[kScanCacheMax];
    size_t s_scan_count = 0u;

    static void ensureSyncObjects()
    {
        if (s_wifi_mutex != nullptr && s_scan_done != nullptr && s_scan_mutex != nullptr)
        {
            return;
        }
        portENTER_CRITICAL(&s_mutex_init_spin);
        if (s_wifi_mutex == nullptr)
        {
            s_wifi_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_wifi_mutex_buf);
        }
        if (s_scan_done == nullptr)
        {
            s_scan_done = xSemaphoreCreateBinaryStatic(&s_scan_done_buf);
        }
        if (s_scan_mutex == nullptr)
        {
            s_scan_mutex = xSemaphoreCreateMutexStatic(&s_scan_mutex_buf);
        }
        portEXIT_CRITICAL(&s_mutex_init_spin);
    }

    class WifiLockGuard
    {
    public:
        explicit WifiLockGuard(TickType_t timeout = portMAX_DELAY)
        {
            ensureSyncObjects();
            if (s_wifi_mutex != nullptr)
            {
                m_locked = (xSemaphoreTakeRecursive(s_wifi_mutex, timeout) == pdTRUE);
            }
        }
        ~WifiLockGuard()
        {
            if (m_locked && s_wifi_mutex != nullptr)
            {
                xSemaphoreGiveRecursive(s_wifi_mutex);
            }
        }
        bool locked() const { return m_locked; }

    private:
        bool m_locked = false;
    };

    static inline uint32_t nowMs()
    {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    }

    static void invalidateLastStaConfig()
    {
        s_last_sta_valid = false;
        s_last_sta_ssid[0] = '\0';
        s_last_sta_pass[0] = '\0';
    }

    static bool isTerminalConnectReason(uint8_t reason)
    {
        switch (reason)
        {
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_NO_AP_FOUND:
            return true;
        default:
            return false;
        }
    }

    static const char *disconnectReasonText(uint8_t reason)
    {
        switch (reason)
        {
        case WIFI_REASON_AUTH_EXPIRE:
            return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE:
            return "AUTH_LEAVE";
        case WIFI_REASON_AUTH_FAIL:
            return "AUTH_FAIL";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "4WAY_HANDSHAKE_TIMEOUT(wrong pass?)";
        case WIFI_REASON_NO_AP_FOUND:
            return "NO_AP_FOUND";
        case WIFI_REASON_CONNECTION_FAIL:
            return "CONNECTION_FAIL";
#ifdef WIFI_REASON_ASSOC_EXPIRE
        case WIFI_REASON_ASSOC_EXPIRE:
            return "ASSOC_EXPIRE";
#endif
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_MIC_FAILURE:
            return "MIC_FAILURE";
        default:
            return "OTHER";
        }
    }

    static void wifiEventHandler(void *, esp_event_base_t base, int32_t id, void *data)
    {
        if (base == WIFI_EVENT)
        {
            switch (id)
            {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "evt STA_START");
                break;
            case WIFI_EVENT_STA_STOP:
                ESP_LOGI(TAG, "evt STA_STOP");
                s_sta_associated = false;
                s_sta_has_ip = false;
                break;
            case WIFI_EVENT_STA_CONNECTED:
            {
                auto *e = static_cast<wifi_event_sta_connected_t *>(data);
                s_sta_associated = true;
                ESP_LOGI(TAG, "evt STA_CONNECTED ssid=%.*s channel=%u",
                         static_cast<int>(e->ssid_len),
                         reinterpret_cast<const char *>(e->ssid),
                         static_cast<unsigned>(e->channel));
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED:
            {
                auto *e = static_cast<wifi_event_sta_disconnected_t *>(data);
                s_sta_associated = false;
                s_sta_has_ip = false;
                s_last_disconnect_reason = e->reason;
                s_disconnect_count.fetch_add(1u, std::memory_order_relaxed);
                // Heap snapshot pins "driver starved of internal RAM"
                // vs RF/software causes when the link drops mid-traffic.
                ESP_LOGW(TAG, "evt STA_DISCONNECTED reason=%u (%s) rssi=%d dram free=%u largest=%u min=%u",
                         static_cast<unsigned>(e->reason),
                         disconnectReasonText(e->reason),
                         static_cast<int>(e->rssi),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
                break;
            }
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "evt AP_START");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "evt AP_STOP");
                break;
            case WIFI_EVENT_AP_STACONNECTED:
            {
                auto *e = static_cast<wifi_event_ap_staconnected_t *>(data);
                ESP_LOGI(TAG, "evt AP client connected mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u",
                         e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                         static_cast<unsigned>(e->aid));
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED:
            {
                auto *e = static_cast<wifi_event_ap_stadisconnected_t *>(data);
                ESP_LOGI(TAG, "evt AP client disconnected aid=%u", static_cast<unsigned>(e->aid));
                break;
            }
            case WIFI_EVENT_SCAN_DONE:
                // Default event loop runs in task context (sys_evt), not an ISR.
                if (s_scan_done != nullptr)
                {
                    xSemaphoreGive(s_scan_done);
                }
                break;
            default:
                break;
            }
        }
        else if (base == IP_EVENT)
        {
            if (id == IP_EVENT_STA_GOT_IP)
            {
                auto *e = static_cast<ip_event_got_ip_t *>(data);
                s_sta_has_ip = true;
                char ip_buf[16] = {};
                nrlIpToString(e->ip_info.ip.addr, ip_buf, sizeof(ip_buf));
                ESP_LOGI(TAG, "evt STA_GOT_IP %s", ip_buf);
            }
            else if (id == IP_EVENT_STA_LOST_IP)
            {
                s_sta_has_ip = false;
                ESP_LOGW(TAG, "evt STA_LOST_IP");
            }
        }
    }

    static void applyStaRadioPolicy(wifi_mode_t mode)
    {
        if (!s_started || (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA))
        {
            return;
        }
        // Disable WiFi modem sleep for low-latency 20 ms G.711 voice unless BT
        // or BLE shares the single 2.4 GHz radio:
        // - Classic BT HFP SCO requires WIFI_PS_MAX_MODEM to avoid coex timeouts.
        // - BLE provisioning requires at least WIFI_PS_MIN_MODEM per ESP-IDF coex.
        wifi_ps_type_t ps = WIFI_PS_NONE;
        if (NRL_BtHfp_IsEnabled())
        {
            ps = WIFI_PS_MAX_MODEM;
        }
        else if (BLEConfig_IsReady())
        {
            ps = WIFI_PS_MIN_MODEM;
        }
        const esp_err_t ps_err = esp_wifi_set_ps(ps);
        if (ps_err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_wifi_set_ps(%d) failed: %s",
                     static_cast<int>(ps), esp_err_to_name(ps_err));
        }

        // Must be called after esp_wifi_start() and only when STA is active.
        const esp_err_t ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to set HT20: %s", esp_err_to_name(ret));
        }
    }

    static bool refreshMode()
    {
        wifi_mode_t target = WIFI_MODE_NULL;
        if (s_sta_active && s_ap_active)
        {
            target = WIFI_MODE_APSTA;
        }
        else if (s_sta_active)
        {
            target = WIFI_MODE_STA;
        }
        else if (s_ap_active)
        {
            target = WIFI_MODE_AP;
        }

        wifi_mode_t current = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&current) != ESP_OK)
        {
            current = WIFI_MODE_NULL;
        }

        if (target == WIFI_MODE_NULL)
        {
            if (s_started)
            {
                const esp_err_t stop_err = esp_wifi_stop();
                if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT)
                {
                    ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(stop_err));
                }
                s_started = false;
            }
            // Clear driver mode to WIFI_MODE_NULL so a later restart into the
            // same mode does not see target == current while s_started == false.
            if (current != WIFI_MODE_NULL)
            {
                (void)esp_wifi_set_mode(WIFI_MODE_NULL);
            }
            s_sta_associated = false;
            s_sta_has_ip = false;
            invalidateLastStaConfig();
            return true;
        }

        if (target == current && s_started)
        {
            applyStaRadioPolicy(target);
            return true;
        }

        if (target != current)
        {
            const esp_err_t err = esp_wifi_set_mode(target);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_set_mode(%d) failed: %s",
                         static_cast<int>(target), esp_err_to_name(err));
                return false;
            }
        }

        if (!s_started)
        {
            const esp_err_t err2 = esp_wifi_start();
            if (err2 != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err2));
                return false;
            }
            s_started = true;
        }

        applyStaRadioPolicy(target);
        return true;
    }

    static void makeHostname(char *buf, size_t buf_size)
    {
        const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
        NrlMakeHostname(config != nullptr ? config->callsign : nullptr,
                        config != nullptr ? config->callsign_ssid : 0u,
                        buf, buf_size);
    }

} // namespace

bool nrlWifiInit()
{
    WifiLockGuard lock;
    if (!lock.locked())
    {
        return false;
    }
    if (s_inited)
    {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init failed: %d", err);
        return false;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", err);
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", err);
        return false;
    }

    if (s_sta_netif == nullptr)
    {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_ap_netif == nullptr)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (s_sta_netif == nullptr || s_ap_netif == nullptr)
    {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi failed");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %d (internal free=%u largest=%u)", err,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return false;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifiEventHandler, nullptr);

    s_inited = true;
    nrlMdnsRefresh();
    ESP_LOGI(TAG, "WiFi stack initialized");
    return true;
}

bool wifiIsConnected()
{
    return s_sta_has_ip;
}

void nrlWifiRefreshPowerSave()
{
    WifiLockGuard lock(pdMS_TO_TICKS(500));
    if (!lock.locked() || !s_inited || !s_started)
    {
        return;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK)
    {
        applyStaRadioPolicy(mode);
    }
}

void nrlWifiDisconnect()
{
    // Bump abort generation before taking the mutex so any in-flight
    // wifiEnsureConnected() polling loop exits promptly.
    s_connect_abort_gen.fetch_add(1u, std::memory_order_relaxed);

    WifiLockGuard lock;
    if (!lock.locked())
    {
        return;
    }
    invalidateLastStaConfig();
    if (s_inited && s_started && s_sta_active)
    {
        (void)esp_wifi_disconnect();
    }
    s_sta_associated = false;
    s_sta_has_ip = false;
}

WifiConnResult wifiEnsureConnected(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (ssid == nullptr || *ssid == '\0')
    {
        ESP_LOGE(TAG, "connect failed: empty SSID");
        return WIFI_CONN_FAILED;
    }

    WifiLockGuard lock;
    if (!lock.locked())
    {
        return WIFI_CONN_FAILED;
    }
    if (!nrlWifiInit())
    {
        return WIFI_CONN_FAILED;
    }

    const uint32_t my_abort_gen = s_connect_abort_gen;
    const char *pass_str = (pass != nullptr) ? pass : "";
    const size_t pass_len = strlen(pass_str);

    // Snapshot IP settings once so static-vs-DHCP decision is consistent.
    const ExternalRadioConfig *net_config = EXTERNAL_RADIO_GetConfig();
    const bool use_static = (net_config != nullptr && !net_config->wifi_dhcp_enabled &&
                             net_config->wifi_ip != 0u && net_config->wifi_netmask != 0u &&
                             net_config->wifi_gateway != 0u);
    const uint32_t cfg_ip = use_static ? net_config->wifi_ip : 0u;
    const uint32_t cfg_mask = use_static ? net_config->wifi_netmask : 0u;
    const uint32_t cfg_gw = use_static ? net_config->wifi_gateway : 0u;
    const uint32_t cfg_dns = use_static
                                 ? ((net_config->wifi_dns != 0u) ? net_config->wifi_dns : net_config->wifi_gateway)
                                 : 0u;

    ESP_LOGI(TAG, "connecting: SSID=\"%s\" passLen=%u timeout=%lums",
             ssid,
             static_cast<unsigned>(pass_len),
             static_cast<unsigned long>(timeout_ms));

    // Already on the target SSID with an IP AND unchanged credentials/IP settings?
    if (s_sta_has_ip && s_last_sta_valid &&
        strcmp(s_last_sta_ssid, ssid) == 0 &&
        strcmp(s_last_sta_pass, pass_str) == 0 &&
        s_last_sta_dhcp == !use_static &&
        (!use_static || (s_last_sta_ip == cfg_ip && s_last_sta_mask == cfg_mask &&
                         s_last_sta_gw == cfg_gw && s_last_sta_dns == cfg_dns)))
    {
        wifi_ap_record_t info = {};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK &&
            strncmp(reinterpret_cast<const char *>(info.ssid), ssid, sizeof(info.ssid)) == 0)
        {
            char ip_buf[16] = {};
            nrlIpToString(nrlWifiStaIp(), ip_buf, sizeof(ip_buf));
            ESP_LOGI(TAG, "already on target SSID, IP=%s, RSSI=%ddBm",
                     ip_buf, static_cast<int>(info.rssi));
            return WIFI_ALREADY_ON_SSID;
        }
    }

    // If previously associated or holding an IP, disconnect cleanly and wait
    // briefly for STA_DISCONNECTED so the netif drops before reconfiguring IP.
    if (s_started && s_sta_active && (s_sta_associated || s_sta_has_ip))
    {
        ESP_LOGI(TAG, "disconnecting current STA session before reconnecting");
        (void)esp_wifi_disconnect();
        const uint32_t disc_t0 = nowMs();
        while ((s_sta_associated || s_sta_has_ip) && (nowMs() - disc_t0) < 400u)
        {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }

    // Activate STA mode (preserve AP if already up).
    const bool prev_sta_active = s_sta_active;
    s_sta_active = true;
    if (!refreshMode())
    {
        s_sta_active = prev_sta_active;
        return WIFI_CONN_FAILED;
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // let the STA interface settle

    char hostname[40] = {};
    makeHostname(hostname, sizeof(hostname));
    esp_netif_set_hostname(s_sta_netif, hostname);
    nrlMdnsRefresh();

    // Reset state flags before configuring IP/DHCP so we never clobber a newly
    // posted IP_EVENT_STA_GOT_IP.
    s_sta_associated = false;
    s_sta_has_ip = false;
    s_last_disconnect_reason = 0u;
    const uint32_t start_disconnects = s_disconnect_count;

    // Static IP or DHCP.
    if (use_static)
    {
        const esp_err_t stop_err = esp_netif_dhcpc_stop(s_sta_netif);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
        {
            ESP_LOGW(TAG, "esp_netif_dhcpc_stop failed: %s", esp_err_to_name(stop_err));
        }
        esp_netif_ip_info_t info = {};
        info.ip.addr = cfg_ip;
        info.netmask.addr = cfg_mask;
        info.gw.addr = cfg_gw;
        const esp_err_t ip_err = esp_netif_set_ip_info(s_sta_netif, &info);
        if (ip_err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(ip_err));
        }
        esp_netif_dns_info_t dns = {};
        dns.ip.u_addr.ip4.addr = cfg_dns;
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        (void)esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        char ip_buf[16] = {}, mask_buf[16] = {}, gw_buf[16] = {}, dns_buf[16] = {};
        nrlIpToString(info.ip.addr, ip_buf, sizeof(ip_buf));
        nrlIpToString(info.netmask.addr, mask_buf, sizeof(mask_buf));
        nrlIpToString(info.gw.addr, gw_buf, sizeof(gw_buf));
        nrlIpToString(dns.ip.u_addr.ip4.addr, dns_buf, sizeof(dns_buf));
        ESP_LOGI(TAG, "static config: ip=%s mask=%s gateway=%s dns=%s",
                 ip_buf, mask_buf, gw_buf, dns_buf);
    }
    else
    {
        if (!s_last_sta_dhcp)
        {
            // Clear leftover static IP before starting DHCP so stale IP does not
            // make nrlWifiStaConnected() report connected prematurely.
            (void)esp_netif_dhcpc_stop(s_sta_netif);
            esp_netif_ip_info_t zero = {};
            (void)esp_netif_set_ip_info(s_sta_netif, &zero);
        }
        const esp_err_t dhcp_err = esp_netif_dhcpc_start(s_sta_netif);
        if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        {
            ESP_LOGW(TAG, "esp_netif_dhcpc_start failed: %s", esp_err_to_name(dhcp_err));
        }
    }

    // Build STA config. Accept WPA-PSK and above when a password is set so
    // WPA/WPA2 mixed routers remain reachable without permitting open downgrade.
    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
    if (pass_len > 0u)
    {
        strncpy(reinterpret_cast<char *>(cfg.sta.password), pass_str, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = (pass_len > 0u) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    const esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (cfg_err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(cfg_err));
        return WIFI_CONN_FAILED;
    }

    const esp_err_t conn_err = esp_wifi_connect();
    if (conn_err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(conn_err));
        invalidateLastStaConfig();
        return WIFI_CONN_FAILED;
    }

    const uint32_t t0 = nowMs();
    uint32_t last_log = 0u;
    uint32_t last_reconnect = 0u;
    while (!s_sta_has_ip && (nowMs() - t0) < timeout_ms)
    {
        if (s_connect_abort_gen != my_abort_gen)
        {
            ESP_LOGI(TAG, "connect aborted by newer request");
            (void)esp_wifi_disconnect();
            invalidateLastStaConfig();
            return WIFI_CONN_FAILED;
        }

        const uint32_t elapsed = nowMs() - t0;
        if (elapsed - last_log >= 2000u)
        {
            ESP_LOGI(TAG, "+%lums assoc=%d has_ip=%d last_reason=%u",
                     static_cast<unsigned long>(elapsed),
                     s_sta_associated ? 1 : 0,
                     s_sta_has_ip ? 1 : 0,
                     static_cast<unsigned>(s_last_disconnect_reason));
            last_log = elapsed;
        }

        // Stop wasting the full timeout if we already failed twice with a
        // definitive auth/missing-AP reason.
        const uint32_t disconnects = s_disconnect_count - start_disconnects;
        if (!s_sta_associated && disconnects >= 2u &&
            isTerminalConnectReason(s_last_disconnect_reason))
        {
            ESP_LOGW(TAG, "aborting connect early after %lu terminal failures: reason=%u (%s)",
                     static_cast<unsigned long>(disconnects),
                     static_cast<unsigned>(s_last_disconnect_reason),
                     disconnectReasonText(s_last_disconnect_reason));
            break;
        }

        // If we haven't even associated after 5s, kick connect again. Don't
        // retry if we're past L2 association — that means DHCP is in progress.
        if (!s_sta_associated && elapsed >= 5000u && (elapsed - last_reconnect) >= 5000u)
        {
            ESP_LOGI(TAG, "still not associating, retrying connect");
            (void)esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(50));
            const esp_err_t retry_err = esp_wifi_connect();
            if (retry_err != ESP_OK)
            {
                ESP_LOGW(TAG, "retry esp_wifi_connect failed: %s", esp_err_to_name(retry_err));
                break;
            }
            last_reconnect = elapsed;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (s_sta_has_ip)
    {
        snprintf(s_last_sta_ssid, sizeof(s_last_sta_ssid), "%s", ssid);
        snprintf(s_last_sta_pass, sizeof(s_last_sta_pass), "%s", pass_str);
        s_last_sta_dhcp = !use_static;
        s_last_sta_ip = cfg_ip;
        s_last_sta_mask = cfg_mask;
        s_last_sta_gw = cfg_gw;
        s_last_sta_dns = cfg_dns;
        s_last_sta_valid = true;

        char ip_buf[16] = {};
        nrlIpToString(nrlWifiStaIp(), ip_buf, sizeof(ip_buf));
        wifi_ap_record_t ap_info = {};
        esp_wifi_sta_get_ap_info(&ap_info);
        ESP_LOGI(TAG, "connected! elapsed=%lums IP=%s RSSI=%ddBm channel=%u hostname=%s",
                 static_cast<unsigned long>(nowMs() - t0),
                 ip_buf,
                 static_cast<int>(ap_info.rssi),
                 static_cast<unsigned>(ap_info.primary),
                 hostname);
        return WIFI_CONN_OK;
    }

    // Explicitly stop the failed STA connection attempt so the driver does not
    // keep channel-hopping in the background and drop SoftAP clients.
    (void)esp_wifi_disconnect();
    invalidateLastStaConfig();

    ESP_LOGW(TAG, "connect timeout: elapsed=%lums assoc=%d last_reason=%u (%s)",
             static_cast<unsigned long>(nowMs() - t0),
             s_sta_associated ? 1 : 0,
             static_cast<unsigned>(s_last_disconnect_reason),
             disconnectReasonText(s_last_disconnect_reason));
    return WIFI_CONN_TIMEOUT;
}

bool nrlWifiApStart(const char *ssid, uint8_t channel, uint8_t max_conn,
                    uint32_t ap_ip, uint32_t gw, uint32_t mask)
{
    if (ssid == nullptr || *ssid == '\0')
    {
        return false;
    }

    WifiLockGuard lock;
    if (!lock.locked())
    {
        return false;
    }
    if (!nrlWifiInit())
    {
        return false;
    }

    const bool prev_ap_active = s_ap_active;
    const wifi_mode_t target = s_sta_active ? WIFI_MODE_APSTA : WIFI_MODE_AP;

    wifi_mode_t prev_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&prev_mode) != ESP_OK)
    {
        prev_mode = WIFI_MODE_NULL;
    }

    // Set mode and configure AP parameters BEFORE esp_wifi_start() when the
    // radio is stopped, so we never briefly beacon a stale/default SSID.
    if (prev_mode != target)
    {
        const esp_err_t mode_err = esp_wifi_set_mode(target);
        if (mode_err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_set_mode(%d) for AP failed: %s",
                     static_cast<int>(target), esp_err_to_name(mode_err));
            return false;
        }
    }

    char hostname[40] = {};
    makeHostname(hostname, sizeof(hostname));
    esp_netif_set_hostname(s_ap_netif, hostname);
    nrlMdnsRefresh();

    // DHCP server must be stopped before changing the IP info.
    (void)esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t info = {};
    info.ip.addr = ap_ip;
    info.gw.addr = gw;
    info.netmask.addr = mask;
    const esp_err_t ip_err = esp_netif_set_ip_info(s_ap_netif, &info);
    if (ip_err != ESP_OK)
    {
        ESP_LOGW(TAG, "AP esp_netif_set_ip_info failed: %s", esp_err_to_name(ip_err));
    }
    const esp_err_t dhcps_err = esp_netif_dhcps_start(s_ap_netif);
    if (dhcps_err != ESP_OK && dhcps_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    {
        ESP_LOGW(TAG, "AP esp_netif_dhcps_start failed: %s", esp_err_to_name(dhcps_err));
    }

    wifi_config_t cfg = {};
    const size_t ssid_len = strnlen(ssid, sizeof(cfg.ap.ssid));
    memcpy(cfg.ap.ssid, ssid, ssid_len);
    cfg.ap.ssid_len = static_cast<uint8_t>(ssid_len);
    cfg.ap.channel = channel;
    cfg.ap.max_connection = (max_conn == 0u) ? 4u : max_conn;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.ssid_hidden = 0;
    cfg.ap.pmf_cfg.required = false;

    const esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (cfg_err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(cfg_err));
        if (!prev_ap_active && prev_mode != target)
        {
            (void)esp_wifi_set_mode(prev_mode);
        }
        return false;
    }

    if (!s_started)
    {
        const esp_err_t start_err = esp_wifi_start();
        if (start_err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_start (AP) failed: %s", esp_err_to_name(start_err));
            if (!prev_ap_active)
            {
                (void)esp_wifi_set_mode(prev_mode);
            }
            return false;
        }
        s_started = true;
    }

    s_ap_active = true;
    applyStaRadioPolicy(target);

    char ip_buf[16] = {};
    nrlIpToString(ap_ip, ip_buf, sizeof(ip_buf));
    ESP_LOGI(TAG, "AP ready: ssid=%s open ip=%s channel=%u max=%u",
             ssid, ip_buf, static_cast<unsigned>(channel), static_cast<unsigned>(cfg.ap.max_connection));
    return true;
}

bool nrlWifiApStop()
{
    WifiLockGuard lock;
    if (!lock.locked())
    {
        return false;
    }
    if (!s_ap_active)
    {
        return true;
    }
    s_ap_active = false;
    const bool ok = refreshMode();
    ESP_LOGI(TAG, "AP stopped");
    return ok;
}

bool nrlWifiApIsActive()
{
    return s_ap_active;
}

// Master radio stop: deactivate STA + AP and drop to WIFI_MODE_NULL, which calls
// esp_wifi_stop() -- freeing BOTH the shared 2.4 GHz radio AND WiFi's dynamic RAM
// so Bluetooth A2DP can register and stream. wifiEnsureConnected() brings it back
// (esp_wifi_start + reconnect) when the Wi-Fi master switch is turned on again.
bool nrlWifiStopRadio()
{
    s_connect_abort_gen.fetch_add(1u, std::memory_order_relaxed);

    WifiLockGuard lock;
    if (!lock.locked())
    {
        return false;
    }
    s_sta_active = false;
    s_ap_active = false;
    const bool ok = refreshMode();
    nrlMdnsStop();
    ESP_LOGI(TAG, "Wi-Fi radio stopped (STA+AP down, RAM freed)");
    return ok;
}

size_t nrlWifiApGetStationCount()
{
    if (!s_ap_active || !s_started)
    {
        return 0u;
    }
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK)
    {
        return 0u;
    }
    return static_cast<size_t>(list.num);
}

bool nrlWifiScanStartBlocking(uint32_t timeout_ms)
{
    const uint32_t t_entry = nowMs();
    const uint32_t lock_wait_ms = (timeout_ms > 0u) ? timeout_ms : 1000u;
    WifiLockGuard lock(pdMS_TO_TICKS(lock_wait_ms));
    if (!lock.locked())
    {
        ESP_LOGW(TAG, "scan skipped: WiFi busy");
        return false;
    }
    if (!nrlWifiInit())
    {
        return false;
    }

    const bool prev_sta_active = s_sta_active;
    if (!s_started || !s_sta_active)
    {
        // Scanning requires STA capability (WIFI_MODE_STA or WIFI_MODE_APSTA).
        s_sta_active = true;
        if (!refreshMode())
        {
            s_sta_active = prev_sta_active;
            return false;
        }
    }

    // Drain any stale completion signal so we wait for THIS scan.
    if (s_scan_done != nullptr)
    {
        xSemaphoreTake(s_scan_done, 0);
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 80;
    scan_cfg.scan_time.active.max = 120;

    bool scan_ok = false;
    const esp_err_t err = esp_wifi_scan_start(&scan_cfg, /*block=*/false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
    }
    else
    {
        const uint32_t elapsed = nowMs() - t_entry;
        const uint32_t wait_ms = (timeout_ms > elapsed) ? (timeout_ms - elapsed) : 500u;
        if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
        {
            (void)esp_wifi_scan_stop();
            ESP_LOGW(TAG, "scan timeout after %lums", static_cast<unsigned long>(timeout_ms));
        }
        else
        {
            uint16_t ap_count = 0;
            if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK)
            {
                ap_count = 0;
            }
            if (ap_count == 0u)
            {
                if (s_scan_mutex != nullptr)
                {
                    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                    s_scan_count = 0;
                    xSemaphoreGive(s_scan_mutex);
                }
                ESP_LOGI(TAG, "scan done: 0 APs");
                scan_ok = true;
            }
            else
            {
                // Cap allocation size to avoid large contiguous internal-DRAM
                // allocations in dense RF environments; IDF sorts by RSSI.
                uint16_t got = (ap_count > kScanFetchMax) ? kScanFetchMax : ap_count;
                wifi_ap_record_t *records = static_cast<wifi_ap_record_t *>(
                    malloc(sizeof(wifi_ap_record_t) * got));
                if (records == nullptr)
                {
                    (void)esp_wifi_clear_ap_list();
                    ESP_LOGW(TAG, "scan OOM allocating %u records", static_cast<unsigned>(got));
                }
                else
                {
                    const esp_err_t rec_err = esp_wifi_scan_get_ap_records(&got, records);
                    if (rec_err != ESP_OK)
                    {
                        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s",
                                 esp_err_to_name(rec_err));
                        (void)esp_wifi_clear_ap_list();
                    }
                    else
                    {
                        if (s_scan_mutex != nullptr)
                        {
                            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                        }
                        s_scan_count = 0;
                        for (uint16_t i = 0; i < got && s_scan_count < kScanCacheMax; ++i)
                        {
                            if (records[i].ssid[0] == '\0')
                            {
                                continue;
                            }
                            // Dedupe by SSID (a single SSID can appear on multiple BSSIDs).
                            bool dup = false;
                            for (size_t j = 0; j < s_scan_count; ++j)
                            {
                                if (strncmp(s_scan_cache[j].ssid,
                                            reinterpret_cast<const char *>(records[i].ssid),
                                            sizeof(s_scan_cache[j].ssid)) == 0)
                                {
                                    dup = true;
                                    break;
                                }
                            }
                            if (dup)
                            {
                                continue;
                            }
                            snprintf(s_scan_cache[s_scan_count].ssid,
                                     sizeof(s_scan_cache[s_scan_count].ssid),
                                     "%s", reinterpret_cast<const char *>(records[i].ssid));
                            s_scan_cache[s_scan_count].rssi = records[i].rssi;
                            s_scan_cache[s_scan_count].secured = (records[i].authmode != WIFI_AUTH_OPEN);
                            ++s_scan_count;
                        }
                        const size_t cached = s_scan_count;
                        if (s_scan_mutex != nullptr)
                        {
                            xSemaphoreGive(s_scan_mutex);
                        }
                        ESP_LOGI(TAG, "scan done: %u unique SSIDs cached (total=%u)",
                                 static_cast<unsigned>(cached), static_cast<unsigned>(ap_count));
                        scan_ok = true;
                    }
                    free(records);
                }
            }
        }
    }

    // Restore STA-inactive state if we only temporarily enabled STA for scanning
    // (e.g., AP-only mode or pre-scan before AP start).
    if (!prev_sta_active)
    {
        s_sta_active = false;
        refreshMode();
    }

    return scan_ok;
}

size_t nrlWifiScanGetCache(NrlWifiScanResult *out, size_t max_count)
{
    if (out == nullptr || max_count == 0u)
    {
        return 0u;
    }
    ensureSyncObjects();
    if (s_scan_mutex != nullptr)
    {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    const size_t n = (s_scan_count < max_count) ? s_scan_count : max_count;
    for (size_t i = 0; i < n; ++i)
    {
        out[i] = s_scan_cache[i];
    }
    if (s_scan_mutex != nullptr)
    {
        xSemaphoreGive(s_scan_mutex);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Reachability probe — lwIP socket version of the original WiFiClient probe.
// ---------------------------------------------------------------------------

namespace
{

    // Non-blocking TCP connect with timeout. Returns true if the 3-way handshake
    // completed within timeout_ms.
    static bool tcpConnectWithTimeout(uint32_t ip_net, uint16_t port,
                                      uint32_t timeout_ms, int *out_fd)
    {
        if (out_fd == nullptr)
        {
            return false;
        }
        *out_fd = -1;

        const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0)
        {
            return false;
        }
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            close(fd);
            return false;
        }

        struct sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        dest.sin_addr.s_addr = ip_net;
        const int rc = connect(fd, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
        if (rc == 0)
        {
            *out_fd = fd;
            return true;
        }
        if (errno != EINPROGRESS)
        {
            close(fd);
            return false;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = {};
        tv.tv_sec = timeout_ms / 1000u;
        tv.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
        const int sel = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (sel <= 0)
        {
            close(fd);
            return false;
        }

        int sockerr = 0;
        socklen_t errlen = sizeof(sockerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) < 0 || sockerr != 0)
        {
            close(fd);
            return false;
        }
        *out_fd = fd;
        return true;
    }

    static bool tcpReachableRtt(uint32_t ip_net, uint16_t port,
                                uint32_t timeout_ms, uint32_t &out_ms)
    {
        const uint32_t t0 = nowMs();
        int fd = -1;
        const bool ok = tcpConnectWithTimeout(ip_net, port, timeout_ms, &fd);
        out_ms = nowMs() - t0;
        if (ok && fd >= 0)
        {
            close(fd);
        }
        return ok;
    }

    static bool httpStatusLineRtt(const char *host, uint16_t port, const char *path,
                                  char *out_status, size_t out_status_size,
                                  uint32_t timeout_ms, uint32_t &out_ms)
    {
        out_ms = 0;
        if (out_status != nullptr && out_status_size > 0)
        {
            out_status[0] = '\0';
        }

        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr)
        {
            if (res != nullptr)
                freeaddrinfo(res);
            return false;
        }
        const uint32_t ip_net =
            reinterpret_cast<const struct sockaddr_in *>(res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);

        const uint32_t t0 = nowMs();
        int fd = -1;
        if (!tcpConnectWithTimeout(ip_net, port, timeout_ms, &fd))
        {
            return false;
        }

        char req[256];
        const int req_len = snprintf(req, sizeof(req),
                                     "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                                     path, host);
        if (req_len <= 0 || send(fd, req, static_cast<size_t>(req_len), 0) != req_len)
        {
            close(fd);
            return false;
        }

        char buf[160];
        size_t buf_used = 0;
        while (true)
        {
            const uint32_t elapsed = nowMs() - t0;
            if (elapsed >= timeout_ms)
            {
                break;
            }
            const uint32_t remaining = timeout_ms - elapsed;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv = {};
            tv.tv_sec = remaining / 1000u;
            tv.tv_usec = static_cast<long>((remaining % 1000u) * 1000u);
            const int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0)
            {
                break;
            }
            const ssize_t got = recv(fd, buf + buf_used,
                                     sizeof(buf) - 1u - buf_used, 0);
            if (got <= 0)
            {
                break;
            }
            buf_used += static_cast<size_t>(got);
            buf[buf_used] = '\0';
            char *nl = strchr(buf, '\n');
            if (nl != nullptr)
            {
                *nl = '\0';
                if (nl > buf && nl[-1] == '\r')
                {
                    nl[-1] = '\0';
                }
                if (out_status != nullptr && out_status_size > 0)
                {
                    snprintf(out_status, out_status_size, "%.*s",
                             static_cast<int>(out_status_size - 1), buf);
                }
                out_ms = nowMs() - t0;
                close(fd);
                return true;
            }
            if (buf_used >= sizeof(buf) - 1u)
            {
                break;
            }
        }
        close(fd);
        return false;
    }

} // namespace

NetProbeResult internetProbeCN(uint32_t timeout_ms)
{
    NetProbeResult r;
    if (!nrlNetworkConnected())
    {
        return r;
    }

    const uint32_t t_start = nowMs();

    // Step 1: direct TCP to 223.5.5.5:53 (Ali DNS) — DNS-independent.
    {
        struct in_addr ali = {};
        inet_aton("223.5.5.5", &ali);
        uint32_t ms = 0;
        const uint32_t step1_ms = (timeout_ms >= 2000u) ? (timeout_ms / 2u) : timeout_ms;
        if (tcpReachableRtt(ali.s_addr, 53, step1_ms, ms))
        {
            r.reachable = true;
            r.rtt_ms = ms;
            r.method = "TCP 223.5.5.5:53";
            return r;
        }
    }

    // Step 2: HTTP GET www.baidu.com — accept 2xx/3xx as reachable.
    const uint32_t elapsed = nowMs() - t_start;
    if (elapsed < timeout_ms)
    {
        char status[80] = {};
        uint32_t ms = 0;
        if (httpStatusLineRtt("www.baidu.com", 80, "/", status, sizeof(status),
                              timeout_ms - elapsed, ms))
        {
            if (strncmp(status, "HTTP/1.1 200", 12) == 0 ||
                strncmp(status, "HTTP/1.0 200", 12) == 0 ||
                strncmp(status, "HTTP/1.1 30", 11) == 0 ||
                strncmp(status, "HTTP/1.0 30", 11) == 0)
            {
                r.reachable = true;
                r.rtt_ms = ms;
                r.method = "HTTP baidu 2xx/3xx";
                return r;
            }
        }
    }

    return r;
}
