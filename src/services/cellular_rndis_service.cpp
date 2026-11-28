#include "cellular_rndis_service.h"

// USER CUSTOM BEGIN: optional USB RNDIS cellular network adaptation.

#include "../app/driver/board_pins.h"

#if defined(NRL_HAS_USB_RNDIS_4G) && NRL_HAS_USB_RNDIS_4G

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_eth.h>
#include <iot_eth_netif_glue.h>
#include <iot_usbh_cdc.h>
#include <iot_usbh_rndis.h>

namespace {

const char *TAG = "USB4G";

iot_eth_driver_t *s_rndis_driver = nullptr;
iot_eth_handle_t s_eth_handle = nullptr;
esp_netif_t *s_netif = nullptr;
iot_eth_netif_glue_handle_t s_glue = nullptr;
volatile bool s_started = false;
volatile bool s_link_up = false;
volatile bool s_has_ip = false;

static void restoreFallbackDefaultNetif()
{
    if (esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
        eth != nullptr && esp_netif_is_netif_up(eth)) {
        (void)esp_netif_set_default_netif(eth);
        return;
    }
    if (esp_netif_t *wifi = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        wifi != nullptr && esp_netif_is_netif_up(wifi)) {
        (void)esp_netif_set_default_netif(wifi);
    }
}

static void rndisEvent(void *, esp_event_base_t, int32_t event_id, void *)
{
    switch (event_id) {
    case IOT_ETH_EVENT_START:
        s_started = true;
        ESP_LOGI(TAG, "RNDIS driver started, waiting for ML307R");
        break;
    case IOT_ETH_EVENT_CONNECTED:
        s_link_up = true;
        ESP_LOGI(TAG, "ML307R RNDIS link up");
        break;
    case IOT_ETH_EVENT_DISCONNECTED:
        s_link_up = false;
        s_has_ip = false;
        ESP_LOGW(TAG, "ML307R RNDIS link down");
        restoreFallbackDefaultNetif();
        break;
    case IOT_ETH_EVENT_STOP:
        s_started = false;
        s_link_up = false;
        s_has_ip = false;
        ESP_LOGI(TAG, "RNDIS driver stopped");
        restoreFallbackDefaultNetif();
        break;
    default:
        break;
    }
}

static void rndisGotIp(void *, esp_event_base_t, int32_t, void *event_data)
{
    const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    if (event == nullptr || event->esp_netif != s_netif) {
        return;
    }
    s_has_ip = true;
    if (s_netif != nullptr) {
        (void)esp_netif_set_default_netif(s_netif);
    }
    ESP_LOGI(TAG, "ML307R got IPv4: " IPSTR " gateway " IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
}

static void powerOnMl307()
{
#if defined(NRL_PIN_ML307_POWER) && NRL_PIN_ML307_POWER >= 0
    gpio_reset_pin(static_cast<gpio_num_t>(NRL_PIN_ML307_POWER));
    gpio_set_direction(static_cast<gpio_num_t>(NRL_PIN_ML307_POWER), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_ML307_POWER), NRL_PIN_ML307_POWER_ON_LEVEL ? 1 : 0);
    ESP_LOGI(TAG, "ML307 power switch GPIO%d=%d",
             NRL_PIN_ML307_POWER, NRL_PIN_ML307_POWER_ON_LEVEL ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(1500));
#endif
}

} // namespace

bool CELLULAR_RNDIS_Init(void)
{
    if (s_eth_handle != nullptr) {
        return true;
    }

    powerOnMl307();

    usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_coreid = 0,
        .skip_init_usb_host_driver = false,
    };
    esp_err_t err = usbh_cdc_driver_install(&cdc_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB CDC host install failed: %s", esp_err_to_name(err));
        return false;
    }

    iot_usbh_rndis_config_t rndis_config = {
        .match_id_list = ESP_USB_DEVICE_MATCH_ID_ANY,
    };
    err = iot_eth_new_usb_rndis(&rndis_config, &s_rndis_driver);
    if (err != ESP_OK || s_rndis_driver == nullptr) {
        ESP_LOGE(TAG, "create USB RNDIS driver failed: %s", esp_err_to_name(err));
        return false;
    }

    iot_eth_config_t eth_config = {
        .driver = s_rndis_driver,
        .stack_input = nullptr,
        .stack_input_info = nullptr,
    };
    err = iot_eth_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK || s_eth_handle == nullptr) {
        ESP_LOGE(TAG, "install USB RNDIS ethernet failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_key = "USB4G_DEF";
    inherent.if_desc = "usb4g";
    inherent.route_prio = 60;
    esp_netif_config_t netif_config = {
        .base = &inherent,
        .driver = nullptr,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&netif_config);
    if (s_netif == nullptr) {
        ESP_LOGE(TAG, "create USB 4G netif failed");
        return false;
    }

    s_glue = iot_eth_new_netif_glue(s_eth_handle);
    if (s_glue == nullptr || esp_netif_attach(s_netif, s_glue) != ESP_OK) {
        ESP_LOGE(TAG, "attach USB 4G netif failed");
        return false;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, rndisEvent, nullptr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, rndisGotIp, nullptr));

    err = iot_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start USB RNDIS ethernet failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "USB RNDIS 4G ready; configure ML307R as RNDIS mode if needed");
    return true;
}

bool CELLULAR_RNDIS_IsStarted(void) { return s_started; }
bool CELLULAR_RNDIS_HasIp(void) { return s_has_ip; }

uint32_t CELLULAR_RNDIS_Ip(void)
{
    if (s_netif == nullptr) {
        return 0u;
    }
    esp_netif_ip_info_t info = {};
    return esp_netif_get_ip_info(s_netif, &info) == ESP_OK ? info.ip.addr : 0u;
}

#else

bool CELLULAR_RNDIS_Init(void) { return true; }
bool CELLULAR_RNDIS_IsStarted(void) { return false; }
bool CELLULAR_RNDIS_HasIp(void) { return false; }
uint32_t CELLULAR_RNDIS_Ip(void) { return 0u; }

#endif
// USER CUSTOM END: optional USB RNDIS cellular network adaptation.
