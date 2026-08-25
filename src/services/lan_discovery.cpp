#include "lan_discovery.h"

#include "../app/driver/board_pins.h"
#include "../app/driver/external_radio.h"
#include "../lib/nrl_version.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr uint16_t kDiscoveryPort = 60051u;
constexpr char kDiscoveryRequest[] = "NRL_DISCOVER/1";
constexpr size_t kMaxPacketsPerPoll = 4u;
const char *TAG = "LAN_DISC";
int s_socket = -1;

const char *boardType()
{
#if NRL_BOARD == NRL_BOARD_GEZIPAI_4G
    return "gezipai_4g";
#elif NRL_BOARD == NRL_BOARD_GEZIPAI
    return "gezipai";
#elif NRL_BOARD_IS_BI4UMD_FAMILY
    return "bi4umd";
#elif NRL_BOARD == NRL_BOARD_BH4TDV
    return "bh4tdv";
#elif NRL_BOARD == NRL_BOARD_S31_KORVO
    return "s31_korvo";
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    return "s31_function_coreboard";
#else
    return "unknown";
#endif
}

} // namespace

void LAN_DISCOVERY_Init(void)
{
    if (s_socket >= 0) return;

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0) {
        ESP_LOGE(TAG, "socket create failed: errno=%d", errno);
        return;
    }

    const int reuse = 1;
    setsockopt(s_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const int flags = fcntl(s_socket, F_GETFL, 0);
    if (flags >= 0) fcntl(s_socket, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kDiscoveryPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_socket, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0) {
        ESP_LOGE(TAG, "bind UDP/%u failed: errno=%d",
                 static_cast<unsigned>(kDiscoveryPort), errno);
        close(s_socket);
        s_socket = -1;
        return;
    }
    ESP_LOGI(TAG, "listening on UDP/%u", static_cast<unsigned>(kDiscoveryPort));
}

void LAN_DISCOVERY_Poll(void)
{
    if (s_socket < 0) {
        LAN_DISCOVERY_Init();
        if (s_socket < 0) return;
    }

    for (size_t packet = 0; packet < kMaxPacketsPerPoll; ++packet) {
        char request[64] = {};
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        const ssize_t got = recvfrom(s_socket, request, sizeof(request) - 1u, 0,
                                     reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (got < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recvfrom failed: errno=%d", errno);
            }
            break;
        }
        request[got] = '\0';
        if (strcmp(request, kDiscoveryRequest) != 0) continue;

        const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char response[320] = {};
        const int length = snprintf(
            response, sizeof(response),
            "{\"protocol\":\"nrl-discovery/1\",\"device_id\":\"%02X%02X%02X%02X%02X%02X\","
            "\"name\":\"%s\",\"callsign\":\"%s\",\"ssid\":%u,\"model\":\"%s\","
            "\"version\":\"%s\",\"http_port\":80,\"at_path\":\"/api/at\"}",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            NRL_FIRMWARE_NAME,
            config != nullptr ? config->callsign : "",
            static_cast<unsigned>(config != nullptr ? config->callsign_ssid : 0u),
            boardType(), NRL_FIRMWARE_VERSION);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(response)) continue;
        sendto(s_socket, response, static_cast<size_t>(length), 0,
               reinterpret_cast<const sockaddr *>(&peer), peer_len);
    }
}
