#include "nrl_ethernet.h"

#include "driver/board_pins.h"

#if defined(NRL_HAS_ETHERNET) && NRL_HAS_ETHERNET

#include <esp_check.h>
#include <esp_eth.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>

static const char *TAG = "Ethernet";

namespace {

esp_eth_handle_t s_eth_handle = nullptr;
esp_netif_t *s_eth_netif = nullptr;
esp_eth_netif_glue_handle_t s_eth_glue = nullptr;
volatile bool s_started = false;
volatile bool s_link_up = false;
volatile bool s_has_ip = false;

static esp_err_t configureYt8531(esp_eth_handle_t handle)
{
    // YT8531 disables auto-negotiation after its hardware reset. This behavior
    // is handled explicitly by ESP-IDF's S31 Ethernet example as well.
    bool auto_negotiation = true;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_S_AUTONEGO, &auto_negotiation),
                        TAG, "enable PHY auto-negotiation failed");

    esp_eth_phy_reg_rw_data_t reg = {};
    uint32_t value = 0;
    reg.reg_value_p = &value;

    // RX clock delay: EXT_CHIP_CONFIG (0xA001), bit 8, approximately 2 ns.
    value = 0xA001;
    reg.reg_addr = 0x1e;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &reg),
                        TAG, "select YT8531 chip-config register failed");
    reg.reg_addr = 0x1f;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &reg),
                        TAG, "read YT8531 chip-config register failed");
    value |= (1u << 8);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &reg),
                        TAG, "write YT8531 RX delay failed");

    // TX delay: EXT_RGMII_CONFIG1 (0xA003), 13 * 150 ps for both speeds.
    value = 0xA003;
    reg.reg_addr = 0x1e;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &reg),
                        TAG, "select YT8531 RGMII config register failed");
    reg.reg_addr = 0x1f;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &reg),
                        TAG, "read YT8531 RGMII config register failed");
    value = (value & ~0xffu) | (13u << 4) | 13u;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &reg),
                        TAG, "write YT8531 TX delay failed");

    ESP_LOGI(TAG, "YT8531 auto-negotiation and RGMII delays configured");
    return ESP_OK;
}

static void ethernetEvent(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_START:
        s_started = true;
        ESP_LOGI(TAG, "interface started");
        break;
    case ETHERNET_EVENT_CONNECTED: {
        s_link_up = true;
        esp_eth_handle_t handle = *static_cast<esp_eth_handle_t *>(event_data);
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        (void)esp_eth_ioctl(handle, ETH_CMD_G_SPEED, &speed);
        (void)esp_eth_ioctl(handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        const int mbps = speed == ETH_SPEED_10M ? 10 : speed == ETH_SPEED_100M ? 100 : 1000;
        ESP_LOGI(TAG, "link up: %d Mbps %s duplex", mbps,
                 duplex == ETH_DUPLEX_HALF ? "half" : "full");
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        s_has_ip = false;
        ESP_LOGW(TAG, "link down");
        if (esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            wifi_netif && esp_netif_is_netif_up(wifi_netif)) {
            esp_netif_set_default_netif(wifi_netif);
        }
        break;
    case ETHERNET_EVENT_STOP:
        s_started = false;
        s_link_up = false;
        s_has_ip = false;
        ESP_LOGI(TAG, "interface stopped");
        if (esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            wifi_netif && esp_netif_is_netif_up(wifi_netif)) {
            esp_netif_set_default_netif(wifi_netif);
        }
        break;
    default:
        break;
    }
}

static void ethernetGotIp(void *, esp_event_base_t, int32_t, void *event_data)
{
    const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    if (event == nullptr || event->esp_netif != s_eth_netif) {
        return;
    }
    s_has_ip = true;
    // ESP_NETIF_DEFAULT_ETH has a lower route priority than Wi-Fi STA. The
    // product policy is wired-first, so promote Ethernet whenever it has IP.
    if (s_eth_netif != nullptr) {
        (void)esp_netif_set_default_netif(s_eth_netif);
    }
    ESP_LOGI(TAG, "got IPv4: " IPSTR " gateway " IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
}

} // namespace

bool nrlEthernetInit()
{
    if (s_eth_handle != nullptr) {
        return true;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;
    phy_config.reset_gpio_num = NRL_PIN_ETH_PHY_RESET;

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.interface = EMAC_DATA_INTERFACE_RGMII;
    emac.smi_gpio.mdc_num = NRL_PIN_ETH_MDC;
    emac.smi_gpio.mdio_num = NRL_PIN_ETH_MDIO;
    emac.clock_config.rgmii.clock_rx_gpio = NRL_PIN_ETH_RX_CLK;
    emac.clock_config.rgmii.clock_tx_gpio = NRL_PIN_ETH_TX_CLK;
    emac.clock_config.rgmii.clock_phy_ref_gpio = -1; // PHY has its own 25 MHz crystal
    emac.emac_dataif_gpio.rgmii.tx_ctl_num = NRL_PIN_ETH_TX_CTL;
    emac.emac_dataif_gpio.rgmii.txd0_num = NRL_PIN_ETH_TXD0;
    emac.emac_dataif_gpio.rgmii.txd1_num = NRL_PIN_ETH_TXD1;
    emac.emac_dataif_gpio.rgmii.txd2_num = NRL_PIN_ETH_TXD2;
    emac.emac_dataif_gpio.rgmii.txd3_num = NRL_PIN_ETH_TXD3;
    emac.emac_dataif_gpio.rgmii.rx_ctl_num = NRL_PIN_ETH_RX_CTL;
    emac.emac_dataif_gpio.rgmii.rxd0_num = NRL_PIN_ETH_RXD0;
    emac.emac_dataif_gpio.rgmii.rxd1_num = NRL_PIN_ETH_RXD1;
    emac.emac_dataif_gpio.rgmii.rxd2_num = NRL_PIN_ETH_RXD2;
    emac.emac_dataif_gpio.rgmii.rxd3_num = NRL_PIN_ETH_RXD3;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    if (mac == nullptr) {
        ESP_LOGE(TAG, "create EMAC failed");
        return false;
    }
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == nullptr) {
        ESP_LOGE(TAG, "create generic PHY failed");
        mac->del(mac);
        return false;
    }

    esp_eth_config_t driver_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&driver_config, &s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        s_eth_handle = nullptr;
        return false;
    }

    err = configureYt8531(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "YT8531 setup failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    if (s_eth_netif == nullptr) {
        ESP_LOGE(TAG, "create Ethernet netif failed");
        return false;
    }
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == nullptr || esp_netif_attach(s_eth_netif, s_eth_glue) != ESP_OK) {
        ESP_LOGE(TAG, "attach Ethernet netif failed");
        return false;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, ethernetEvent, nullptr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ethernetGotIp, nullptr));

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool nrlEthernetIsStarted() { return s_started; }
bool nrlEthernetLinkUp() { return s_link_up; }
bool nrlEthernetHasIp() { return s_has_ip; }

uint32_t nrlEthernetIp()
{
    if (s_eth_netif == nullptr) {
        return 0;
    }
    esp_netif_ip_info_t info = {};
    return esp_netif_get_ip_info(s_eth_netif, &info) == ESP_OK ? info.ip.addr : 0;
}

#else

bool nrlEthernetInit() { return true; }
bool nrlEthernetIsStarted() { return false; }
bool nrlEthernetLinkUp() { return false; }
bool nrlEthernetHasIp() { return false; }
uint32_t nrlEthernetIp() { return 0; }

#endif
