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
// Programs safe expander outputs (PTT/PD off, low power) at the default strap
// address before the slow full-bus scan runs; Init re-binds afterwards if the
// scan finds the expander at another address.
bool BH4TDV_RF_IO_EarlySafeInit(void);
bool BH4TDV_RF_IO_IsReady(void);
bool BH4TDV_RF_IO_Read(uint8_t *keys, bool *sql_active);
bool BH4TDV_RF_IO_SetGpsPower(bool enabled);
bool BH4TDV_RF_IO_SetRadioPower(bool enabled);
bool BH4TDV_RF_IO_SetRadioPtt(bool transmit);
// Current radio PTT output state (driven from the PCA9555 output latch).
bool BH4TDV_RF_IO_IsTransmitting(void);
// Drives the PCA9555 P1.4 radio-type select: true = YAESU/MOTO (high),
// false = other radios (low). (RJ11 board; the old H/L line is gone.)
bool BH4TDV_RF_IO_SetYaesuMoto(bool yaesu_moto);
// NET/SQL/PTT status LEDs on PCA9555 P0.2/P0.3/P0.4, active low. Arguments
// are logical on/off; one I2C write, skipped when nothing changes.
bool BH4TDV_RF_IO_SetStatusLeds(bool net, bool sql, bool ptt);

#endif // DRIVER_BH4TDV_RF_IO_H
