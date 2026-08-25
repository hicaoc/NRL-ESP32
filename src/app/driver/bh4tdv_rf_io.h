#ifndef DRIVER_BH4TDV_RF_IO_H
#define DRIVER_BH4TDV_RF_IO_H

#include <stdbool.h>
#include <stdint.h>

enum Bh4tdvRfKeyMask : uint8_t {
    BH4TDV_RF_KEY_F2      = 1u << 0,
    BH4TDV_RF_KEY_F3      = 1u << 1,
    BH4TDV_RF_KEY_DOWN    = 1u << 2,
    BH4TDV_RF_KEY_UP      = 1u << 3,
    BH4TDV_RF_KEY_CONFIRM = 1u << 4,
};

bool BH4TDV_RF_IO_Init(void);
bool BH4TDV_RF_IO_IsReady(void);
bool BH4TDV_RF_IO_Read(uint8_t *keys, bool *sql_active);
bool BH4TDV_RF_IO_SetGpsPower(bool enabled);
bool BH4TDV_RF_IO_SetRadioPower(bool enabled);
bool BH4TDV_RF_IO_SetRadioPtt(bool transmit);
bool BH4TDV_RF_IO_SetLowPower(bool low_power);

#endif // DRIVER_BH4TDV_RF_IO_H
