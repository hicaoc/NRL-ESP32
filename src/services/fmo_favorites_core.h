#ifndef SRC_SERVICES_FMO_FAVORITES_CORE_H
#define SRC_SERVICES_FMO_FAVORITES_CORE_H

// Pure list operations behind fmo_favorites.cpp: no NVS/FreeRTOS deps, so the
// host tests (tests/fmo_favorites_test.cpp) can exercise every rule directly.

#include "services/fmo_favorites.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FmoFavorite entries[FMO_FAV_MAX];
    size_t count;
} FmoFavoriteList;

// Insert or update in place (same uid when non-zero, else same host:port).
// Rejects unusable entries (empty host/callsign, port 0, uid 0) and a full
// list. Text fields are force-terminated on copy.
bool FMO_FAV_CORE_Add(FmoFavoriteList *list, const FmoFavorite *entry);

bool FMO_FAV_CORE_Remove(FmoFavoriteList *list, size_t index);

// Index of the entry matching uid (when non-zero, else host:port), or -1.
int FMO_FAV_CORE_Find(const FmoFavoriteList *list, uint32_t uid,
                      const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_FAVORITES_CORE_H
