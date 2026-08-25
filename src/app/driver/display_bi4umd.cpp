#include "display_bi4umd.h"

#include "board_pins.h"

#if NRL_BOARD_IS_BI4UMD_FAMILY

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_rom_sys.h>

namespace {

constexpr int kDrawBufferLines = 60;
constexpr uint8_t kCmdSwReset = 0x01;
constexpr uint8_t kCmdColumnAddress = 0x2A;
constexpr uint8_t kCmdPageAddress = 0x2B;
constexpr uint8_t kCmdMemoryWrite = 0x2C;

struct InitCommand {
    uint8_t command;
    uint8_t data[16];
    uint8_t data_length;
    uint16_t delay_ms;
};

constexpr InitCommand kInitCommands[] = {
    {0xCF, {0x00, 0xC1, 0x30}, 3, 0},
    {0xED, {0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, {0x85, 0x00, 0x78}, 3, 0},
    {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, {0x20}, 1, 0}, {0xEA, {0x00, 0x00}, 2, 0},
    {0xC0, {0x23}, 1, 0}, {0xC1, {0x10}, 1, 0},
    {0xC5, {0x3E, 0x28}, 2, 0}, {0xC7, {0x86}, 1, 0},
    {0x36, {0x00}, 1, 0}, {0x3A, {0x55}, 1, 0},
    {0xB1, {0x00, 0x13}, 2, 0}, {0xB6, {0x0A, 0xA2}, 2, 0},
    {0xF6, {0x09, 0x30, 0x00}, 3, 0}, {0xF2, {0x00}, 1, 0},
    {0x26, {0x01}, 1, 0},
    {0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
            0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15, 0},
    {0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
            0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15, 0},
    {0x21, {}, 0, 0}, {0x11, {}, 0, 120}, {0x29, {}, 0, 20},
};

bool txParameter(esp_lcd_panel_io_handle_t panel_io, uint8_t command,
                 const uint8_t *data, size_t data_length)
{
    return esp_lcd_panel_io_tx_param(panel_io, command, data, data_length) == ESP_OK;
}

} // namespace

bool BI4UMD_Display_Init(esp_lcd_panel_io_handle_t *panel_io)
{
    if (panel_io == nullptr) {
        return false;
    }

    gpio_config_t backlight_config = {};
    backlight_config.pin_bit_mask = 1ULL << NRL_PIN_DISPLAY_BL;
    backlight_config.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&backlight_config) != ESP_OK) {
        return false;
    }
    BI4UMD_Display_SetBacklight(false);

    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = NRL_PIN_DISPLAY_SCLK;
    bus_config.mosi_io_num = NRL_PIN_DISPLAY_MOSI;
    bus_config.miso_io_num = NRL_PIN_DISPLAY_MISO;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz =
        NRL_DISPLAY_WIDTH * kDrawBufferLines * static_cast<int>(sizeof(uint16_t));
    if (spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_CS);
    io_config.dc_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_DC);
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    // The LVGL render buffer lives in PSRAM: DMA straight from it instead of
    // bounce-copying every flush through an internal temporary buffer.
    io_config.flags.psram_dma_direct = 1;
    if (esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, panel_io) != ESP_OK) {
        return false;
    }

    if (!txParameter(*panel_io, kCmdSwReset, nullptr, 0)) {
        return false;
    }
    esp_rom_delay_us(120000);
    for (const InitCommand &step : kInitCommands) {
        const uint8_t *data = step.data_length > 0 ? step.data : nullptr;
        if (!txParameter(*panel_io, step.command, data, step.data_length)) {
            return false;
        }
        if (step.delay_ms > 0) {
            esp_rom_delay_us(static_cast<uint32_t>(step.delay_ms) * 1000U);
        }
    }
    return true;
}

bool BI4UMD_Display_Flush(esp_lcd_panel_io_handle_t panel_io,
                         int x1, int y1, int x2, int y2,
                         uint8_t *pixels, size_t pixel_count)
{
    if (panel_io == nullptr || pixels == nullptr) {
        return false;
    }

    uint16_t *pixel_data = reinterpret_cast<uint16_t *>(pixels);
    for (size_t i = 0; i < pixel_count; ++i) {
        pixel_data[i] = static_cast<uint16_t>((pixel_data[i] >> 8) | (pixel_data[i] << 8));
    }

    const uint8_t columns[] = {
        static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1),
        static_cast<uint8_t>(x2 >> 8), static_cast<uint8_t>(x2),
    };
    const uint8_t rows[] = {
        static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1),
        static_cast<uint8_t>(y2 >> 8), static_cast<uint8_t>(y2),
    };
    if (!txParameter(panel_io, kCmdColumnAddress, columns, sizeof(columns)) ||
        !txParameter(panel_io, kCmdPageAddress, rows, sizeof(rows))) {
        return false;
    }
    return esp_lcd_panel_io_tx_color(panel_io, kCmdMemoryWrite, pixels,
                                     pixel_count * sizeof(uint16_t)) == ESP_OK;
}

void BI4UMD_Display_SetBacklight(bool enabled)
{
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_DISPLAY_BL), enabled ? 1 : 0);
}

#else

bool BI4UMD_Display_Init(esp_lcd_panel_io_handle_t *) { return false; }
bool BI4UMD_Display_Flush(esp_lcd_panel_io_handle_t, int, int, int, int,
                         uint8_t *, size_t) { return false; }
void BI4UMD_Display_SetBacklight(bool) {}

#endif
