#include "nrl_mdns.h"

#include "nrl_hostname.h"
#include "nrl_version.h"
#include "../app/driver/external_radio.h"

#include <esp_log.h>
#include <mdns.h>
#include <stdio.h>
#include <string.h>

namespace {

const char *TAG = "mDNS";
bool s_started = false;
bool s_services_added = false;
char s_hostname[40] = {};

bool addService(const char *instance, const char *service, mdns_txt_item_t *txt,
                const size_t txt_count)
{
    esp_err_t err = mdns_service_add(nullptr, service, "_tcp", 80,
                                     txt, txt_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add %s service failed: %s", service, esp_err_to_name(err));
        return false;
    }
    err = mdns_service_instance_name_set(service, "_tcp", instance);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "name %s service failed: %s", service, esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace

void nrlMdnsRefresh()
{
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    char hostname[40] = {};
    NrlMakeHostname(config != nullptr ? config->callsign : nullptr,
                    config != nullptr ? config->callsign_ssid : 0u,
                    hostname, sizeof(hostname));

    if (!s_started) {
        const esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "init failed: %s", esp_err_to_name(err));
            return;
        }
        s_started = true;
    }

    char ssid[4] = {};
    snprintf(ssid, sizeof(ssid), "%u",
             static_cast<unsigned>(config != nullptr ? config->callsign_ssid : 0u));
    mdns_txt_item_t txt[] = {
        {"call", config != nullptr ? config->callsign : "NOCALL"},
        {"ssid", ssid},
        {"version", NRL_FIRMWARE_VERSION},
        {"path", "/"},
    };
    char instance[64] = {};
    snprintf(instance, sizeof(instance), "%s %s", NRL_FIRMWARE_NAME, hostname);

    esp_err_t err = mdns_hostname_set(hostname);
    if (err == ESP_OK) err = mdns_instance_name_set(instance);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set device name failed: %s", esp_err_to_name(err));
        return;
    }

    if (!s_services_added) {
        s_services_added = addService(instance, "_http", txt, 4u);
        s_services_added = addService(instance, "_nrl", txt, 4u) && s_services_added;
    } else {
        (void)mdns_service_instance_name_set("_http", "_tcp", instance);
        (void)mdns_service_instance_name_set("_nrl", "_tcp", instance);
        (void)mdns_service_txt_set("_http", "_tcp", txt, 4u);
        (void)mdns_service_txt_set("_nrl", "_tcp", txt, 4u);
    }

    if (s_services_added && strcmp(s_hostname, hostname) != 0) {
        ESP_LOGI(TAG, "http://%s.local/ (%s)", hostname, instance);
        snprintf(s_hostname, sizeof(s_hostname), "%s", hostname);
    }
}

void nrlMdnsStop()
{
    if (!s_started) return;
    mdns_free();
    s_started = false;
    s_services_added = false;
    s_hostname[0] = '\0';
}
