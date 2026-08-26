#include "i2c_device_discovery.h"

#include "board_pins.h"
#include "i2c1.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string.h>

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF

namespace {

constexpr const char *TAG = "I2C_SCAN";
constexpr size_t kMaxDevices = 32u;
constexpr int kProbeTimeoutMs = 5;

I2CDiscoveredDevice s_devices[kMaxDevices] = {};
size_t s_device_count = 0u;
uint32_t s_revision = 0u;
portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t s_scan_mutex = nullptr;
StaticSemaphore_t s_scan_mutex_buffer = {};
portMUX_TYPE s_scan_mutex_lock = portMUX_INITIALIZER_UNLOCKED;

bool ensureScanMutex()
{
    if (s_scan_mutex != nullptr) return true;
    // Serialize the lazy allocation: concurrent first callers (LVGL task,
    // HTTP task) must not each create their own mutex and break the
    // scan-reentrancy guard.
    portENTER_CRITICAL(&s_scan_mutex_lock);
    if (s_scan_mutex == nullptr) {
        s_scan_mutex = xSemaphoreCreateMutexStatic(&s_scan_mutex_buffer);
    }
    portEXIT_CRITICAL(&s_scan_mutex_lock);
    return s_scan_mutex != nullptr;
}

bool readRegisters(const uint8_t address, const uint8_t reg,
                   uint8_t *data, const size_t size)
{
    // Uncached single-shot transfer: a full sweep must not occupy the shared
    // driver device-cache slots with throwaway scan addresses.
    return I2C_MasterTransmitReceiveOnce(address, &reg, 1u, data, size, 30);
}

I2CDeviceModel identifyDevice(const uint8_t address,
                              uint8_t *identity, bool *identity_valid)
{
    *identity = 0u;
    *identity_valid = false;

    if (address == 0x76u || address == 0x77u) {
        uint8_t id = 0u;
        // 0x58 = BMP280, 0x60 = BME280 (shared temp/pressure register map).
        if (readRegisters(address, 0xD0u, &id, 1u) &&
            (id == 0x58u || id == 0x60u)) {
            *identity = id;
            *identity_valid = true;
            return I2CDeviceModel::Bmp280;
        }
    }
    if (address == 0x0Du) {
        uint8_t id = 0u;
        if (readRegisters(address, 0x0Du, &id, 1u)) {
            *identity = id;
            *identity_valid = true;
            return I2CDeviceModel::Qmc5883l;
        }
    }
    if (address == 0x18u) {
        uint8_t reset_register = 0u;
        if (readRegisters(address, 0x00u, &reset_register, 1u)) {
            *identity = reset_register;
            *identity_valid = true;
            return I2CDeviceModel::Es8311;
        }
    }
    if (address == 0x38u) {
        // Both devices use a fixed 0x38 address. Do not send an identification
        // command: if both are fitted, their simultaneous replies are unsafe.
        return I2CDeviceModel::Ft5x06OrAht20;
    }
    if (address == 0x23u) {
        // BH1750 ADDR=0 collides with a PCA9555 strapped to A2:A0=011.
        // Neither device has a non-destructive, unique chip-ID register.
        return I2CDeviceModel::Pca9555OrBh1750;
    }
    if (address == 0x5Cu) {
        return I2CDeviceModel::Bh1750;
    }
    if (address >= 0x20u && address <= 0x27u) {
        uint8_t configuration[2] = {};
        if (readRegisters(address, 0x06u, configuration,
                          sizeof(configuration))) {
            return I2CDeviceModel::Pca9555;
        }
    }
    return I2CDeviceModel::Unknown;
}

} // namespace

bool I2C_DEVICE_DISCOVERY_Scan(void)
{
    if (!ensureScanMutex() ||
        xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "scan already running or mutex allocation failed");
        return false;
    }

    I2CDiscoveredDevice found[kMaxDevices] = {};
    size_t count = 0u;
    ESP_LOGI(TAG, "scan raw address bytes 0x00-0xFF (7-bit 0x00-0x7F)");
    for (unsigned address = 0u; address <= 0x7Fu; ++address) {
        const uint8_t address_7bit = static_cast<uint8_t>(address);
        if (!I2C_MasterProbe(address_7bit, kProbeTimeoutMs)) continue;

        uint8_t identity = 0u;
        bool identity_valid = false;
        const I2CDeviceModel model = identifyDevice(
            address_7bit, &identity, &identity_valid);
        if (count < kMaxDevices) {
            found[count++] = {
                address_7bit,
                static_cast<uint8_t>(address_7bit << 1u),
                static_cast<uint8_t>((address_7bit << 1u) | 1u),
                model,
                identity,
                identity_valid,
            };
        }
        if (identity_valid) {
            ESP_LOGI(TAG, "found 7b=0x%02X W=0x%02X R=0x%02X model=%s id=0x%02X",
                     address_7bit,
                     static_cast<uint8_t>(address_7bit << 1u),
                     static_cast<uint8_t>((address_7bit << 1u) | 1u),
                     I2C_DEVICE_DISCOVERY_ModelName(model), identity);
        } else {
            ESP_LOGI(TAG, "found 7b=0x%02X W=0x%02X R=0x%02X model=%s",
                     address_7bit,
                     static_cast<uint8_t>(address_7bit << 1u),
                     static_cast<uint8_t>((address_7bit << 1u) | 1u),
                     I2C_DEVICE_DISCOVERY_ModelName(model));
        }
    }

    // 0x23 alone is ambiguous because neither BH1750 nor PCA9555 exposes a
    // unique, read-only chip ID. On BH4TDV-RF, finding a PCA9555-compatible
    // device at another address resolves 0x23 as the BH1750 module without
    // sending potentially disruptive guess commands to it.
    bool pca9555_found_elsewhere = false;
    for (size_t i = 0u; i < count; ++i) {
        if (found[i].model == I2CDeviceModel::Pca9555) {
            pca9555_found_elsewhere = true;
            break;
        }
    }
    if (pca9555_found_elsewhere) {
        for (size_t i = 0u; i < count; ++i) {
            if (found[i].address_7bit == 0x23u &&
                found[i].model == I2CDeviceModel::Pca9555OrBh1750) {
                found[i].model = I2CDeviceModel::Bh1750;
                ESP_LOGI(TAG, "resolved 7b=0x23 as BH1750 using PCA9555 at another address");
                break;
            }
        }
    }

    // Mirror case: a BH1750 answering at its alternate 0x5C address means the
    // module is not at 0x23, so an unresolved 0x23 is the PCA9555 strapped to
    // A2:A0=011. Resolving it lets the expander driver bind the real address
    // instead of failing on the fallback probe.
    bool bh1750_found_elsewhere = false;
    for (size_t i = 0u; i < count; ++i) {
        if (found[i].model == I2CDeviceModel::Bh1750 &&
            found[i].address_7bit != 0x23u) {
            bh1750_found_elsewhere = true;
            break;
        }
    }
    if (bh1750_found_elsewhere) {
        for (size_t i = 0u; i < count; ++i) {
            if (found[i].address_7bit == 0x23u &&
                found[i].model == I2CDeviceModel::Pca9555OrBh1750) {
                found[i].model = I2CDeviceModel::Pca9555;
                ESP_LOGI(TAG, "resolved 7b=0x23 as PCA9555 using BH1750 at another address");
                break;
            }
        }
    }

    portENTER_CRITICAL(&s_snapshot_lock);
    memcpy(s_devices, found, count * sizeof(found[0]));
    s_device_count = count;
    ++s_revision;
    portEXIT_CRITICAL(&s_snapshot_lock);
    xSemaphoreGive(s_scan_mutex);
    ESP_LOGI(TAG, "scan complete: %u device(s)", static_cast<unsigned>(count));
    return true;
}

size_t I2C_DEVICE_DISCOVERY_GetSnapshot(I2CDiscoveredDevice *devices,
                                        const size_t capacity,
                                        uint32_t *revision)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    const size_t count = s_device_count;
    const size_t copy_count = count < capacity ? count : capacity;
    if (devices != nullptr && copy_count > 0u) {
        memcpy(devices, s_devices, copy_count * sizeof(s_devices[0]));
    }
    if (revision != nullptr) *revision = s_revision;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return count;
}

uint8_t I2C_DEVICE_DISCOVERY_GetAddress(const I2CDeviceModel model,
                                        const uint8_t fallback_address)
{
    uint8_t result = fallback_address;
    portENTER_CRITICAL(&s_snapshot_lock);
    for (size_t i = 0u; i < s_device_count; ++i) {
        if (s_devices[i].model == model) {
            result = s_devices[i].address_7bit;
            break;
        }
    }
    portEXIT_CRITICAL(&s_snapshot_lock);
    return result;
}

I2CDeviceModel I2C_DEVICE_DISCOVERY_GetModelAt(const uint8_t address_7bit)
{
    I2CDeviceModel result = I2CDeviceModel::Unknown;
    portENTER_CRITICAL(&s_snapshot_lock);
    for (size_t i = 0u; i < s_device_count; ++i) {
        if (s_devices[i].address_7bit == address_7bit) {
            result = s_devices[i].model;
            break;
        }
    }
    portEXIT_CRITICAL(&s_snapshot_lock);
    return result;
}

uint32_t I2C_DEVICE_DISCOVERY_GetRevision(void)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    const uint32_t revision = s_revision;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return revision;
}

const char *I2C_DEVICE_DISCOVERY_ModelName(const I2CDeviceModel model)
{
    switch (model) {
        case I2CDeviceModel::Pca9555: return "PCA9555-compatible";
        case I2CDeviceModel::Es8311: return "ES8311";
        case I2CDeviceModel::Ft5x06OrAht20: return "FT5x06/AHT20 CONFLICT";
        case I2CDeviceModel::Bmp280: return "BMP280/BME280";
        case I2CDeviceModel::Qmc5883l: return "QMC5883L-compatible";
        case I2CDeviceModel::Bh1750: return "BH1750-compatible";
        case I2CDeviceModel::Pca9555OrBh1750:
            return "BH1750/PCA9555 CONFLICT";
        case I2CDeviceModel::Unknown:
        default: return "UNKNOWN";
    }
}

#else

bool I2C_DEVICE_DISCOVERY_Scan(void) { return true; }
size_t I2C_DEVICE_DISCOVERY_GetSnapshot(I2CDiscoveredDevice *, size_t,
                                        uint32_t *revision)
{
    if (revision != nullptr) *revision = 0u;
    return 0u;
}
uint8_t I2C_DEVICE_DISCOVERY_GetAddress(I2CDeviceModel, uint8_t fallback_address)
{
    return fallback_address;
}
I2CDeviceModel I2C_DEVICE_DISCOVERY_GetModelAt(uint8_t)
{
    return I2CDeviceModel::Unknown;
}
uint32_t I2C_DEVICE_DISCOVERY_GetRevision(void) { return 0u; }
const char *I2C_DEVICE_DISCOVERY_ModelName(I2CDeviceModel) { return "UNKNOWN"; }

#endif
