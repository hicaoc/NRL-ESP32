#include "environment_sensors.h"

#include "board_pins.h"
#include "i2c1.h"
#include "i2c_device_discovery.h"
#include "sr110u.h"
#include "../../services/radio_config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <math.h>
#include <string.h>

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF

namespace {

constexpr const char *TAG = "ENV";
constexpr uint8_t kBmpIdRegister = 0xD0u;
constexpr uint32_t kCompassPeriodMs = 100u;
constexpr uint32_t kEnvironmentPeriodMs = 2000u;
constexpr uint32_t kSensorRetryPeriodMs = 5000u;
constexpr uint32_t kRssiPollPeriodMs = 1000u;
// When the boot-time module handshake fails (e.g. the radio was still in its
// ~1 s power-up window), re-run the full apply every so often. Each attempt
// can block ~2.5 s, so the period stays well above the sensor cadence.
constexpr uint32_t kRadioRetryPeriodMs = 15000u;

struct BmpCalibration {
    uint16_t t1;
    int16_t t2;
    int16_t t3;
    uint16_t p1;
    int16_t p2;
    int16_t p3;
    int16_t p4;
    int16_t p5;
    int16_t p6;
    int16_t p7;
    int16_t p8;
    int16_t p9;
    // BME280 only: humidity coefficients (id 0x60).
    uint8_t h1;
    int16_t h2;
    uint8_t h3;
    int16_t h4;
    int16_t h5;
    int8_t h6;
};

EnvironmentSensorSnapshot s_snapshot = {};
portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
BmpCalibration s_bmp_cal = {};
bool s_bme280 = false;
bool s_started = false;
uint8_t s_bmp_address = NRL_BMP280_I2C_ADDR;
uint8_t s_qmc_address = NRL_QMC5883L_I2C_ADDR;
uint8_t s_bh_address = NRL_BH1750_I2C_ADDR;

uint16_t u16le(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

int16_t s16le(const uint8_t *p)
{
    return static_cast<int16_t>(u16le(p));
}

bool readRegisters(const uint8_t address, const uint8_t reg,
                   uint8_t *data, const size_t size)
{
    return I2C_MasterTransmitReceive(address, &reg, 1u, data, size, 100);
}

bool writeRegister(const uint8_t address, const uint8_t reg, const uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return I2C_MasterTransmit(address, data, sizeof(data), 100);
}

bool initBmp280At(const uint8_t address)
{
    uint8_t id = 0u;
    if (!I2C_MasterProbe(address, 100) ||
        !readRegisters(address, kBmpIdRegister, &id, 1u)) {
        return false;
    }
    // 0x58 = BMP280, 0x60 = BME280. Both share the temperature/pressure
    // registers and compensation math used below (BME280 humidity is not
    // configured, so it stays in skip mode).
    if (id != 0x58u && id != 0x60u) {
        return false;
    }

    uint8_t raw[24] = {};
    if (!readRegisters(address, 0x88u, raw, sizeof(raw))) return false;
    s_bmp_cal.t1 = u16le(raw + 0);
    s_bmp_cal.t2 = s16le(raw + 2);
    s_bmp_cal.t3 = s16le(raw + 4);
    s_bmp_cal.p1 = u16le(raw + 6);
    s_bmp_cal.p2 = s16le(raw + 8);
    s_bmp_cal.p3 = s16le(raw + 10);
    s_bmp_cal.p4 = s16le(raw + 12);
    s_bmp_cal.p5 = s16le(raw + 14);
    s_bmp_cal.p6 = s16le(raw + 16);
    s_bmp_cal.p7 = s16le(raw + 18);
    s_bmp_cal.p8 = s16le(raw + 20);
    s_bmp_cal.p9 = s16le(raw + 22);
    if (s_bmp_cal.t1 == 0u || s_bmp_cal.p1 == 0u) return false;

    s_bme280 = id == 0x60u;
    if (s_bme280) {
        // BME280 humidity calibration: H1 at 0xA1, H2..H6 at 0xE1-0xE7.
        uint8_t hcal[8] = {};
        if (!readRegisters(address, 0xA1u, hcal, 1u) ||
            !readRegisters(address, 0xE1u, hcal + 1, 7u)) return false;
        s_bmp_cal.h1 = hcal[0];
        s_bmp_cal.h2 = s16le(hcal + 1);
        s_bmp_cal.h3 = hcal[3];
        s_bmp_cal.h4 = static_cast<int16_t>((hcal[4] << 4) | (hcal[5] & 0x0Fu));
        s_bmp_cal.h5 = static_cast<int16_t>((hcal[6] << 4) | (hcal[5] >> 4u));
        s_bmp_cal.h6 = static_cast<int8_t>(hcal[7]);
        // osrs_h x1. ctrl_hum only takes effect when ctrl_meas is written
        // afterwards, which the normal-mode write below does.
        if (!writeRegister(address, 0xF2u, 0x01u)) return false;
    }

    // 1 s standby, IIR x4; temperature x1, pressure x4, normal mode.
    if (!writeRegister(address, 0xF5u, 0xA8u) ||
        !writeRegister(address, 0xF4u, 0x2Fu)) {
        return false;
    }
    s_bmp_address = address;
    ESP_LOGI(TAG, "BMP280/BME280 ready at 0x%02X (id=0x%02X)", address, id);
    return true;
}

bool initBmp280()
{
    const uint8_t discovered = I2C_DEVICE_DISCOVERY_GetAddress(
        I2CDeviceModel::Bmp280, NRL_BMP280_I2C_ADDR);
    // Try the discovered/default strap first, then the other one: a scan that
    // ACKed the chip but failed the chip-ID read must not lock the driver
    // onto a dead address until the next manual rescan.
    const uint8_t alternate = discovered == 0x76u ? 0x77u : 0x76u;
    if (initBmp280At(discovered)) return true;
    if (alternate != discovered && initBmp280At(alternate)) return true;
    ESP_LOGW(TAG, "BMP280 not found at 0x%02X or 0x%02X", discovered, alternate);
    return false;
}

bool readBmp280(float *temperature_c, float *pressure_hpa,
                float *humidity_percent)
{
    // BME280 appends the humidity ADC at 0xFD-0xFE, so burst 8 bytes there.
    uint8_t raw[8] = {};
    const size_t burst = s_bme280 ? sizeof(raw) : 6u;
    if (!readRegisters(s_bmp_address, 0xF7u, raw, burst)) return false;
    const int32_t adc_p = (static_cast<int32_t>(raw[0]) << 12) |
                          (static_cast<int32_t>(raw[1]) << 4) |
                          (static_cast<int32_t>(raw[2]) >> 4);
    const int32_t adc_t = (static_cast<int32_t>(raw[3]) << 12) |
                          (static_cast<int32_t>(raw[4]) << 4) |
                          (static_cast<int32_t>(raw[5]) >> 4);
    if (adc_p == 0x80000 || adc_t == 0x80000) return false;

    const int32_t var1_t = ((((adc_t >> 3) -
        (static_cast<int32_t>(s_bmp_cal.t1) << 1))) *
        static_cast<int32_t>(s_bmp_cal.t2)) >> 11;
    const int32_t var2_t = (((((adc_t >> 4) -
        static_cast<int32_t>(s_bmp_cal.t1)) *
        ((adc_t >> 4) - static_cast<int32_t>(s_bmp_cal.t1))) >> 12) *
        static_cast<int32_t>(s_bmp_cal.t3)) >> 14;
    const int32_t t_fine = var1_t + var2_t;
    const int32_t temp_x100 = (t_fine * 5 + 128) >> 8;

    int64_t var1 = static_cast<int64_t>(t_fine) - 128000;
    int64_t var2 = var1 * var1 * static_cast<int64_t>(s_bmp_cal.p6);
    var2 += (var1 * static_cast<int64_t>(s_bmp_cal.p5)) << 17;
    var2 += static_cast<int64_t>(s_bmp_cal.p4) << 35;
    var1 = ((var1 * var1 * static_cast<int64_t>(s_bmp_cal.p3)) >> 8) +
           ((var1 * static_cast<int64_t>(s_bmp_cal.p2)) << 12);
    var1 = (((static_cast<int64_t>(1) << 47) + var1) *
            static_cast<int64_t>(s_bmp_cal.p1)) >> 33;
    if (var1 == 0) return false;
    int64_t pressure = 1048576 - adc_p;
    pressure = (((pressure << 31) - var2) * 3125) / var1;
    var1 = (static_cast<int64_t>(s_bmp_cal.p9) *
            (pressure >> 13) * (pressure >> 13)) >> 25;
    var2 = (static_cast<int64_t>(s_bmp_cal.p8) * pressure) >> 19;
    pressure = ((pressure + var1 + var2) >> 8) +
               (static_cast<int64_t>(s_bmp_cal.p7) << 4);

    if (s_bme280 && humidity_percent != nullptr) {
        // BME280 datasheet fixed-point humidity compensation.
        const int32_t adc_h = (static_cast<int32_t>(raw[6]) << 8) | raw[7];
        int32_t v = t_fine - 76800;
        v = (((((adc_h << 14) - (static_cast<int32_t>(s_bmp_cal.h4) << 20) -
                (static_cast<int32_t>(s_bmp_cal.h5) * v)) + 16384) >> 15) *
             (((((((v * static_cast<int32_t>(s_bmp_cal.h6)) >> 10) *
                  (((v * static_cast<int32_t>(s_bmp_cal.h3)) >> 11) + 32768)) >> 10) +
                2097152) * static_cast<int32_t>(s_bmp_cal.h2) + 8192) >> 14));
        v = v - (((((v >> 15) * (v >> 15)) >> 7) *
                  static_cast<int32_t>(s_bmp_cal.h1)) >> 4);
        v = v < 0 ? 0 : (v > 419430400 ? 419430400 : v);
        *humidity_percent = static_cast<float>(v >> 12) / 1024.0f;
    }
    *temperature_c = static_cast<float>(temp_x100) / 100.0f;
    *pressure_hpa = static_cast<float>(pressure) / 25600.0f;
    return isfinite(*temperature_c) && isfinite(*pressure_hpa) &&
           *temperature_c > -50.0f && *temperature_c < 100.0f &&
           *pressure_hpa > 300.0f && *pressure_hpa < 1200.0f;
}

bool initQmc5883l()
{
    const uint8_t address = I2C_DEVICE_DISCOVERY_GetAddress(
        I2CDeviceModel::Qmc5883l, NRL_QMC5883L_I2C_ADDR);
    if (!I2C_MasterProbe(address, 100)) return false;
    uint8_t id = 0u;
    if (!readRegisters(address, 0x0Du, &id, 1u)) return false;
    s_qmc_address = address;
    ESP_LOGI(TAG, "QMC5883L address=0x%02X id=0x%02X", address, id);
    // Soft reset, set/reset period, then continuous 50 Hz, 2 G, OSR 512.
    if (!writeRegister(address, 0x0Au, 0x80u)) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    return writeRegister(address, 0x0Bu, 0x01u) &&
           writeRegister(address, 0x09u, 0x09u);
}

bool readQmc5883l(float *x_ut, float *y_ut, float *z_ut, float *heading_deg)
{
    uint8_t status = 0u;
    if (!readRegisters(s_qmc_address, 0x06u, &status, 1u) ||
        (status & 0x01u) == 0u || (status & 0x02u) != 0u) return false;
    uint8_t raw[6] = {};
    if (!readRegisters(s_qmc_address, 0x00u, raw, sizeof(raw))) return false;
    const int16_t x = s16le(raw + 0);
    const int16_t y = s16le(raw + 2);
    const int16_t z = s16le(raw + 4);
    // At the selected +/-2 G range the nominal scale is 12000 LSB/G.
    constexpr float kMicroteslaPerCount = 100.0f / 12000.0f;
    *x_ut = static_cast<float>(x) * kMicroteslaPerCount;
    *y_ut = static_cast<float>(y) * kMicroteslaPerCount;
    *z_ut = static_cast<float>(z) * kMicroteslaPerCount;
    constexpr float kRadiansToDegrees = 57.2957795131f;
    float heading = atan2f(*y_ut, *x_ut) * kRadiansToDegrees;
    if (heading < 0.0f) heading += 360.0f;
    *heading_deg = heading;
    return true;
}

bool initBh1750()
{
    const uint8_t address = I2C_DEVICE_DISCOVERY_GetAddress(
        I2CDeviceModel::Bh1750, NRL_BH1750_I2C_ADDR);
    // Never poke an address whose model is still an unresolved conflict:
    // BH1750 opcodes sent to a PCA9555 strapped at 0x23 would be interpreted
    // as its command/register byte.
    if (I2C_DEVICE_DISCOVERY_GetModelAt(address) ==
        I2CDeviceModel::Pca9555OrBh1750) {
        ESP_LOGW(TAG, "BH1750 init skipped: 0x%02X is an unresolved "
                      "BH1750/PCA9555 conflict", address);
        return false;
    }
    if (!I2C_MasterProbe(address, 100)) return false;
    s_bh_address = address;
    const uint8_t power_on = 0x01u;
    const uint8_t continuous_high_resolution = 0x10u;
    return I2C_MasterTransmit(address, &power_on, 1u, 100) &&
           I2C_MasterTransmit(address, &continuous_high_resolution, 1u, 100);
}

bool readBh1750(float *lux)
{
    uint8_t raw[2] = {};
    if (!I2C_MasterReceive(s_bh_address, raw, sizeof(raw), 100)) return false;
    const uint16_t value = static_cast<uint16_t>(raw[0] << 8u) | raw[1];
    *lux = static_cast<float>(value) / 1.2f;
    return isfinite(*lux) && *lux >= 0.0f;
}

void sensorTask(void *)
{
    EnvironmentSensorSnapshot local = {};
    uint32_t discovery_revision = UINT32_MAX;
    uint32_t last_environment_ms = 0u;
    uint32_t last_retry_ms = 0u;
    uint32_t last_rssi_ms = 0u;
    uint32_t last_radio_retry_ms = 0u;
    for (;;) {
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        const uint32_t current_revision = I2C_DEVICE_DISCOVERY_GetRevision();
        if (current_revision != discovery_revision) {
            discovery_revision = current_revision;
            local = {};
            local.bmp280_present = initBmp280();
            local.qmc5883l_present = initQmc5883l();
            local.bh1750_present = initBh1750();
            // AHT20 is physically unsafe while the touch controller remains
            // on the same fixed 0x38 address.
            local.aht20_available = false;
            last_environment_ms = 0u;
            last_retry_ms = now;
            ESP_LOGW(TAG, "AHT20 disabled: I2C address 0x38 conflicts with touch");
        } else if ((!local.bmp280_present || !local.qmc5883l_present ||
                    !local.bh1750_present) &&
                   now - last_retry_ms >= kSensorRetryPeriodMs) {
            // A sensor that failed init at boot (bus contention, slow power-up)
            // previously stayed dead until the next manual rescan.
            last_retry_ms = now;
            if (!local.bmp280_present) local.bmp280_present = initBmp280();
            if (!local.qmc5883l_present) local.qmc5883l_present = initQmc5883l();
            if (!local.bh1750_present) local.bh1750_present = initBh1750();
        }
        if (local.qmc5883l_present) {
            local.qmc5883l_valid = readQmc5883l(
                &local.magnetic_x_ut, &local.magnetic_y_ut,
                &local.magnetic_z_ut, &local.heading_deg);
        }
        if (last_environment_ms == 0u || now - last_environment_ms >= kEnvironmentPeriodMs) {
            last_environment_ms = now;
            if (local.bmp280_present) {
                local.bmp280_valid = readBmp280(&local.temperature_c,
                                                 &local.pressure_hpa,
                                                 &local.humidity_percent);
                local.bme280_humidity_valid = s_bme280 && local.bmp280_valid;
            }
            if (local.bh1750_present) {
                local.bh1750_valid = readBh1750(&local.illuminance_lux);
            }
            local.updated_ms = now;
        }
        // Feed the cached RF RSSI shown in the status bar and web portal.
        // PollRssi() never powers the module up, so this is safe while the
        // radio is configured off.
        if (now - last_rssi_ms >= kRssiPollPeriodMs) {
            last_rssi_ms = now;
            SR110U_PollRssi();
        }
        // The radio module may have missed its boot-time configuration
        // (handshake raced the power-up window) or rejected a group;
        // dirty groups stay pending, so keep retrying the apply.
        if (RADIO_CONFIG_Get()->enabled &&
            (!SR110U_IsReady() || RADIO_CONFIG_PendingApply()) &&
            now - last_radio_retry_ms >= kRadioRetryPeriodMs) {
            last_radio_retry_ms = now;
            (void)RADIO_CONFIG_ApplyToModule();
        }
        portENTER_CRITICAL(&s_snapshot_lock);
        s_snapshot = local;
        portEXIT_CRITICAL(&s_snapshot_lock);
        vTaskDelay(pdMS_TO_TICKS(kCompassPeriodMs));
    }
}

} // namespace

bool ENV_SENSORS_Init(void)
{
    if (s_started) return true;
    s_started = xTaskCreate(sensorTask, "env_sensors", 3072, nullptr, 3, nullptr) == pdPASS;
    if (!s_started) ESP_LOGE(TAG, "failed to create sensor task");
    return s_started;
}

bool ENV_SENSORS_GetSnapshot(EnvironmentSensorSnapshot *snapshot)
{
    if (snapshot == nullptr) return false;
    portENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return s_started;
}

#else

bool ENV_SENSORS_Init(void) { return true; }
bool ENV_SENSORS_GetSnapshot(EnvironmentSensorSnapshot *snapshot)
{
    if (snapshot != nullptr) memset(snapshot, 0, sizeof(*snapshot));
    return false;
}

#endif
