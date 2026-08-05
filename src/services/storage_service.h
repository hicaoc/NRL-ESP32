#ifndef SRC_SERVICES_STORAGE_SERVICE_H
#define SRC_SERVICES_STORAGE_SERVICE_H

// Storage service (docs/architecture.md 3.5): mounts removable media into
// VFS/FatFS so the media player and playlists access files by path without
// caring about the medium. Backends:
//   - TF card via SDMMC (S31-Korvo only, vendored BSP) -> /sdcard
//   - USB flash drive via USB Host MSC -> /usb        (planned, Phase 1)
// On boards without the hardware every call is a cheap no-op returning false.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount whatever storage backends the board has. Failure to mount (e.g. no
// card inserted) is logged but not fatal; returns true if at least one
// backend mounted.
bool STORAGE_Init(void);

// TF card state.
bool STORAGE_SdMounted(void);

// Mount point of the TF card ("/sdcard"), or NULL when not mounted.
const char *STORAGE_SdMountPoint(void);

// Retry mounting the TF card (AT+SDMOUNT): lets a card inserted after boot
// come online without a reboot. Returns the mounted state.
bool STORAGE_SdMountRetry(void);

// Erase + format the TF card as FAT in-device, then mount it. DESTRUCTIVE
// (all card data is lost); only reachable via the explicit AT+SDFORMAT=YES.
// Rescues cards shipped exFAT/NTFS, which this firmware's FatFs cannot read,
// and sidesteps Windows' 32 GB FAT32 formatting limit.
bool STORAGE_SdFormat(void);

// USB flash drive state (USB-OTG Host MSC, hot-pluggable). On insertion the
// drive is mounted at /usb and the playlist rescans; on removal any track
// playing from it is stopped first.
bool STORAGE_UsbMounted(void);

// Mount point of the USB drive ("/usb"), or NULL when not mounted.
const char *STORAGE_UsbMountPoint(void);

// SMB network share backend (docs/architecture.md 3.5): plays audio from a
// NAS / Windows shared folder mounted at /smb. Configuration persists in
// NVS. The mount is lazy: nothing touches the network at boot (a mount+scan
// storm there starves internal RAM exactly when the boot OTA check runs);
// the supervisor task starts on the first playlist browse/play that could
// reach the share, waits for WiFi, mounts, and retries every 30 s while the
// share is unreachable. Point the share (or its path) directly at the music
// folder -- the playlist scans the share root, not a /music subdirectory.
// Start the mount supervisor if a share is configured; idempotent.
void STORAGE_SmbAutoStart(void);
// Configure + persist + (re)mount. Empty user means guest.
bool STORAGE_SmbConfigure(const char *server, const char *share,
                          const char *user, const char *password);

// Unmount and erase the persisted configuration.
void STORAGE_SmbClear(void);

bool STORAGE_SmbMounted(void);

// Mount point ("/smb"), or NULL when not mounted.
const char *STORAGE_SmbMountPoint(void);

// Human-readable status for the AT console, e.g. "//nas/music (mounted)".
void STORAGE_SmbDescribe(char *out, size_t out_size);

// Copy the current SMB configuration into the out buffers (any pointer may
// be NULL to skip that field). Returns true when a server+share are
// configured. Web/UI config forms use this to prefill their fields.
bool STORAGE_SmbGetConfig(char *server, size_t server_size,
                          char *share, size_t share_size,
                          char *user, size_t user_size,
                          char *password, size_t password_size);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_STORAGE_SERVICE_H
