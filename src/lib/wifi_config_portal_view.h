#ifndef SRC_LIB_WIFI_CONFIG_PORTAL_VIEW_H
#define SRC_LIB_WIFI_CONFIG_PORTAL_VIEW_H

#include "../app/driver/external_radio.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

struct WifiConfigPortalScanEntry {
    std::string ssid;
    int32_t rssi;
};

struct WifiConfigPortalPageState {
    const char *title;
    const char *headline;
    const char *headline_key;
    const char *intro;
    const char *intro_key;
    const char *form_action;
    bool network_active;
    bool device_active;
    bool audio_active;
    bool media_active;
    bool aprs_active;
    bool signaling_active;
    bool home_page;
    std::string footer;
};

std::string WifiConfigPortalView_BuildNetworkSection(const ExternalRadioConfig *config,
                                                     const WifiConfigPortalScanEntry *scan_entries,
                                                     size_t scan_count);
std::string WifiConfigPortalView_BuildDeviceSections(const ExternalRadioConfig *config);
std::string WifiConfigPortalView_BuildAudioSections(const ExternalRadioConfig *config);
// Media / voice-link page: S31 gets the full media stack; gezipai/bh4tdv get
// the PTT target, NRL codec and ESP-NOW intercom controls.
std::string WifiConfigPortalView_BuildMediaSections(void);
// APRS page: switches, APRS-IS/beacon settings and the stations-heard table.
std::string WifiConfigPortalView_BuildAprsSections(void);
// MDC1200/DTMF decode and voice-tail transmit routing.
std::string WifiConfigPortalView_BuildSignalingSections(void);
// Simple navigation home: one button per management page.
std::string WifiConfigPortalView_BuildHomeCards(void);
std::string WifiConfigPortalView_BuildConfigPage(const ExternalRadioConfig *config,
                                                 const WifiConfigPortalPageState &state,
                                                 const std::string &form_sections);
std::string WifiConfigPortalView_BuildUpdatePage(const char *headline,
                                                 const char *headline_key,
                                                 const char *intro,
                                                 const char *intro_key);

#endif
