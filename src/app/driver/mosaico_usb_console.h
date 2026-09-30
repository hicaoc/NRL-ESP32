#ifndef DRIVER_MOSAICO_USB_CONSOLE_H
#define DRIVER_MOSAICO_USB_CONSOLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the tinyusb CDC-ACM console on the Type-C OTG port and redirect
// stdout/stderr to it. ESP-Mosaico only.
bool MOSAICO_USB_CONSOLE_Init(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_MOSAICO_USB_CONSOLE_H
