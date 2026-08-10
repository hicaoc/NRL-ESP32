#ifndef SRC_SERVICES_SERVER_LIST_STORE_H
#define SRC_SERVICES_SERVER_LIST_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVER_LIST_NRL = 1,
    SERVER_LIST_FMO = 2,
    SERVER_FILE_FMO_USER_CERT = 3,
    SERVER_FILE_FMO_INTERMEDIATE_CERT = 4,
    SERVER_FILE_FMO_DEVICE_KEY = 5,
} ServerListKind;

typedef struct {
    char name[96];
    char host[96];
    uint16_t port;
    uint32_t online;
    uint32_t total;
} NrlServerInfo;

// Mounts the final LittleFS partition (1152 KiB on S3, 896 KiB on S31,
// 640 KiB on the legacy 4 MB layout).
// A blank partition is formatted automatically on its first boot.
bool SERVER_LIST_STORE_Init(void);
bool SERVER_LIST_STORE_Ready(void);

// Read allocates the returned payload exclusively in PSRAM. The caller owns it
// and must call free(); allocation failure never falls back to internal SRAM.
// Lists and FMO identity files are protected by a versioned header and CRC; a
// valid backup is used if the newest copy was interrupted during replacement.
bool SERVER_LIST_STORE_Read(ServerListKind kind, uint8_t **payload,
                            size_t *payload_size);
bool SERVER_LIST_STORE_Write(ServerListKind kind, const void *payload,
                             size_t payload_size);

// Looks up the friendly name of the configured NRL endpoint in the cached
// platform-server JSON. The JSON payload is read into PSRAM and scanned
// without constructing a cJSON tree in scarce internal SRAM.
bool SERVER_LIST_STORE_FindNrlServerName(const char *host, uint16_t port,
                                         char *name, size_t name_size);

// Loads the cached NRL server records into a PSRAM-owned array. The caller
// owns the result and releases it with free().
bool SERVER_LIST_STORE_LoadNrlServers(NrlServerInfo **servers, size_t *count);

// Downloads https://<host>/platform/list, validates the data.items array and
// atomically replaces the cached NRL directory. The response buffer is held
// in PSRAM. This is used as a same-origin proxy by the Web UI because the
// platform endpoint does not advertise browser CORS access.
bool SERVER_LIST_STORE_RefreshNrlHttps(const char *host, uint16_t port,
                                      size_t *count);

// Lightweight validation shared by the HTTPS refresher and legacy Web POST.
// Returns the number of usable records without building a cJSON tree.
size_t SERVER_LIST_STORE_ValidateNrlJson(const char *json, size_t size);

// Increments after a successful runtime replacement. Consumers can cache a
// lookup and refresh only when the underlying list changes.
uint32_t SERVER_LIST_STORE_Generation(ServerListKind kind);

#ifdef __cplusplus
}
#endif

#endif
