#ifndef DRIVER_I2C_H
#define DRIVER_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <driver/i2c_master.h>

// One shared ESP-IDF I2C master bus serves the board's codec, touch controller
// and direct EEPROM window. ESP-IDF serializes transactions on the bus, so
// callers from LVGL, HTTP and audio tasks cannot interleave individual bytes.
bool I2C_MasterGetBus(i2c_master_bus_handle_t *out_bus);
bool I2C_MasterProbe(uint8_t address, int timeout_ms);
bool I2C_MasterTransmit(uint8_t address, const uint8_t *data, size_t size,
                        int timeout_ms);
bool I2C_MasterReceive(uint8_t address, uint8_t *data, size_t size,
                       int timeout_ms);
bool I2C_MasterTransmitReceive(uint8_t address,
                               const uint8_t *write_data, size_t write_size,
                               uint8_t *read_data, size_t read_size,
                               int timeout_ms);
// Single-shot variant used by full-bus scans: creates a temporary device
// handle and removes it right after the transfer, so sweeping many addresses
// cannot exhaust the cached device slots the runtime drivers rely on.
bool I2C_MasterTransmitReceiveOnce(uint8_t address,
                                   const uint8_t *write_data, size_t write_size,
                                   uint8_t *read_data, size_t read_size,
                                   int timeout_ms);

#endif // DRIVER_I2C_H
