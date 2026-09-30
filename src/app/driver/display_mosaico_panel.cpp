// ESP-Mosaico panel glue: 480x480 QSPI AMOLED (CO5300).
//
// Power sequence: VCC_PW (GPIO60, low active) switches the VCC_3V3 rail that
// feeds the LCD connector; LCD_RST and the CST9220 touch reset share GPIO42,
// so the panel reset pulse doubles as the touch reset. Brightness is the
// CO5300 0x51 command -- AMOLED pixels emit directly, there is no backlight.

#include "display_mosaico_panel.h"

#include "board_pins.h"
#include "mosaico_variant.h"

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_lcd_co5300.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

const char *kTag = "MOSAICO_PANEL";

// Boot-stage progress for the CDC-console status dump (display_mosaico.cpp
// prints it): 1 rail on, 2 bus up, 3 io, 4 panel created, 5 reset done,
// 6 init done, 7 display on; display side adds 10 lvgl, 11 touch, 12 ui,
// 13 first render.
extern "C" {
volatile int g_mosaico_boot_stage = 0;
}

// Vendor init sequence from the official ESP-Mosaico BSP (display.c): the
// component's built-in default targets a different panel (466x466 with an
// x-gap) and leaves this 480x480 module black. Sleep-Out comes first with a
// long settle, then the AMOLED power/scan registers, full 480x480 window.
const co5300_lcd_init_cmd_t kVendorInit[] = {
    {0x11, nullptr, 0, 600},                            // Sleep Out
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},                    // RGB565
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},                    // brightness max
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},  // columns 0..479
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},  // rows 0..479
    {0x29, nullptr, 0, 600},                            // Display On
};

esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_panel_io_handle_t s_panel_io = nullptr;
// The esp_lcd SPI IO fires on_color_trans_done once per draw_bitmap (the
// driver marks only the last chunk). The LVGL flush must wait for it before
// reporting flush_ready, otherwise LVGL re-renders into the buffer while the
// DMA is still streaming it -> garbled pixels on small text updates.
SemaphoreHandle_t s_trans_done = nullptr;
StaticSemaphore_t s_trans_done_buf = {};

bool transDoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *,
                 void *)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

void panelRailPowerOn()
{
    gpio_reset_pin((gpio_num_t)NRL_PIN_LCD_PWR_EN);
    gpio_set_direction((gpio_num_t)NRL_PIN_LCD_PWR_EN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)NRL_PIN_LCD_PWR_EN, NRL_PIN_LCD_PWR_EN_ACTIVE_LEVEL);
    // Give the rail and the panel's own power-up sequence time to settle.
    vTaskDelay(pdMS_TO_TICKS(50));
}

} // namespace

extern "C" esp_lcd_panel_handle_t MosaicoPanel_Init(void)
{
    if (s_panel != nullptr) {
        return s_panel;
    }

    panelRailPowerOn();
    g_mosaico_boot_stage = 1;
    ESP_LOGI(kTag, "rail on, starting QSPI bus");

    // The CO5300_PANEL_*_CONFIG macros use C designated initializers that trip
    // -Werror=missing-field-initializers in C++; build the configs field by
    // field instead.
    // LCD SCL/RST differ between hardware revisions (v1.2 swapped them).
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = MosaicoVariant_LcdClkPin();
    bus_cfg.data0_io_num = NRL_PIN_LCD_QSPI_D0;
    bus_cfg.data1_io_num = NRL_PIN_LCD_QSPI_D1;
    bus_cfg.data2_io_num = NRL_PIN_LCD_QSPI_D2;
    bus_cfg.data3_io_num = NRL_PIN_LCD_QSPI_D3;
    bus_cfg.max_transfer_sz = NRL_DISPLAY_WIDTH * 80 * sizeof(uint16_t);
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "QSPI bus init failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    ESP_LOGI(kTag, "bus up");
    g_mosaico_boot_stage = 2;

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = (gpio_num_t)NRL_PIN_LCD_QSPI_CS;
    io_cfg.dc_gpio_num = GPIO_NUM_NC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.quad_mode = true;
    // LVGL's draw buffers live in PSRAM: without this flag the SPI driver would
    // try to bounce the whole 57 KB chunk through internal RAM (which does not
    // fit) and every pixel transfer fails with ESP_ERR_NO_MEM.
    io_cfg.flags.psram_dma_direct = true;
    io_cfg.on_color_trans_done = transDoneCb;
    s_trans_done = xSemaphoreCreateBinaryStatic(&s_trans_done_buf);
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg,
                                 &s_panel_io) != ESP_OK) {
        ESP_LOGE(kTag, "panel IO create failed");
        return nullptr;
    }
    g_mosaico_boot_stage = 3;

    co5300_vendor_config_t vendor_cfg = {};
    vendor_cfg.init_cmds = kVendorInit;
    vendor_cfg.init_cmds_size = sizeof(kVendorInit) / sizeof(kVendorInit[0]);
    vendor_cfg.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = (gpio_num_t)MosaicoVariant_LcdRstPin();
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor_cfg;
    if (esp_lcd_new_panel_co5300(s_panel_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(kTag, "CO5300 create failed");
        return nullptr;
    }
    g_mosaico_boot_stage = 4;
    ESP_LOGI(kTag, "reset+init (vendor sequence, ~1.3s)...");
    if (esp_lcd_panel_reset(s_panel) != ESP_OK) {
        ESP_LOGE(kTag, "CO5300 reset failed");
        return nullptr;
    }
    g_mosaico_boot_stage = 5;
    ESP_LOGI(kTag, "reset done");
    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(kTag, "CO5300 init failed");
        return nullptr;
    }
    g_mosaico_boot_stage = 6;
    ESP_LOGI(kTag, "init done");
    esp_lcd_panel_disp_on_off(s_panel, true);
    g_mosaico_boot_stage = 7;
    ESP_LOGI(kTag, "display on");

    ESP_LOGI(kTag, "CO5300 QSPI AMOLED ready (%dx%d)", NRL_DISPLAY_WIDTH,
             NRL_DISPLAY_HEIGHT);
    return s_panel;
}

extern "C" bool MosaicoPanel_SetBrightness(const uint8_t brightness)
{
    if (s_panel == nullptr) {
        return false;
    }
    // Driver API takes percent; the UI slider works in 0-255.
    const uint8_t percent =
        static_cast<uint8_t>((static_cast<unsigned>(brightness) * 100u + 127u) / 255u);
    return esp_lcd_panel_co5300_set_brightness(s_panel, percent) == ESP_OK;
}

extern "C" bool MosaicoPanel_SetDisplayOn(const bool on)
{
    if (s_panel == nullptr) {
        return false;
    }
    return esp_lcd_panel_disp_on_off(s_panel, on) == ESP_OK;
}

extern "C" bool MosaicoPanel_WaitFlushDone(const uint32_t timeout_ms)
{
    if (s_trans_done == nullptr) {
        return false;
    }
    return xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

#endif // NRL_BOARD == NRL_BOARD_ESP_MOSAICO
