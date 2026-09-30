#ifndef DRIVER_STATUS_IO_H
#define DRIVER_STATUS_IO_H

#include <stdbool.h>
#include <stdint.h>

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
// True while NRL server traffic has arrived within the last few seconds (same
// window as the white NET LED): the practical "NRL server link up" signal.
bool STATUS_IO_NrlServerLinked(void);
// Hold-to-talk from a touch UI region (S31): true = key up (transmit), false =
// release. No-op on boards whose STATUS_IO build doesn't implement it.
void STATUS_IO_SetSoftPtt(bool held);
// Pulse the vibration motor for `ms` milliseconds (ESP-Mosaico; no-op on
// boards without one). Non-blocking: Poll() turns the motor off.
void STATUS_IO_Vibrate(uint32_t ms);
// Button vibration-feedback master switch (ESP-Mosaico). When off,
// STATUS_IO_Vibrate is a no-op; the incoming-call ring is unaffected.
void STATUS_IO_SetHapticEnabled(bool enabled);
bool STATUS_IO_HapticEnabled(void);
bool STATUS_IO_IsSqlActive(void);
bool STATUS_IO_IsPttActive(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_STATUS_IO_H
