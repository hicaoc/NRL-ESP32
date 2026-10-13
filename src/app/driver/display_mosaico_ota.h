// ESP-Mosaico OTA (system update) detail page, built into the shared content
// container from the Settings page entry. Same detail-page framework as the
// sensor pages: hidden dock, non-scrollable container, up-swipe/Back to exit.
#ifndef DRIVER_DISPLAY_MOSAICO_OTA_H
#define DRIVER_DISPLAY_MOSAICO_OTA_H

#include "display_mosaico_ui.h"

// Implemented by display_mosaico_ota.cpp.
void MosaicoOtaDetail_Build(lv_obj_t *content);
void MosaicoOtaDetail_Refresh(void);

#endif // DRIVER_DISPLAY_MOSAICO_OTA_H
