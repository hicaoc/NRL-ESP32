#ifndef DRIVER_SR110U_H
#define DRIVER_SR110U_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool SR110U_Init(void);
bool SR110U_IsReady(void);
bool SR110U_Command(const char *command, char *response, size_t response_size,
                    unsigned timeout_ms);
// Binary-safe variant for commands that embed raw bytes (AT+DMOGRP tones).
bool SR110U_CommandRaw(const uint8_t *data, size_t data_len,
                       char *response, size_t response_size,
                       unsigned timeout_ms);
// Cleaned module version from AT+DMOVERQ (e.g. "110U-V303"), empty when the
// module has not answered yet.
const char *SR110U_GetVersion(void);
// Last RSSI (0..127) fetched by SR110U_PollRssi(); -1 when unknown/offline.
int SR110U_GetCachedRssi(void);
// Converts a raw RSSI register value (0..127) to dBm per the datasheet table.
int SR110U_RssiToDbm(int rssi);
// Fetches AT+DMORSSI into the cache when the module is ready. Unlike
// SR110U_ReadRssi() this never powers up / re-initializes the module, so it
// is safe to call from a periodic worker while the radio is powered down.
void SR110U_PollRssi(void);
// Drops the cached handshake state after the module power was cycled, so the
// next SR110U_Init() redoes AT+DMOCONT instead of trusting a stale ready flag.
void SR110U_NotifyPowerCycled(void);
int SR110U_ReadRssi(void);

#endif // DRIVER_SR110U_H
