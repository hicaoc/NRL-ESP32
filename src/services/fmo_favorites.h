#ifndef SRC_SERVICES_FMO_FAVORITES_H
#define SRC_SERVICES_FMO_FAVORITES_H

// FMO server favorites (name/callsign/uid/host/port plus the 32-byte server
// fingerprint the MQTT link requires), persisted in NVS. Edited from the web
// portal FMO page (/fmo/favorites); consumed there and by the LCD FMO server
// menu for one-press server switching.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/fmo_service.h" // FMO_SERVER_* size macros

#ifdef __cplusplus
extern "C" {
#endif

enum { FMO_FAV_MAX = 12 }; // list capacity

typedef struct {
    char name[FMO_SERVER_NAME_MAX];
    char host[FMO_SERVER_HOST_MAX];
    char callsign[FMO_SERVER_CALLSIGN_MAX];
    uint16_t port;
    uint32_t uid;
    uint8_t fingerprint[32]; // required: the link refuses fingerprint-less servers
} FmoFavorite;

// Load the list from NVS. Call once at startup (after nvs_flash_init).
void FMO_FAV_Init(void);

size_t FMO_FAV_Count(void);

// Copy entry `index` out. Returns false when out of range or out is NULL.
bool FMO_FAV_Get(size_t index, FmoFavorite *out);

// Insert or update (matched by non-zero uid, else host:port) and persist.
// Entries must satisfy the link's usability rules (host/port/uid/callsign).
bool FMO_FAV_Add(const FmoFavorite *entry);

bool FMO_FAV_Remove(size_t index);

// Index of the entry matching uid (when non-zero, else host:port), or -1.
int FMO_FAV_Find(uint32_t uid, const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_FAVORITES_H
