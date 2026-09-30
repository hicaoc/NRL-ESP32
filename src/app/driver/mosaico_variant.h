#ifndef DRIVER_MOSAICO_VARIANT_H
#define DRIVER_MOSAICO_VARIANT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ESP-Mosaico hardware revision handling, same scheme as the official BSP:
// eFuse USER_DATA holds the board version. v1.2 moves the shared I2C bus to
// GPIO56/3 and swaps the LCD SCL/RST pins vs v1.0. MosaicoVariant_Init() runs
// first thing in initApp(); all accessors are cheap cached reads.
void MosaicoVariant_Init(void);
bool MosaicoVariant_IsV1_2(void);
int MosaicoVariant_LcdClkPin(void);    // 44 on v1.0, 42 on v1.2
int MosaicoVariant_LcdRstPin(void);    // 42 on v1.0, 44 on v1.2
int MosaicoVariant_StatusLedPin(void); // GPIO3 on v1.0, -1 on v1.2 (I2C SCL there)

#ifdef __cplusplus
}
#endif

#endif // DRIVER_MOSAICO_VARIANT_H
