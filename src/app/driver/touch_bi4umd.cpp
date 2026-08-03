#include "touch_bi4umd.h"

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_BI4UMD

#include "i2c1.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr uint8_t kRegTouchCount = 0x02;
constexpr uint8_t kMaxTouchCount = 5;
const char *TAG = "BI4UMD_TOUCH";
bool s_ready = false;

bool readRegisters(const uint8_t reg, uint8_t *data, const uint8_t size)
{
    if (data == nullptr || size == 0) {
        return false;
    }

    return I2C_MasterTransmitReceive(NRL_TOUCH_I2C_ADDR, &reg, 1u,
                                     data, size, 50);
}

} // namespace

bool BI4UMD_Touch_Init(void)
{
    gpio_config_t input = {};
    input.pin_bit_mask = 1ULL << NRL_PIN_TOUCH_INT;
    input.mode = GPIO_MODE_INPUT;
    input.pull_up_en = GPIO_PULLUP_ENABLE;
    input.pull_down_en = GPIO_PULLDOWN_DISABLE;
    input.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&input);

    gpio_config_t reset = {};
    reset.pin_bit_mask = 1ULL << NRL_PIN_TOUCH_RST;
    reset.mode = GPIO_MODE_OUTPUT;
    gpio_config(&reset);
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_TOUCH_RST), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_TOUCH_RST), 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!I2C_MasterProbe(NRL_TOUCH_I2C_ADDR, 100)) {
        ESP_LOGW(TAG, "controller 0x%02X not found", NRL_TOUCH_I2C_ADDR);
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "FT5x06/FT6x36 touch ready (SCL=%d SDA=%d INT=%d RST=%d)",
             NRL_PIN_I2C_SCL, NRL_PIN_I2C_SDA,
             NRL_PIN_TOUCH_INT, NRL_PIN_TOUCH_RST);
    return true;
}

bool BI4UMD_Touch_Read(uint16_t *x, uint16_t *y)
{
    return BI4UMD_Touch_ReadPoints(x, y, 1u) != 0u;
}

uint8_t BI4UMD_Touch_ReadPoints(uint16_t *x, uint16_t *y, uint8_t capacity)
{
    if (!s_ready || x == nullptr || y == nullptr || capacity == 0u) return 0u;

    // Reading while INT is high is still needed once to observe release, but
    // the controller's touch-count register makes that read unambiguous.
    constexpr uint8_t kPointBytes = 6u;
    const uint8_t requested = capacity < 2u ? capacity : 2u;
    uint8_t data[1u + 2u * kPointBytes] = {};
    const uint8_t read_size = static_cast<uint8_t>(1u + requested * kPointBytes);
    if (!readRegisters(kRegTouchCount, data, read_size)) return 0u;
    const uint8_t reported = data[0] & 0x0Fu;
    if (reported == 0u || reported > kMaxTouchCount) return 0u;

    const uint8_t count = reported < requested ? reported : requested;
    uint8_t valid = 0u;
    for (uint8_t i = 0u; i < count; ++i) {
        const uint8_t *point = data + 1u + i * kPointBytes;
        const uint16_t px = static_cast<uint16_t>(((point[0] & 0x0Fu) << 8) | point[1]);
        const uint16_t py = static_cast<uint16_t>(((point[2] & 0x0Fu) << 8) | point[3]);
        if (px < NRL_DISPLAY_WIDTH && py < NRL_DISPLAY_HEIGHT) {
            x[valid] = px;
            y[valid] = py;
            ++valid;
        }
    }
    return valid;
}

#else

bool BI4UMD_Touch_Init(void) { return false; }
bool BI4UMD_Touch_Read(uint16_t *, uint16_t *) { return false; }
uint8_t BI4UMD_Touch_ReadPoints(uint16_t *, uint16_t *, uint8_t) { return 0u; }

#endif
