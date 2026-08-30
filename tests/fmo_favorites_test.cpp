// Host unit test for the FMO server favorites list logic
// (src/services/fmo_favorites_core.cpp): upsert matching, capacity,
// validation and removal semantics.
//
// Build & run (from the repo root, MSYS2/MinGW g++ works):
//   g++ -std=c++17 -Wall -Wextra -I src -I tests/shims
//       tests/fmo_favorites_test.cpp
//       src/services/fmo_favorites_core.cpp
//       -o .tmp/fmo_favorites_test.exe
//   ./.tmp/fmo_favorites_test.exe

#include "services/fmo_favorites_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {

FmoFavorite makeEntry(const char *name, const char *callsign, uint32_t uid,
                      const char *host, uint16_t port)
{
    FmoFavorite entry = {};
    snprintf(entry.name, sizeof(entry.name), "%s", name);
    snprintf(entry.callsign, sizeof(entry.callsign), "%s", callsign);
    snprintf(entry.host, sizeof(entry.host), "%s", host);
    entry.uid = uid;
    entry.port = port;
    for (size_t i = 0; i < sizeof(entry.fingerprint); ++i) {
        entry.fingerprint[i] = static_cast<uint8_t>(i);
    }
    return entry;
}

} // namespace

int main()
{
    FmoFavoriteList list = {};

    // Validation: unusable entries (mirror of the link's serverUsable) fail.
    FmoFavorite no_host = makeEntry("A", "BG1AAA", 1001, "", 18773);
    assert(!FMO_FAV_CORE_Add(&list, &no_host));
    FmoFavorite no_port = makeEntry("A", "BG1AAA", 1001, "fmo.example.com", 0);
    assert(!FMO_FAV_CORE_Add(&list, &no_port));
    FmoFavorite no_uid = makeEntry("A", "BG1AAA", 0, "fmo.example.com", 18773);
    assert(!FMO_FAV_CORE_Add(&list, &no_uid));
    FmoFavorite no_call = makeEntry("A", "", 1001, "fmo.example.com", 18773);
    assert(!FMO_FAV_CORE_Add(&list, &no_call));
    assert(list.count == 0u);

    // Plain adds and find by uid / host:port.
    FmoFavorite a = makeEntry("Alpha", "BG1AAA", 1001, "a.fmo.example.com", 18773);
    FmoFavorite b = makeEntry("Bravo", "BG2BBB", 1002, "b.fmo.example.com", 18774);
    assert(FMO_FAV_CORE_Add(&list, &a));
    assert(FMO_FAV_CORE_Add(&list, &b));
    assert(list.count == 2u);
    assert(FMO_FAV_CORE_Find(&list, 1001, nullptr, 0) == 0);
    assert(FMO_FAV_CORE_Find(&list, 1002, nullptr, 0) == 1);
    assert(FMO_FAV_CORE_Find(&list, 9999, nullptr, 0) == -1);
    assert(FMO_FAV_CORE_Find(&list, 0, "b.fmo.example.com", 18774) == 1);
    assert(FMO_FAV_CORE_Find(&list, 0, "b.fmo.example.com", 1) == -1);

    // Upsert by uid: same uid with a new host/name updates in place.
    FmoFavorite a2 = makeEntry("Alpha2", "BG1AAA", 1001, "a2.fmo.example.com", 18773);
    assert(FMO_FAV_CORE_Add(&list, &a2));
    assert(list.count == 2u);
    assert(strcmp(list.entries[0].name, "Alpha2") == 0);
    assert(strcmp(list.entries[0].host, "a2.fmo.example.com") == 0);

    // Upsert by host:port happens through Find's fallback; a different uid
    // counts as a new entry because uid matching wins when non-zero.
    FmoFavorite c = makeEntry("Alpha3", "BG3CCC", 1003, "a2.fmo.example.com", 18773);
    assert(FMO_FAV_CORE_Add(&list, &c));
    assert(list.count == 3u);

    // Overlong text is force-terminated on copy.
    FmoFavorite long_name = makeEntry("x", "BG4DDD", 1004, "d.fmo.example.com", 18773);
    memset(long_name.name, 'N', sizeof(long_name.name)); // no NUL anywhere
    assert(FMO_FAV_CORE_Add(&list, &long_name));
    assert(list.entries[3].name[FMO_SERVER_NAME_MAX - 1] == '\0');

    // Removal shifts and keeps order.
    assert(!FMO_FAV_CORE_Remove(&list, list.count));
    assert(FMO_FAV_CORE_Remove(&list, 1));
    assert(list.count == 3u);
    assert(FMO_FAV_CORE_Find(&list, 1002, nullptr, 0) == -1);
    assert(FMO_FAV_CORE_Find(&list, 1003, nullptr, 0) == 1);

    // Capacity: fill to FMO_FAV_MAX, the next add fails.
    while (list.count < FMO_FAV_MAX) {
        char host[32];
        snprintf(host, sizeof(host), "h%u.fmo.example.com", static_cast<unsigned>(list.count));
        FmoFavorite filler = makeEntry("F", "BG5EEE", 2000u + list.count, host, 18773);
        assert(FMO_FAV_CORE_Add(&list, &filler));
    }
    FmoFavorite overflow = makeEntry("X", "BG6FFF", 3001, "x.fmo.example.com", 18773);
    assert(!FMO_FAV_CORE_Add(&list, &overflow));
    // ...but an upsert of an existing entry still succeeds on a full list.
    FmoFavorite upd = makeEntry("F2", "BG5EEE", 2005, "h5.fmo.example.com", 18773);
    assert(FMO_FAV_CORE_Add(&list, &upd));

    printf("fmo_favorites_test: all assertions passed\n");
    return 0;
}
