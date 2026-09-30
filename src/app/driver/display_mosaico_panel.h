#ifndef DRIVER_DISPLAY_MOSAICO_PANEL_H
#define DRIVER_DISPLAY_MOSAICO_PANEL_H

// ESP-Mosaico panel glue: 480x480 QSPI AMOLED (CO5300) power/bus/panel init
// and AMOLED brightness control. Touch (CST9220) lives in display_mosaico.cpp.

#include <stdbool.h>
#include <stdint.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#ifdef __cplusplus
extern "C" {
#endif

// Switch on the VCC_3V3 rail (GPIO60, low active) that feeds the LCD
// connector, then init the QSPI bus + CO5300 panel. Returns the panel
// handle, or nullptr on failure.
esp_lcd_panel_handle_t MosaicoPanel_Init(void);

// AMOLED has no backlight; brightness is the CO5300 0x51 command (0-255).
bool MosaicoPanel_SetBrightness(uint8_t brightness);

// Display on/off (CO5300 0x29/0x28).
bool MosaicoPanel_SetDisplayOn(bool on);

// Wait for the last draw_bitmap's DMA transfer to complete. The LVGL flush
// must call this before lv_display_flush_ready, or LVGL re-renders into the
// buffer while the SPI DMA is still streaming it (garbled pixels).
bool MosaicoPanel_WaitFlushDone(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_DISPLAY_MOSAICO_PANEL_H
