#ifndef DRIVER_FONTS_LV_FONT_CALLSIGN_H
#define DRIVER_FONTS_LV_FONT_CALLSIGN_H

#include "driver/board_pins.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compact 72 px font containing only A-Z, 0-9 and '-'.
extern const lv_font_t lv_font_callsign_72;

#ifdef __cplusplus
}
#endif
#endif

#endif // DRIVER_FONTS_LV_FONT_CALLSIGN_H
