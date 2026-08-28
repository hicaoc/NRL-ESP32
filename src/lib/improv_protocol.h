#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Improv Serial protocol handler (https://www.improv-wifi.com/serial/).
// Enables ESP Web Tools to detect the running firmware, query WiFi state,
// scan networks and provision WiFi credentials over the USB/UART console.
//
// Frame: 'I' 'M' 'P' 'R' 'O' 'V' | version | type | length | data[N] | checksum
// Checksum = simple sum (LSB) of all preceding bytes (magic through data).

#ifdef __cplusplus
extern "C" {
#endif

// Feed one byte from the serial console. Returns true if the byte was consumed
// by the Improv state machine (caller should NOT pass it to the AT parser).
bool IMPROV_ProcessByte(uint8_t byte);

// Bytes swallowed while probing for the "IMPROV" magic that turned out to be
// plain AT text are queued here for the caller to replay into the AT parser.
// Returns the next rejected byte, or -1 when the queue is empty. Drain after
// every IMPROV_ProcessByte call, regardless of its return value.
int IMPROV_ReadRejected(void);

#ifdef __cplusplus
}
#endif
