#ifndef DRIVER_ENVIRONMENT_SENSORS_H
#define DRIVER_ENVIRONMENT_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

struct EnvironmentSensorSnapshot {
    bool bmp280_present;
    bool bmp280_valid;
    float temperature_c;
    float pressure_hpa;

    bool aht20_available;
    bool aht20_valid;
    float humidity_percent;

    bool bh1750_present;
    bool bh1750_valid;
    float illuminance_lux;

    bool qmc5883l_present;
    bool qmc5883l_valid;
    bool compass_calibrated;
    float magnetic_x_ut;
    float magnetic_y_ut;
    float magnetic_z_ut;
    float heading_deg;

    uint32_t updated_ms;
};

// Starts the BH4TDV-RF sensor worker. Other boards provide harmless stubs.
bool ENV_SENSORS_Init(void);
bool ENV_SENSORS_GetSnapshot(EnvironmentSensorSnapshot *snapshot);

#endif // DRIVER_ENVIRONMENT_SENSORS_H
