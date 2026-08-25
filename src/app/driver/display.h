#ifndef DRIVER_DISPLAY_H
#define DRIVER_DISPLAY_H

// On-device LCD UI for the 格子派 board (ST7789 240x240 SPI panel + LVGL).
// The panel/wiring matches the 小智 (xiaozhi) 格子派 board. On boards without
// a display these calls are compiled to no-ops, so callers may invoke them
// unconditionally or behind `#if NRL_HAS_DISPLAY`.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the SPI bus, ST7789 panel and LVGL, then build the UI. Safe to call
// once from setup(); a failure is logged and leaves Display_Poll() inert.
void Display_Init(void);

// Select the lightweight provisioning UI. This may be called before
// Display_Init(); once the display is ready it rebuilds the active screen.
// Provisioning mode deliberately avoids polling audio/APRS/storage services
// that have not been started yet.
void Display_SetProvisioningMode(bool enabled);

// Refresh the on-screen values (caller callsign/SSID, clock, WiFi signal,
// battery voltage, IP address). Call periodically from loop(); the work is
// internally throttled so a tight 20 ms poll loop is fine.
void Display_Poll(void);

// Gezipai's non-touch on-device menu. These entry points are intentionally
// input-only: the button task queues navigation/confirm events and LVGL
// consumes them later from Display_Poll(), so no display calls happen from
// the audio/status task. direction: +1 = up, -1 = down.
void Display_MenuOpen(void);
bool Display_MenuIsActive(void);
void Display_MenuNavigate(int direction);
void Display_MenuConfirm(void);
enum DisplayHardwareKey {
    DISPLAY_HW_KEY_UP = 0,
    DISPLAY_HW_KEY_DOWN,
    DISPLAY_HW_KEY_CONFIRM,
    DISPLAY_HW_KEY_SOFT_LEFT,
    DISPLAY_HW_KEY_SOFT_RIGHT,
};
// Thread-safe event injection from board-key polling. LVGL consumes the event
// later from Display_Poll() in the display task.
void Display_HardwareKeyPress(enum DisplayHardwareKey key);
bool Display_CwIsActive(void);
void Display_CwExit(void);

// Battery sense (Geizipai/BI4UMD; returns 0 on boards without a battery ADC).
// Both readings are in millivolts. The raw reading is the uncalibrated ADC
// voltage * 3 (divider); the calibrated reading additionally multiplies by
// the configured battery_cal_milli / 1000 correction factor.
// These take a fresh sample, so callers do not have to wait for the next
// Display_Poll() refresh.
int Display_GetBatteryRawMv(void);
int Display_GetBatteryCalibratedMv(void);

// Diagnostics (S31 only; -1 elsewhere): memset throughput of the RGB
// panel's framebuffer in MB/s. Distinguishes "framebuffer memory is slow /
// uncached" from "the renderer is slow". Briefly corrupts the screen, then
// repaints.
long Display_FramebufferBenchMBps(void);

// CJK font engine on the S31 LCD (no-ops elsewhere; the setter returns
// false). 0 = built-in bitmap subset (GB2312 level-1), 1 = FreeType vector
// rendering from the TTF at /sdcard/fonts/cjk.ttf. Runtime-switchable for
// comparison; the active page is rebuilt on switch.
#define DISPLAY_CJK_FONT_BITMAP   0
#define DISPLAY_CJK_FONT_FREETYPE 1
bool Display_SetCjkFontEngine(int engine);
int Display_GetCjkFontEngine(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_DISPLAY_H
