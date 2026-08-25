#include "serial_port_config.h"

#include "board_pins.h"
#include "gps_serial.h"
#include "sci_serial.h"

#include <esp_log.h>
#include <nvs.h>

#include <string.h>

namespace {

constexpr const char *TAG = "SERIAL_IO";
constexpr const char *kNamespace = "serial_io";
constexpr const char *kKey = "config";
constexpr uint32_t kMagic = 0x53494F31u; // SIO1
constexpr uint8_t kVersion = 2u;

struct PersistedConfig {
    uint32_t magic;
    uint8_t version;
    uint8_t board;
    uint8_t uart1_enabled;
    uint8_t uart2_enabled;
    int8_t uart1_rx_pin;
    int8_t uart1_tx_pin;
    int8_t uart2_rx_pin;
    int8_t uart2_tx_pin;
    uint32_t uart2_baud;
    uint8_t uart2_data_bits;
    char uart2_parity;
    uint8_t uart2_stop_bits;
} __attribute__((packed));

SerialPortConfig s_config{};
bool s_initialized = false;

void applyDefaults()
{
#if NRL_BOARD == NRL_BOARD_S31_KORVO || NRL_BOARD == NRL_BOARD_GEZIPAI_4G
    s_config.uart1_enabled = false;
    s_config.uart2_enabled = false;
#elif NRL_BOARD == NRL_BOARD_BI4UMD
    // The board's U0 header is wired to its onboard GPS. Use UART2 on the
    // mapped GPIO44/43 pins so USB Serial/JTAG remains the console.
    s_config.uart1_enabled = false;
    s_config.uart2_enabled = true;
#elif NRL_BOARD == NRL_BOARD_BH4TDV_RF
    // SR-110U uses UART1 on GPIO44/43 while the proven UMD GPS uses UART2 on
    // GPIO2/3. Both ports are fixed board functions and enabled by default.
    s_config.uart1_enabled = true;
    s_config.uart2_enabled = true;
#else
    s_config.uart1_enabled = true;
    s_config.uart2_enabled = true;
#endif
    s_config.uart1_rx_pin = NRL_PIN_SCI_RX;
    s_config.uart1_tx_pin = NRL_PIN_SCI_TX;
    s_config.uart2_rx_pin = NRL_PIN_GPS_RX;
    s_config.uart2_tx_pin = NRL_PIN_GPS_TX;
    s_config.uart2_baud = 9600u;
    s_config.uart2_data_bits = 8u;
    s_config.uart2_parity = 'N';
    s_config.uart2_stop_bits = 1u;
}

bool validFormat(uint32_t baud, uint8_t data_bits, char parity, uint8_t stop_bits)
{
    return baud >= 300u && baud <= 921600u &&
           data_bits >= 5u && data_bits <= 8u &&
           (parity == 'N' || parity == 'E' || parity == 'O') &&
           (stop_bits == 1u || stop_bits == 2u);
}

bool saveConfig()
{
    PersistedConfig blob{};
    blob.magic = kMagic;
    blob.version = kVersion;
    blob.board = static_cast<uint8_t>(NRL_BOARD);
    blob.uart1_enabled = s_config.uart1_enabled ? 1u : 0u;
    blob.uart2_enabled = s_config.uart2_enabled ? 1u : 0u;
    blob.uart1_rx_pin = static_cast<int8_t>(s_config.uart1_rx_pin);
    blob.uart1_tx_pin = static_cast<int8_t>(s_config.uart1_tx_pin);
    blob.uart2_rx_pin = static_cast<int8_t>(s_config.uart2_rx_pin);
    blob.uart2_tx_pin = static_cast<int8_t>(s_config.uart2_tx_pin);
    blob.uart2_baud = s_config.uart2_baud;
    blob.uart2_data_bits = s_config.uart2_data_bits;
    blob.uart2_parity = s_config.uart2_parity;
    blob.uart2_stop_bits = s_config.uart2_stop_bits;

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t set_err = nvs_set_blob(handle, kKey, &blob, sizeof(blob));
    const esp_err_t commit_err = set_err == ESP_OK ? nvs_commit(handle) : set_err;
    nvs_close(handle);
    return set_err == ESP_OK && commit_err == ESP_OK;
}

} // namespace

extern "C" bool SERIAL_PORT_CONFIG_IsAllowedPin(const int gpio)
{
#if NRL_BOARD == NRL_BOARD_BH4TDV
    switch (gpio) {
        case 3: case 4: case 5: case 6: case 7: case 15: case 16:
        case 22: case 23: case 24: case 25: case 33: case 34: case 35:
        case 36: case 37: case 41: case 43: case 44: case 47: case 48:
            return true;
        default:
            return false;
    }
#elif NRL_BOARD == NRL_BOARD_GEZIPAI_4G
    switch (gpio) {
        case 10: case 11: case 12: case 14:
        case 22: case 23: case 24: case 25: case 33: case 34: case 35:
        case 36: case 37: case 43:
            return true;
        default:
            return false;
    }
#elif NRL_BOARD == NRL_BOARD_GEZIPAI
    switch (gpio) {
        case 8: case 9: case 10: case 11: case 12: case 13: case 14:
        case 22: case 23: case 24: case 25: case 33: case 34: case 35:
        case 36: case 37: case 43: case 44:
            return true;
        default:
            return false;
    }
#elif NRL_BOARD_IS_BI4UMD_FAMILY
    switch (gpio) {
        case 2: case 3: case 19: case 20: case 22: case 23: case 24: case 25:
        case 33: case 34: case 35: case 36: case 37: case 43: case 44:
            return true;
        default:
            return false;
    }
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    return (gpio >= 0 && gpio <= 4) || (gpio >= 20 && gpio <= 25) ||
           (gpio >= 33 && gpio <= 49);
#elif NRL_BOARD == NRL_BOARD_S31_KORVO
    // The DVP camera is unused by this firmware and exposes these GPIOs.
    return gpio >= 46 && gpio <= 57;
#else
    (void)gpio;
    return false;
#endif
}

extern "C" bool SERIAL_PORT_CONFIG_Validate(const SerialPortConfig *config)
{
    if (config == nullptr ||
#if NRL_BOARD == NRL_BOARD_GEZIPAI_4G
        config->uart1_enabled ||
#endif
        !validFormat(config->uart2_baud, config->uart2_data_bits,
                     config->uart2_parity, config->uart2_stop_bits)) {
        return false;
    }
    if (config->uart1_enabled &&
        (!SERIAL_PORT_CONFIG_IsAllowedPin(config->uart1_rx_pin) ||
         !SERIAL_PORT_CONFIG_IsAllowedPin(config->uart1_tx_pin) ||
         config->uart1_rx_pin == config->uart1_tx_pin)) {
        return false;
    }
    if (config->uart2_enabled) {
        // GPS/NMEA receivers commonly expose TX only, so UART2 needs a valid
        // ESP RX pin but its TX output may remain unassigned (-1).
        if (!SERIAL_PORT_CONFIG_IsAllowedPin(config->uart2_rx_pin) ||
            (config->uart2_tx_pin != -1 &&
             !SERIAL_PORT_CONFIG_IsAllowedPin(config->uart2_tx_pin)) ||
            (config->uart2_tx_pin >= 0 &&
             config->uart2_rx_pin == config->uart2_tx_pin)) {
            return false;
        }
    }
    if (config->uart1_enabled && config->uart2_enabled) {
        const int uart1_pins[] = {config->uart1_rx_pin, config->uart1_tx_pin};
        const int uart2_pins[] = {config->uart2_rx_pin, config->uart2_tx_pin};
        for (const int uart1_pin : uart1_pins) {
            for (const int uart2_pin : uart2_pins) {
                if (uart2_pin >= 0 && uart1_pin == uart2_pin) return false;
            }
        }
    }
    return true;
}

extern "C" void SERIAL_PORT_CONFIG_Init(void)
{
    if (s_initialized) return;
    applyDefaults();

    PersistedConfig blob{};
    size_t size = sizeof(blob);
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
        const esp_err_t err = nvs_get_blob(handle, kKey, &blob, &size);
        nvs_close(handle);
        if (err == ESP_OK && size == sizeof(blob) && blob.magic == kMagic &&
            blob.version == kVersion && blob.board == static_cast<uint8_t>(NRL_BOARD)) {
            SerialPortConfig loaded{
                blob.uart1_enabled != 0u, blob.uart2_enabled != 0u,
                blob.uart1_rx_pin, blob.uart1_tx_pin,
                blob.uart2_rx_pin, blob.uart2_tx_pin,
                blob.uart2_baud, blob.uart2_data_bits,
                blob.uart2_parity, blob.uart2_stop_bits,
            };
#if NRL_BOARD == NRL_BOARD_BI4UMD
            // Older BI4UMD builds persisted UART1 as unassigned. Migrate only
            // that legacy value to the board's confirmed RX=2/TX=3 mapping.
            if (loaded.uart1_rx_pin < 0 && loaded.uart1_tx_pin < 0) {
                loaded.uart1_rx_pin = NRL_PIN_SCI_RX;
                loaded.uart1_tx_pin = NRL_PIN_SCI_TX;
            }
#endif
            if (SERIAL_PORT_CONFIG_Validate(&loaded)) s_config = loaded;
        }
    }
    s_initialized = true;
}

extern "C" void SERIAL_PORT_CONFIG_Get(SerialPortConfig *out)
{
    if (out == nullptr) return;
    SERIAL_PORT_CONFIG_Init();
    *out = s_config;
}

extern "C" bool SERIAL_PORT_CONFIG_Set(const SerialPortConfig *config, const bool persist)
{
    SERIAL_PORT_CONFIG_Init();
    if (!SERIAL_PORT_CONFIG_Validate(config)) return false;
    const SerialPortConfig old = s_config;
    s_config = *config;
    if (persist && !saveConfig()) {
        s_config = old;
        return false;
    }
    ESP_LOGI(TAG, "UART1 %s rx=%d tx=%d; UART2 %s rx=%d tx=%d %lu,%u,%c,%u",
             s_config.uart1_enabled ? "on" : "off",
             s_config.uart1_rx_pin, s_config.uart1_tx_pin,
             s_config.uart2_enabled ? "on" : "off",
             s_config.uart2_rx_pin, s_config.uart2_tx_pin,
             static_cast<unsigned long>(s_config.uart2_baud),
             static_cast<unsigned>(s_config.uart2_data_bits),
             s_config.uart2_parity, static_cast<unsigned>(s_config.uart2_stop_bits));
    return true;
}

extern "C" bool SERIAL_PORT_CONFIG_ReloadDrivers(void)
{
    // Stop both first. This matters when one UART takes GPIOs previously used
    // by the other UART in the same configuration transaction.
    SCI_SERIAL_Stop();
    GPS_SERIAL_Stop();

    const bool sci_ok = SCI_SERIAL_Init();
    const bool gps_ok = GPS_SERIAL_Init();
    if (!sci_ok || !gps_ok) {
        SCI_SERIAL_Stop();
        GPS_SERIAL_Stop();
        ESP_LOGE(TAG, "UART reload failed: UART1=%s UART2=%s",
                 sci_ok ? "ok" : "failed", gps_ok ? "ok" : "failed");
        return false;
    }
    return true;
}
