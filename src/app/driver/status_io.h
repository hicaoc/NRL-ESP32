#ifndef DRIVER_STATUS_IO_H
#define DRIVER_STATUS_IO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void STATUS_IO_Init(void);
void STATUS_IO_Poll(void);
void STATUS_IO_SetPttActive(bool active);
// FMO downlink owns a second network-audio latch. The physical PTT/radio
// output remains asserted until both the NRL and FMO downlinks are idle.
void STATUS_IO_SetFmoPttActive(bool active);
void STATUS_IO_NotifyHeartbeatReceived(void);
// Hold-to-talk from a touch UI region (S31): true = key up (transmit), false =
// release. No-op on boards whose STATUS_IO build doesn't implement it.
void STATUS_IO_SetSoftPtt(bool held);
bool STATUS_IO_IsSqlActive(void);
bool STATUS_IO_IsPttActive(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_STATUS_IO_H
