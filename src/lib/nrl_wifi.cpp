#include "nrl_wifi.h"

#include "nrl_bt_hfp.h"
#include "nrl_net_compat.h"

#include "../app/driver/external_radio.h"

#include <esp_efuse.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "WiFi";

namespace
{

    constexpr size_t kScanCacheMax = 24u;

    bool s_inited = false;
    bool s_started = false;
    esp_netif_t *s_sta_netif = nullptr;
    esp_netif_t *s_ap_netif = nullptr;

    bool s_sta_active = false; // STA mode requested
    bool s_ap_active = false;  // AP mode requested

    volatile bool s_sta_associated = false;
    volatile bool s_sta_has_ip = false;
    volatile uint8_t s_last_disconnect_reason = 0u;

    NrlWifiScanResult s_scan_cache[kScanCacheMax];
    size_t s_scan_count = 0u;
    SemaphoreHandle_t s_scan_done = nullptr;
    SemaphoreHandle_t s_scan_mutex = nullptr;
    volatile bool s_scan_in_progress = false;

    static inline uint32_t nowMs()
    {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    }

    static const char *disconnectReasonText(uint8_t reason)
    {
        switch (reason)
        {
        case WIFI_REASON_AUTH_EXPIRE:
            return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE:
            return "AUTH_LEAVE";
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
                int rssi = 0;
                (void)esp_wifi_sta_get_rssi(&rssi);
                // Heap snapshot pins "driver starved of internal RAM"
                // vs RF/software causes when the link drops mid-traffic.
                ESP_LOGW(TAG, "evt STA_DISCONNECTED reason=%u (%s) rssi=%d dram free=%u largest=%u min=%u",
                         static_cast<unsigned>(e->reason),
                         disconnectReasonText(e->reason), rssi,
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
                if (s_scan_done != nullptr)
                {
                    BaseType_t hp_woken = pdFALSE;
                    xSemaphoreGiveFromISR(s_scan_done, &hp_woken);
                    if (hp_woken == pdTRUE)
                    {
                        portYIELD_FROM_ISR();
                    }
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
        esp_wifi_get_mode(&current);
        if (target == current)
        {
            return true;
        }

        if (target == WIFI_MODE_NULL)
        {
            if (s_started)
            {
                esp_wifi_stop();
                s_started = false;
            }
            return true;
        }

        const esp_err_t err = esp_wifi_set_mode(target);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_set_mode(%d) failed: %d", static_cast<int>(target), err);
            return false;
        }
        if (!s_started)
        {
            const esp_err_t err2 = esp_wifi_start();
            if (err2 != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_start failed: %d", err2);
                return false;
            }
            s_started = true;
        }
        // Disable WiFi modem sleep. The IDF default (WIFI_PS_MIN_MODEM) lets the
        // radio sleep between DTIM intervals, which makes the MAC batch UDP voice
        // packets into 30-70 ms bursts on both TX and RX -- audible as jitter on
        // the 20 ms G.711 stream. Voice latency wins over the modest power saving.
        //
        // EXCEPTION: when the Bluetooth headset (HFP) is enabled, WIFI_PS_NONE
        // makes WiFi hog the single radio and starves the BT controller's coex
        // scheduler, dropping the SCO link on a 16 s supervision timeout. BT then
        // needs WiFi to yield slots (MIN_MODEM). This is a hard single-radio
        // tradeoff: with BT on, WiFi voice may gain some jitter.
        // MAX_MODEM: WiFi sleeps between beacons so the radio is free for the
        // isochronous SCO slots most of the time (MIN_MODEM still let WiFi grab
        // the radio mid-SCO-slot -> warble). Downlink UDP batches at the AP and
        // is absorbed by the 100 ms playback buffer.
        const wifi_ps_type_t ps = NRL_BtHfp_IsEnabled() ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE;
        const esp_err_t ps_err = esp_wifi_set_ps(ps);
        if (ps_err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_wifi_set_ps failed: %d", ps_err);
        }

        // ⚠️ 必须在 esp_wifi_start() 成功调用之后执行
        const esp_err_t ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to set HT20: %s", esp_err_to_name(ret));
        }

        return true;
    }

    static void makeHostname(char *buf, size_t buf_size)
    {
        if (buf == nullptr || buf_size == 0u)
        {
            return;
        }
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        const uint32_t tail = (static_cast<uint32_t>(mac[3]) << 16) |
                              (static_cast<uint32_t>(mac[4]) << 8) |
                              mac[5];

        const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
        if (config != nullptr && config->callsign[0] != '\0')
        {
            // Lowercase + alnum-only token from the callsign, joined with '-'.
            char call_token[12] = {};
            size_t pos = 0;
            bool last_was_dash = false;
            for (size_t i = 0; config->callsign[i] != '\0' && pos < sizeof(call_token) - 1; ++i)
            {
                const unsigned char ch = static_cast<unsigned char>(config->callsign[i]);
                if (isalnum(ch))
                {
                    call_token[pos++] = static_cast<char>(tolower(ch));
                    last_was_dash = false;
                }
                else if (!last_was_dash && pos > 0)
                {
                    call_token[pos++] = '-';
                    last_was_dash = true;
                }
            }
            while (pos > 0 && call_token[pos - 1] == '-')
            {
                call_token[--pos] = '\0';
            }
            if (pos > 0)
            {
                snprintf(buf, buf_size, "nrl-%s-%u-%06lX",
                         call_token,
                         static_cast<unsigned>(config->callsign_ssid),
                         static_cast<unsigned long>(tail));
                return;
            }
        }
        snprintf(buf, buf_size, "esp32s3-%06lX", static_cast<unsigned long>(tail));
    }

} // namespace

bool nrlWifiInit()
{
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

    if (s_scan_done == nullptr)
    {
        s_scan_done = xSemaphoreCreateBinary();
    }
    if (s_scan_mutex == nullptr)
    {
        s_scan_mutex = xSemaphoreCreateMutex();
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifiEventHandler, nullptr);

    s_inited = true;
    ESP_LOGI(TAG, "WiFi stack initialized");
    return true;
}

bool wifiIsConnected()
{
    return s_sta_has_ip;
}

WifiConnResult wifiEnsureConnected(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (ssid == nullptr || *ssid == '\0')
    {
        ESP_LOGE(TAG, "connect failed: empty SSID");
        return WIFI_CONN_FAILED;
    }
    if (!nrlWifiInit())
    {
        return WIFI_CONN_FAILED;
    }

    ESP_LOGI(TAG, "connecting: SSID=\"%s\" passLen=%u timeout=%lums",
             ssid,
             static_cast<unsigned>(pass != nullptr ? strlen(pass) : 0),
             static_cast<unsigned long>(timeout_ms));

    // Already on the target SSID with an IP?
    if (s_sta_has_ip)
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
        ESP_LOGI(TAG, "connected to a different SSID, will disconnect first");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Activate STA mode (preserve AP if already up).
    s_sta_active = true;
    if (!refreshMode())
    {
        return WIFI_CONN_FAILED;
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // let the STA interface settle

    char hostname[40] = {};
    makeHostname(hostname, sizeof(hostname));
    esp_netif_set_hostname(s_sta_netif, hostname);

    // Static IP or DHCP.
    const ExternalRadioConfig *net_config = EXTERNAL_RADIO_GetConfig();
    if (net_config != nullptr && !net_config->wifi_dhcp_enabled &&
        net_config->wifi_ip != 0u && net_config->wifi_netmask != 0u && net_config->wifi_gateway != 0u)
    {
        esp_netif_dhcpc_stop(s_sta_netif);
        esp_netif_ip_info_t info = {};
        info.ip.addr = net_config->wifi_ip;
        info.netmask.addr = net_config->wifi_netmask;
        info.gw.addr = net_config->wifi_gateway;
        esp_netif_set_ip_info(s_sta_netif, &info);
        esp_netif_dns_info_t dns = {};
        dns.ip.u_addr.ip4.addr = (net_config->wifi_dns != 0u) ? net_config->wifi_dns : net_config->wifi_gateway;
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
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
        esp_netif_dhcpc_start(s_sta_netif);
    }

    // Build STA config.
    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
    if (pass != nullptr)
    {
        strncpy(reinterpret_cast<char *>(cfg.sta.password), pass, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = (pass != nullptr && *pass != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    s_sta_associated = false;
    s_sta_has_ip = false;
    s_last_disconnect_reason = 0;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed");
        return WIFI_CONN_FAILED;
    }
    esp_wifi_connect();

    const uint32_t t0 = nowMs();
    uint32_t last_log = 0u;
    uint32_t last_reconnect = 0u;
    while (!s_sta_has_ip && (nowMs() - t0) < timeout_ms)
    {
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
        // If we haven't even associated after 5s, kick connect again. Don't
        // retry if we're past L2 association — that means DHCP is in progress.
        if (!s_sta_associated && elapsed >= 5000u && (elapsed - last_reconnect) >= 5000u)
        {
            ESP_LOGI(TAG, "still not associating, retrying connect");
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_wifi_connect();
            last_reconnect = elapsed;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (s_sta_has_ip)
    {
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
    if (!nrlWifiInit())
    {
        return false;
    }

    s_ap_active = true;
    if (!refreshMode())
    {
        return false;
    }

    // DHCP server must be stopped before changing the IP info.
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t info = {};
    info.ip.addr = ap_ip;
    info.gw.addr = gw;
    info.netmask.addr = mask;
    esp_netif_set_ip_info(s_ap_netif, &info);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_config_t cfg = {};
    const size_t ssid_len = strnlen(ssid, sizeof(cfg.ap.ssid));
    memcpy(cfg.ap.ssid, ssid, ssid_len);
    cfg.ap.ssid_len = static_cast<uint8_t>(ssid_len);
    cfg.ap.channel = channel;
    cfg.ap.max_connection = (max_conn == 0u) ? 4u : max_conn;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.ssid_hidden = 0;
    cfg.ap.pmf_cfg.required = false;

    if (esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed");
        return false;
    }

    char ip_buf[16] = {};
    nrlIpToString(ap_ip, ip_buf, sizeof(ip_buf));
    ESP_LOGI(TAG, "AP ready: ssid=%s open ip=%s channel=%u max=%u",
             ssid, ip_buf, static_cast<unsigned>(channel), static_cast<unsigned>(cfg.ap.max_connection));
    return true;
}

bool nrlWifiApStop()
{
    if (!s_ap_active)
    {
        return true;
    }
    s_ap_active = false;
    refreshMode();
    ESP_LOGI(TAG, "AP stopped");
    return true;
}

// Master radio stop: deactivate STA + AP and drop to WIFI_MODE_NULL, which calls
// esp_wifi_stop() -- freeing BOTH the shared 2.4 GHz radio AND WiFi's dynamic RAM
// so Bluetooth A2DP can register and stream. wifiEnsureConnected() brings it back
// (esp_wifi_start + reconnect) when the Wi-Fi master switch is turned on again.
bool nrlWifiStopRadio()
{
    s_sta_active = false;
    s_ap_active = false;
    const bool ok = refreshMode();
    ESP_LOGI(TAG, "Wi-Fi radio stopped (STA+AP down, RAM freed)");
    return ok;
}

size_t nrlWifiApGetStationCount()
{
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK)
    {
        return 0u;
    }
    return static_cast<size_t>(list.num);
}

bool nrlWifiScanStartBlocking(uint32_t timeout_ms)
{
    if (!nrlWifiInit())
    {
        return false;
    }
    if (!s_started)
    {
        // Need at least STA capability to scan.
        s_sta_active = true;
        if (!refreshMode())
        {
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

    s_scan_in_progress = true;
    const esp_err_t err = esp_wifi_scan_start(&scan_cfg, /*block=*/false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %d", err);
        s_scan_in_progress = false;
        return false;
    }

    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        esp_wifi_scan_stop();
        s_scan_in_progress = false;
        ESP_LOGW(TAG, "scan timeout after %lums", static_cast<unsigned long>(timeout_ms));
        return false;
    }
    s_scan_in_progress = false;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0u)
    {
        if (s_scan_mutex != nullptr)
        {
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            s_scan_count = 0;
            xSemaphoreGive(s_scan_mutex);
        }
        ESP_LOGI(TAG, "scan done: 0 APs");
        return true;
    }

    wifi_ap_record_t *records = static_cast<wifi_ap_record_t *>(
        malloc(sizeof(wifi_ap_record_t) * ap_count));
    if (records == nullptr)
    {
        esp_wifi_clear_ap_list();
        return false;
    }
    uint16_t got = ap_count;
    esp_wifi_scan_get_ap_records(&got, records);

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
        // Dedupe by SSID (a single SSID can appear on multiple bands/BSSIDs).
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
        snprintf(s_scan_cache[s_scan_count].ssid, sizeof(s_scan_cache[s_scan_count].ssid),
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
    free(records);
    esp_wifi_clear_ap_list();

    ESP_LOGI(TAG, "scan done: %u unique SSIDs cached", static_cast<unsigned>(cached));
    return true;
}

size_t nrlWifiScanGetCache(NrlWifiScanResult *out, size_t max_count)
{
    if (out == nullptr || max_count == 0u)
    {
        return 0u;
    }
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
        while ((nowMs() - t0) < timeout_ms)
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            const uint32_t remaining = timeout_ms - (nowMs() - t0);
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

    // Step 1: direct TCP to 223.5.5.5:53 (Ali DNS) — DNS-independent.
    {
        struct in_addr ali = {};
        inet_aton("223.5.5.5", &ali);
        uint32_t ms = 0;
        if (tcpReachableRtt(ali.s_addr, 53, timeout_ms, ms))
        {
            r.reachable = true;
            r.rtt_ms = ms;
            r.method = "TCP 223.5.5.5:53";
            return r;
        }
    }

    // Step 2: HTTP GET www.baidu.com — accept 2xx/3xx as reachable.
    {
        char status[80] = {};
        uint32_t ms = 0;
        if (httpStatusLineRtt("www.baidu.com", 80, "/", status, sizeof(status), timeout_ms, ms))
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
