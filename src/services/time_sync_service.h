#pragma once

// Start the process-wide SNTP client once. Safe to call concurrently from
// display, FMO, or other network tasks. Returns true only for the caller that
// actually starts the client.
bool TIME_SYNC_StartIfNeeded();
