// Host unit test for the FMO online-count roster and the online/peak
// override semantics (src/services/fmo_station_broadcast_core.cpp):
// distinct-uid counting inside the 120 s sliding window, window expiry,
// session peak, capacity eviction, and the 0 = automatic override rule.
//
// Build & run (from the repo root, MSYS2/MinGW g++ works):
//   g++ -std=c++17 -Wall -Wextra -I src tests/fmo_online_roster_test.cpp
//       src/services/fmo_station_broadcast_core.cpp
//       -o .tmp/fmo_online_roster_test.exe
//   ./.tmp/fmo_online_roster_test.exe

#include "services/fmo_station_broadcast_core.h"

#include <assert.h>
#include <stdio.h>

namespace {

FmoOnlineRoster s_roster; // static: ~4 KiB

void testCounting()
{
    FMO_STATION_CORE_RosterReset(&s_roster);
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 1000u) == 0u);
    // uid 0 is a valid, countable id.
    assert(FMO_STATION_CORE_RosterFeed(&s_roster, 0u, 1000u) == 1u);
    assert(FMO_STATION_CORE_RosterFeed(&s_roster, 42u, 1000u) == 2u);
    assert(FMO_STATION_CORE_RosterFeed(&s_roster, 43u, 1010u) == 3u);
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 1010u) == 3u);
    // A repeated heartbeat refreshes the entry, it does not double count.
    assert(FMO_STATION_CORE_RosterFeed(&s_roster, 42u, 1020u) == 3u);
    assert(FMO_STATION_CORE_RosterFeed(&s_roster, 0u, 1020u) == 3u);
    printf("ok: distinct-uid counting (uid 0 counts)\n");
}

void testWindowExpiry()
{
    FMO_STATION_CORE_RosterReset(&s_roster);
    FMO_STATION_CORE_RosterFeed(&s_roster, 1u, 1000u);
    FMO_STATION_CORE_RosterFeed(&s_roster, 2u, 1119u); // 119 s later, inside
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 1119u) == 2u);
    // uid 1 expires exactly 120 s after its last heartbeat.
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 1120u) == 1u);
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 1239u) == 0u);
    // A heartbeat inside the window slides the expiry forward.
    FMO_STATION_CORE_RosterFeed(&s_roster, 3u, 2000u);
    FMO_STATION_CORE_RosterFeed(&s_roster, 3u, 2110u);
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 2119u) == 1u);
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 2230u) == 0u);
    // Unsigned arithmetic stays correct across a now_s wraparound.
    FMO_STATION_CORE_RosterReset(&s_roster);
    FMO_STATION_CORE_RosterFeed(&s_roster, 7u, 0xfffffff0u);
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 10u) == 1u);
    printf("ok: 120 s window expiry\n");
}

void testSessionPeak()
{
    FMO_STATION_CORE_RosterReset(&s_roster);
    for (uint32_t i = 0u; i < 10u; ++i) {
        FMO_STATION_CORE_RosterFeed(&s_roster, i, 1000u);
    }
    assert(s_roster.session_peak == 10u);
    // Everything expires; the session peak survives.
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 2000u) == 0u);
    assert(s_roster.session_peak == 10u);
    FMO_STATION_CORE_RosterFeed(&s_roster, 100u, 3000u);
    assert(s_roster.session_peak == 10u);
    // Disconnect reset clears the roster and the session peak.
    FMO_STATION_CORE_RosterReset(&s_roster);
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 3000u) == 0u);
    assert(s_roster.session_peak == 0u);
    printf("ok: session peak survives expiry, cleared on reset\n");
}

void testCapacityEviction()
{
    FMO_STATION_CORE_RosterReset(&s_roster);
    // uid 0 is the oldest entry; all of them are inside the window at t=1000.
    FMO_STATION_CORE_RosterFeed(&s_roster, 0u, 900u);
    for (uint32_t i = 1u; i < FMO_STATION_ROSTER_CAPACITY; ++i) {
        FMO_STATION_CORE_RosterFeed(&s_roster, i, 1000u);
    }
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 1000u) ==
           FMO_STATION_ROSTER_CAPACITY);
    assert(s_roster.session_peak == FMO_STATION_ROSTER_CAPACITY);
    // Full roster: a new uid evicts the oldest entry (uid 0 @ 900).
    assert(FMO_STATION_CORE_RosterFeed(&s_roster, 9999u, 1001u) ==
           FMO_STATION_ROSTER_CAPACITY);
    // A heartbeat for an already-tracked uid never triggers eviction.
    assert(FMO_STATION_CORE_RosterFeed(&s_roster, 5u, 1002u) ==
           FMO_STATION_ROSTER_CAPACITY);
    // Proof that uid 0 (last seen 900) was evicted: at t=1021 it would have
    // expired and dropped the count to CAPACITY-1 if it were still present.
    assert(FMO_STATION_CORE_RosterOnline(&s_roster, 1021u) ==
           FMO_STATION_ROSTER_CAPACITY);
    assert(s_roster.session_peak == FMO_STATION_ROSTER_CAPACITY);
    printf("ok: capacity cap, oldest-first eviction\n");
}

void testEffectiveOverride()
{
    // 0 = automatic (default), >0 = manual override.
    assert(FMO_STATION_CORE_EffectiveCount(0u, 7u) == 7u);
    assert(FMO_STATION_CORE_EffectiveCount(5u, 7u) == 5u);
    assert(FMO_STATION_CORE_EffectiveCount(0u, 0u) == 0u);
    printf("ok: 0 = automatic, >0 = manual override\n");
}

} // namespace

int main()
{
    testCounting();
    testWindowExpiry();
    testSessionPeak();
    testCapacityEviction();
    testEffectiveOverride();
    printf("all fmo_online_roster tests passed\n");
    return 0;
}
