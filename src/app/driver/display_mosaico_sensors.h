// ESP-Mosaico sensor detail pages (compass, bubble level, gyro history,
// magnetometer gauge, battery gauge). display_mosaico.cpp builds these into
// the shared content container when a sensors-overview card is tapped; this
// header is the contract between the two translation units.
#ifndef DRIVER_DISPLAY_MOSAICO_SENSORS_H
#define DRIVER_DISPLAY_MOSAICO_SENSORS_H

#include "display_mosaico_ui.h"

// Detail page ids, matching the tappable cards on the sensors overview.
enum MosaicoSensorDetailId {
    MOSAICO_SENSOR_DETAIL_ACCEL = 0,
    MOSAICO_SENSOR_DETAIL_GYRO,
    MOSAICO_SENSOR_DETAIL_HEADING,
    MOSAICO_SENSOR_DETAIL_MAG,
    MOSAICO_SENSOR_DETAIL_BATTERY,
    MOSAICO_SENSOR_DETAIL_COUNT
};

// Implemented by display_mosaico_sensors.cpp.
void MosaicoSensorDetail_Build(lv_obj_t *content, int sensor);
void MosaicoSensorDetail_Refresh(void);
uint32_t MosaicoSensorDetail_RefreshMs(void);

#endif // DRIVER_DISPLAY_MOSAICO_SENSORS_H
