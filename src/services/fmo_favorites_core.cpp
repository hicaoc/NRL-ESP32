#include "services/fmo_favorites_core.h"

#include <string.h>

namespace {

// Mirror of fmo_service.cpp's serverUsable(): a favorite must carry
// everything the link needs to connect.
bool entryUsable(const FmoFavorite &entry)
{
    return entry.host[0] != '\0' && entry.port != 0u && entry.uid != 0u &&
           entry.callsign[0] != '\0';
}

void terminate(FmoFavorite &entry)
{
    entry.name[FMO_SERVER_NAME_MAX - 1] = '\0';
    entry.host[FMO_SERVER_HOST_MAX - 1] = '\0';
    entry.callsign[FMO_SERVER_CALLSIGN_MAX - 1] = '\0';
}

} // namespace

extern "C" int FMO_FAV_CORE_Find(const FmoFavoriteList *list, const uint32_t uid,
                                 const char *host, const uint16_t port)
{
    if (list == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < list->count; ++i) {
        const FmoFavorite &entry = list->entries[i];
        if (uid != 0u) {
            if (entry.uid == uid) {
                return static_cast<int>(i);
            }
        } else if (host != nullptr && strcmp(entry.host, host) == 0 &&
                   entry.port == port) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

extern "C" bool FMO_FAV_CORE_Add(FmoFavoriteList *list, const FmoFavorite *entry)
{
    if (list == nullptr || entry == nullptr || !entryUsable(*entry)) {
        return false;
    }
    FmoFavorite copy = *entry;
    terminate(copy);
    const int found = FMO_FAV_CORE_Find(list, copy.uid, copy.host, copy.port);
    if (found >= 0) {
        list->entries[found] = copy;
        return true;
    }
    if (list->count >= FMO_FAV_MAX) {
        return false;
    }
    list->entries[list->count++] = copy;
    return true;
}

extern "C" bool FMO_FAV_CORE_Remove(FmoFavoriteList *list, const size_t index)
{
    if (list == nullptr || index >= list->count) {
        return false;
    }
    memmove(&list->entries[index], &list->entries[index + 1],
            (list->count - index - 1u) * sizeof(FmoFavorite));
    --list->count;
    return true;
}
