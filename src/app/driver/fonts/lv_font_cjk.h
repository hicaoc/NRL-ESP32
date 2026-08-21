#ifndef DRIVER_FONTS_LV_FONT_CJK_H
#define DRIVER_FONTS_LV_FONT_CJK_H

// Generated CJK bitmap fonts (scripts/gen_cjk_font.py): GB2312 level-1
// hanzi + CJK punctuation, Noto Sans SC. The 16px font is also compiled for
// gezipai's APRS ticker; S31 additionally uses the 20px font throughout its UI.

#include "driver/board_pins.h"

#if NRL_BOARD == NRL_BOARD_S31_KORVO || NRL_BOARD_IS_GEZIPAI_FAMILY

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t lv_font_cjk_16;
#if NRL_BOARD == NRL_BOARD_S31_KORVO
extern const lv_font_t lv_font_cjk_20;
#endif

#ifdef __cplusplus
}
#endif

#endif // S31_KORVO || GEZIPAI

#endif // DRIVER_FONTS_LV_FONT_CJK_H
