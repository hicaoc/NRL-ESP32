// ESP-Mosaico shared UI context for the full-screen detail pages
// (display_mosaico_sensors.cpp, display_mosaico_ota.cpp). Everything here is
// implemented by display_mosaico.cpp except the detail chrome, which lives in
// display_mosaico_sensors.cpp.
#ifndef DRIVER_DISPLAY_MOSAICO_UI_H
#define DRIVER_DISPLAY_MOSAICO_UI_H

#include <lvgl.h>
#include <stdint.h>

#include "mosaico_sensors.h"

const char *MosaicoUi_Tr(const char *text);
const char *MosaicoUi_CompassPoint(float deg);
const lv_font_t *MosaicoUi_Font(int px);  // 14/16/20/28, CJK fallback attached
lv_obj_t *MosaicoUi_MakeLabel(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color);
bool MosaicoUi_SensorSnapshot(MosaicoSensorSnapshot *out);
void MosaicoUi_ExitSensorDetail(void);    // back to the sensors overview page
void MosaicoUi_ExitOtaDetail(void);       // back to the settings page

// Detail page chrome: transparent full-size non-scrollable container with a
// "< Back" pill (back_cb on click) and a centered title. Implemented by
// display_mosaico_sensors.cpp.
lv_obj_t *MosaicoUi_BuildDetailChrome(lv_obj_t *content, const char *title,
                                      lv_event_cb_t back_cb);

#endif // DRIVER_DISPLAY_MOSAICO_UI_H
