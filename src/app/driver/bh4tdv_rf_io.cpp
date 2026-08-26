#include "bh4tdv_rf_io.h"

#include "board_pins.h"
#include "i2c1.h"
#include "i2c_device_discovery.h"

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr uint8_t kDefaultAddress = 0x20u;
constexpr uint8_t kRegInput0 = 0x00u;
constexpr uint8_t kRegOutput0 = 0x02u;
constexpr uint8_t kRegConfig0 = 0x06u;
constexpr uint8_t kConfig0 = 0xBFu; // P0.6 GPS_EN is the only output.
constexpr uint8_t kConfig1 = 0xE9u; // P1.1 PD, P1.2 PTT1, P1.4 H/L are outputs.
constexpr uint8_t kP0GpsEnable = 1u << 6;
constexpr uint8_t kP1RadioPower = 1u << 1;
constexpr uint8_t kP1RadioPtt = 1u << 2;
constexpr uint8_t kP1Sql = 1u << 3;
constexpr uint8_t kP1LowPower = 1u << 4;
constexpr uint8_t kP1KeyConfirm = 1u << 5;
constexpr uint8_t kP1KeyUp = 1u << 6;
constexpr uint8_t kP1KeyDown = 1u << 7;
constexpr const char *TAG = "BH4TDV_RF_IO";

SemaphoreHandle_t s_mutex = nullptr;
StaticSemaphore_t s_mutex_buffer = {};
portMUX_TYPE s_mutex_lock = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_output0 = 0u;
uint8_t s_output1 = kP1LowPower;
bool s_ready = false;
uint8_t s_address = kDefaultAddress;

bool ensureMutex()
{
    if (s_mutex != nullptr) return true;
    // Serialize the lazy allocation: Init can run concurrently from the LVGL
    // and HTTP tasks after a manual rescan.
    portENTER_CRITICAL(&s_mutex_lock);
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    }
    portEXIT_CRITICAL(&s_mutex_lock);
    return s_mutex != nullptr;
}

bool writePair(const uint8_t reg, const uint8_t port0, const uint8_t port1)
{
    const uint8_t data[] = {reg, port0, port1};
    return I2C_MasterTransmit(s_address, data, sizeof(data), 100);
}

bool updateOutputLocked(const uint8_t port, const uint8_t bit, const bool enabled)
{
    uint8_t next0 = s_output0;
    uint8_t next1 = s_output1;
    uint8_t &next = port == 0u ? next0 : next1;
    if (enabled) next |= bit;
    else next &= static_cast<uint8_t>(~bit);
    if (next0 == s_output0 && next1 == s_output1) return true;
    if (!writePair(kRegOutput0, next0, next1)) return false;
    s_output0 = next0;
    s_output1 = next1;
    return true;
}

bool setOutput(const uint8_t port, const uint8_t bit, const bool enabled)
{
    if (!s_ready || s_mutex == nullptr ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const bool ok = updateOutputLocked(port, bit, enabled);
    xSemaphoreGive(s_mutex);
    return ok;
}

} // namespace

bool BH4TDV_RF_IO_Init(void)
{
    const uint8_t detected_address = I2C_DEVICE_DISCOVERY_GetAddress(
        I2CDeviceModel::Pca9555, kDefaultAddress);
    if (s_ready && s_address == detected_address) return true;
    s_ready = false;
    s_address = detected_address;
    if (!ensureMutex() || !I2C_MasterProbe(s_address, 100)) {
        ESP_LOGE(TAG, "PCA9555 not found at 7-bit 0x%02X (write 0x%02X)",
                 s_address, static_cast<uint8_t>(s_address << 1u));
        return false;
    }

    // Program safe output latches before enabling any output driver. PTT and
    // PD stay off; H/L's transistor is on so the radio starts in low power.
    s_output0 = 0u;
    s_output1 = kP1LowPower;
    if (!writePair(kRegOutput0, s_output0, s_output1) ||
        !writePair(kRegConfig0, kConfig0, kConfig1)) {
        ESP_LOGE(TAG, "PCA9555 safe initialization failed");
        return false;
    }

    gpio_reset_pin(static_cast<gpio_num_t>(NRL_PIN_PCA9555_INT));
    gpio_set_direction(static_cast<gpio_num_t>(NRL_PIN_PCA9555_INT), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(NRL_PIN_PCA9555_INT), GPIO_PULLUP_ONLY);
    s_ready = true;
    ESP_LOGI(TAG, "PCA9555 ready: address=0x%02X cfg0=0x%02X cfg1=0x%02X",
             s_address, kConfig0, kConfig1);
    return true;
}

bool BH4TDV_RF_IO_IsReady(void) { return s_ready; }

bool BH4TDV_RF_IO_Read(uint8_t *keys, bool *sql_active)
{
    if (!s_ready || keys == nullptr || sql_active == nullptr) return false;
    uint8_t reg = kRegInput0;
    uint8_t input[2] = {};
    if (!I2C_MasterTransmitReceive(s_address, &reg, 1u, input, sizeof(input), 100)) {
        return false;
    }
    uint8_t pressed = 0u;
    if ((input[0] & (1u << 0)) == 0u) pressed |= BH4TDV_RF_KEY_F2;
    if ((input[0] & (1u << 1)) == 0u) pressed |= BH4TDV_RF_KEY_F3;
    if ((input[1] & kP1KeyDown) == 0u) pressed |= BH4TDV_RF_KEY_DOWN;
    if ((input[1] & kP1KeyUp) == 0u) pressed |= BH4TDV_RF_KEY_UP;
    if ((input[1] & kP1KeyConfirm) == 0u) pressed |= BH4TDV_RF_KEY_CONFIRM;
    *keys = pressed;
    *sql_active = (input[1] & kP1Sql) == 0u;
    return true;
}

bool BH4TDV_RF_IO_SetGpsPower(const bool enabled)
{
    return setOutput(0u, kP0GpsEnable, enabled);
}

bool BH4TDV_RF_IO_SetRadioPower(const bool enabled)
{
    if (!enabled) (void)BH4TDV_RF_IO_SetRadioPtt(false);
    return setOutput(1u, kP1RadioPower, enabled);
}

bool BH4TDV_RF_IO_SetRadioPtt(const bool transmit)
{
    return setOutput(1u, kP1RadioPtt, transmit);
}

bool BH4TDV_RF_IO_IsTransmitting(void)
{
    return s_ready && (s_output1 & kP1RadioPtt) != 0u;
}

bool BH4TDV_RF_IO_SetLowPower(const bool low_power)
{
    return setOutput(1u, kP1LowPower, low_power);
}

#else

bool BH4TDV_RF_IO_Init(void) { return true; }
bool BH4TDV_RF_IO_IsReady(void) { return false; }
bool BH4TDV_RF_IO_Read(uint8_t *, bool *) { return false; }
bool BH4TDV_RF_IO_SetGpsPower(bool) { return false; }
bool BH4TDV_RF_IO_SetRadioPower(bool) { return false; }
bool BH4TDV_RF_IO_SetRadioPtt(bool) { return false; }
bool BH4TDV_RF_IO_IsTransmitting(void) { return false; }
bool BH4TDV_RF_IO_SetLowPower(bool) { return false; }

#endif
