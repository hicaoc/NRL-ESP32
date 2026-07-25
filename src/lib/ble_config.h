#ifndef SRC_LIB_BLE_CONFIG_H
#define SRC_LIB_BLE_CONFIG_H

#include <stdbool.h>

bool BLEConfig_Init(void);
void BLEConfig_Poll(void);
// True while the BLE provisioning host and GATT service are active (including
// while a provisioning client is connected and advertising is paused).
bool BLEConfig_IsReady(void);

// Report the result of the lightweight boot-time WiFi connection attempt to
// a provisioning client. No-op when no transactional BLE provisioning is in
// progress. On success the notification includes the acquired IPv4 address.
void BLEConfig_ReportWifiResult(bool connected);

// Tear the BLE stack fully down (advertising, host, BT controller) so the
// shared radio is freed for WiFi-only operation -- BT/WiFi coexistence
// otherwise time-slices the single antenna and batches outbound voice
// packets into ~100 ms bursts. BLEConfig_Poll() calls this automatically
// once the WiFi STA has been connected for a grace period, and re-inits BLE
// if the STA link later drops (so BLE provisioning stays available as a
// fallback when WiFi is down).
void BLEConfig_Stop(void);

// True if the BLE module currently owns the BT controller (Bluedroid S31 path).
// Used by nrl_bt_hfp to detect a controller conflict before stackUp().
bool BLEConfig_IsControllerUp(void);

#endif // SRC_LIB_BLE_CONFIG_H
