#ifndef DRIVER_TOUCH_BI4UMD_H
#define DRIVER_TOUCH_BI4UMD_H

#include <stdbool.h>
#include <stdint.h>

bool BI4UMD_Touch_Init(void);
bool BI4UMD_Touch_Read(uint16_t *x, uint16_t *y);
uint8_t BI4UMD_Touch_ReadPoints(uint16_t *x, uint16_t *y, uint8_t capacity);

#endif // DRIVER_TOUCH_BI4UMD_H
