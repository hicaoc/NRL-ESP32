#ifndef DRIVER_I2C_DEVICE_DISCOVERY_H
#define DRIVER_I2C_DEVICE_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

enum class I2CDeviceModel : uint8_t {
    Unknown = 0,
    Pca9555,
    Es8311,
    Ft5x06OrAht20,
    Bmp280,
    Qmc5883l,
    Bh1750,
    Pca9555OrBh1750,
};

struct I2CDiscoveredDevice {
    uint8_t address_7bit;
    uint8_t write_address_8bit;
    uint8_t read_address_8bit;
    I2CDeviceModel model;
    uint8_t identity;
    bool identity_valid;
};

// Scans all 128 7-bit addresses. Together, their write/read address bytes
// cover the raw 0x00-0xFF range requested by the diagnostic UI.
bool I2C_DEVICE_DISCOVERY_Scan(void);
// Probes only the pluggable sensor addresses (used at boot: the screen,
// touch, ES8311 and PCA9555 are soldered down and never scanned).
bool I2C_DEVICE_DISCOVERY_ScanSensors(void);
size_t I2C_DEVICE_DISCOVERY_GetSnapshot(I2CDiscoveredDevice *devices,
                                        size_t capacity,
                                        uint32_t *revision);
uint8_t I2C_DEVICE_DISCOVERY_GetAddress(I2CDeviceModel model,
                                        uint8_t fallback_address);
// Model found at a given 7-bit address, or I2CDeviceModel::Unknown when the
// address did not respond. Lets callers refuse to poke an address whose
// model is still an unresolved conflict (e.g. BH1750/PCA9555 at 0x23).
I2CDeviceModel I2C_DEVICE_DISCOVERY_GetModelAt(uint8_t address_7bit);
uint32_t I2C_DEVICE_DISCOVERY_GetRevision(void);
const char *I2C_DEVICE_DISCOVERY_ModelName(I2CDeviceModel model);

#endif // DRIVER_I2C_DEVICE_DISCOVERY_H
