// ESP-Mosaico sensor detail pages (compass, bubble level, gyro history,
// magnetometer gauge, battery gauge). display_mosaico.cpp builds these into
// the shared content container when a sensors-overview card is tapped; this
// header is the contract between the two translation units.
#ifndef DRIVER_DISPLAY_MOSAICO_SENSORS_H
#define DRIVER_DISPLAY_MOSAICO_SENSORS_H

#include <lvgl.h>
#include <stdint.h>

#include "mosaico_sensors.h"

// Detail page ids, matching the tappable cards on the sensors overview.
enum MosaicoSensorDetailId {
    MOSAICO_SENSOR_DETAIL_ACCEL = 0,
    MOSAICO_SENSOR_DETAIL_GYRO,
    MOSAICO_SENSOR_DETAIL_HEADING,
    MOSAICO_SENSOR_DETAIL_MAG,
    MOSAICO_SENSOR_DETAIL_BATTERY,
    MOSAICO_SENSOR_DETAIL_COUNT
};

// ---- Shared UI context, implemented by display_mosaico.cpp -----------------

const char *MosaicoUi_Tr(const char *text);
const char *MosaicoUi_CompassPoint(float deg);
const lv_font_t *MosaicoUi_Font(int px);  // 14/16/20/28, CJK fallback attached
lv_obj_t *MosaicoUi_MakeLabel(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color);
bool MosaicoUi_SensorSnapshot(MosaicoSensorSnapshot *out);
void MosaicoUi_ExitSensorDetail(void);  // back to the sensors overview page

// ---- Detail pages, implemented by display_mosaico_sensors.cpp --------------

void MosaicoSensorDetail_Build(lv_obj_t *content, int sensor);
void MosaicoSensorDetail_Refresh(void);
uint32_t MosaicoSensorDetail_RefreshMs(void);

#endif // DRIVER_DISPLAY_MOSAICO_SENSORS_H
