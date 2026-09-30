// ESP-Mosaico hardware revision detection (eFuse USER_DATA, same encoding as
// the official BSP: (major << 8) | minor). v1.1/v1.2 share the v1.2 pinout:
// I2C on GPIO56/3, LCD SCL=42 RST=44, and no status LED (GPIO3 is I2C SCL).

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include "mosaico_variant.h"
#include "i2c1.h"

#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_log.h>

namespace {

const char *kTag = "MOSAICO_VARIANT";
bool s_is_v1_2 = false;

} // namespace

extern "C" void MosaicoVariant_Init(void)
{
    uint16_t hwver = 0;
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &hwver, 16) == ESP_OK) {
        s_is_v1_2 = (hwver == 0x0101u || hwver == 0x0102u);
    } else {
        ESP_LOGW(kTag, "eFuse hw version read failed; assuming v1.0 pins");
    }
    ESP_LOGI(kTag, "hw version v%u.%u -> %s pinout",
             static_cast<unsigned>(hwver >> 8), static_cast<unsigned>(hwver & 0xFF),
             s_is_v1_2 ? "v1.2" : "v1.0");
    if (s_is_v1_2) {
        // The shared I2C bus moves off GPIO0/1. The bus is created lazily, so
        // an override now lands before any device touches it.
        I2C_OverridePins(NRL_PIN_I2C_SDA_V1_2, NRL_PIN_I2C_SCL_V1_2);
    }
}

extern "C" bool MosaicoVariant_IsV1_2(void)
{
    return s_is_v1_2;
}

extern "C" int MosaicoVariant_LcdClkPin(void)
{
    return s_is_v1_2 ? NRL_PIN_LCD_QSPI_CLK_V1_2 : NRL_PIN_LCD_QSPI_CLK;
}

extern "C" int MosaicoVariant_LcdRstPin(void)
{
    return s_is_v1_2 ? NRL_PIN_LCD_RST_V1_2 : NRL_PIN_LCD_RST;
}

extern "C" int MosaicoVariant_StatusLedPin(void)
{
    // GPIO3 is the v1.0 orange LED but the v1.2 I2C SCL.
    return s_is_v1_2 ? -1 : NRL_PIN_LED_NET;
}

#endif // NRL_BOARD == NRL_BOARD_ESP_MOSAICO
