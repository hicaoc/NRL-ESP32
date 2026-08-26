#include "i2c1.h"

#include "board_pins.h"

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr uint32_t kBusSpeedHz = 100000u;
constexpr size_t kMaxDevices = 12u;
const char *TAG = "I2C";

struct CachedDevice {
    uint8_t address;
    i2c_master_dev_handle_t handle;
};

i2c_master_bus_handle_t s_bus = nullptr;
SemaphoreHandle_t s_init_mutex = nullptr;
StaticSemaphore_t s_init_mutex_buffer = {};
portMUX_TYPE s_init_mutex_lock = portMUX_INITIALIZER_UNLOCKED;
CachedDevice s_devices[kMaxDevices] = {};
size_t s_device_count = 0u;

bool ensureInitMutex()
{
    if (s_init_mutex != nullptr) return true;
    // Serialize the lazy allocation: concurrent first callers must not each
    // create (and leak) their own mutex.
    portENTER_CRITICAL(&s_init_mutex_lock);
    if (s_init_mutex == nullptr) {
        s_init_mutex = xSemaphoreCreateMutexStatic(&s_init_mutex_buffer);
    }
    portEXIT_CRITICAL(&s_init_mutex_lock);
    return s_init_mutex != nullptr;
}

bool ensureBusLocked()
{
    if (s_bus != nullptr) return true;
    i2c_master_bus_config_t config = {};
    config.i2c_port = kI2cPort;
    config.sda_io_num = static_cast<gpio_num_t>(NRL_PIN_I2C_SDA);
    config.scl_io_num = static_cast<gpio_num_t>(NRL_PIN_I2C_SCL);
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    const esp_err_t err = i2c_new_master_bus(&config, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "master bus ready: port=%d sda=%d scl=%d",
             static_cast<int>(kI2cPort), NRL_PIN_I2C_SDA, NRL_PIN_I2C_SCL);
    return true;
}

bool getDevice(const uint8_t address, i2c_master_dev_handle_t *out_device)
{
    if (out_device == nullptr || address > 0x7Fu || !ensureInitMutex()) {
        return false;
    }
    if (xSemaphoreTake(s_init_mutex, portMAX_DELAY) != pdTRUE) return false;

    bool ok = false;
    do {
        if (!ensureBusLocked()) break;

        for (size_t i = 0; i < s_device_count; ++i) {
            if (s_devices[i].address == address) {
                *out_device = s_devices[i].handle;
                ok = true;
                break;
            }
        }
        if (ok) break;
        if (s_device_count >= kMaxDevices) {
            ESP_LOGE(TAG, "device cache full for address 0x%02X", address);
            break;
        }

        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = address;
        device_config.scl_speed_hz = kBusSpeedHz;
        i2c_master_dev_handle_t device = nullptr;
        const esp_err_t err = i2c_master_bus_add_device(s_bus, &device_config, &device);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add device 0x%02X failed: %s", address, esp_err_to_name(err));
            break;
        }
        s_devices[s_device_count++] = {address, device};
        *out_device = device;
        ok = true;
    } while (false);

    xSemaphoreGive(s_init_mutex);
    return ok;
}

} // namespace

bool I2C_MasterGetBus(i2c_master_bus_handle_t *out_bus)
{
    if (out_bus == nullptr || !ensureInitMutex()) return false;
    if (xSemaphoreTake(s_init_mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool ok = ensureBusLocked();
    if (ok) *out_bus = s_bus;
    xSemaphoreGive(s_init_mutex);
    return ok;
}

bool I2C_MasterProbe(const uint8_t address, const int timeout_ms)
{
    i2c_master_bus_handle_t bus = nullptr;
    if (!I2C_MasterGetBus(&bus)) return false;
    return i2c_master_probe(bus, address, timeout_ms) == ESP_OK;
}

bool I2C_MasterTransmit(const uint8_t address, const uint8_t *data,
                        const size_t size, const int timeout_ms)
{
    if (data == nullptr || size == 0u) return false;
    i2c_master_dev_handle_t device = nullptr;
    return getDevice(address, &device) &&
           i2c_master_transmit(device, data, size, timeout_ms) == ESP_OK;
}

bool I2C_MasterReceive(const uint8_t address, uint8_t *data,
                       const size_t size, const int timeout_ms)
{
    if (data == nullptr || size == 0u) return false;
    i2c_master_dev_handle_t device = nullptr;
    return getDevice(address, &device) &&
           i2c_master_receive(device, data, size, timeout_ms) == ESP_OK;
}

bool I2C_MasterTransmitReceive(const uint8_t address,
                               const uint8_t *write_data, const size_t write_size,
                               uint8_t *read_data, const size_t read_size,
                               const int timeout_ms)
{
    if (write_data == nullptr || write_size == 0u ||
        read_data == nullptr || read_size == 0u) {
        return false;
    }
    i2c_master_dev_handle_t device = nullptr;
    return getDevice(address, &device) &&
           i2c_master_transmit_receive(device, write_data, write_size,
                                       read_data, read_size, timeout_ms) == ESP_OK;
}

bool I2C_MasterTransmitReceiveOnce(const uint8_t address,
                                   const uint8_t *write_data, const size_t write_size,
                                   uint8_t *read_data, const size_t read_size,
                                   const int timeout_ms)
{
    if (address > 0x7Fu || write_data == nullptr || write_size == 0u ||
        read_data == nullptr || read_size == 0u) {
        return false;
    }
    i2c_master_bus_handle_t bus = nullptr;
    if (!I2C_MasterGetBus(&bus)) return false;
    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = address;
    device_config.scl_speed_hz = kBusSpeedHz;
    i2c_master_dev_handle_t device = nullptr;
    if (i2c_master_bus_add_device(bus, &device_config, &device) != ESP_OK) {
        return false;
    }
    const esp_err_t err = i2c_master_transmit_receive(
        device, write_data, write_size, read_data, read_size, timeout_ms);
    i2c_master_bus_rm_device(device);
    return err == ESP_OK;
}
