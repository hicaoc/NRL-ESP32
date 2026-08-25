#include "sr110u.h"

#include "bh4tdv_rf_io.h"
#include "board_pins.h"
#include "sci_serial.h"

#if NRL_BOARD == NRL_BOARD_BH4TDV_RF

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr const char *TAG = "SR110U";
SemaphoreHandle_t s_mutex = nullptr;
bool s_ready = false;
char s_version[32] = {};

uint32_t millisNow()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void drainInput()
{
    uint8_t scratch[64];
    while (SCI_SERIAL_Available() > 0) {
        if (SCI_SERIAL_Read(scratch, sizeof(scratch)) == 0u) break;
    }
}

bool commandLocked(const char *command, char *response, const size_t response_size,
                   const unsigned timeout_ms)
{
    if (command == nullptr || command[0] == '\0' || response == nullptr || response_size < 2u) {
        return false;
    }
    char wire[128];
    const int length = snprintf(wire, sizeof(wire), "%s\r\n", command);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(wire)) return false;

    drainInput();
    if (SCI_SERIAL_Write(reinterpret_cast<const uint8_t *>(wire),
                         static_cast<size_t>(length)) != static_cast<size_t>(length)) {
        return false;
    }

    size_t used = 0u;
    const uint32_t start = millisNow();
    while (millisNow() - start < timeout_ms && used + 1u < response_size) {
        const size_t got = SCI_SERIAL_Read(reinterpret_cast<uint8_t *>(response + used),
                                           response_size - used - 1u);
        if (got == 0u) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        used += got;
        response[used] = '\0';
        if (strstr(response, "\r\n") != nullptr) return true;
    }
    response[used] = '\0';
    return used != 0u;
}

} // namespace

bool SR110U_Command(const char *command, char *response, const size_t response_size,
                    const unsigned timeout_ms)
{
    if (s_mutex == nullptr) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms + 100u)) != pdTRUE) return false;
    const bool ok = commandLocked(command, response, response_size, timeout_ms);
    xSemaphoreGive(s_mutex);
    return ok;
}

bool SR110U_Init(void)
{
    if (s_ready) return true;
    if (!BH4TDV_RF_IO_Init() || !BH4TDV_RF_IO_SetRadioPtt(false) ||
        !BH4TDV_RF_IO_SetLowPower(true) || !BH4TDV_RF_IO_SetRadioPower(true)) {
        ESP_LOGE(TAG, "radio power-up failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(550));
    if (!SCI_SERIAL_Init()) {
        ESP_LOGE(TAG, "UART initialization failed");
        return false;
    }

    char response[64] = {};
    if (!SR110U_Command("AT+DMOCONT", response, sizeof(response), 500u) ||
        strstr(response, ":0") == nullptr) {
        ESP_LOGE(TAG, "handshake failed: %s", response);
        return false;
    }
    response[0] = '\0';
    if (SR110U_Command("AT+DMOVERQ", response, sizeof(response), 500u)) {
        snprintf(s_version, sizeof(s_version), "%s", response);
    }
    s_ready = true;
    ESP_LOGI(TAG, "ready: %s", s_version[0] != '\0' ? s_version : "version unknown");
    return true;
}

bool SR110U_IsReady(void) { return s_ready; }

int SR110U_ReadRssi(void)
{
    if (!s_ready && !SR110U_Init()) return -1;
    char response[48] = {};
    if (!SR110U_Command("AT+DMORSSI", response, sizeof(response), 300u)) return -1;
    const char *colon = strchr(response, ':');
    if (colon == nullptr) return -1;
    const int value = atoi(colon + 1);
    return value >= 0 && value <= 127 ? value : -1;
}

#else

bool SR110U_Init(void) { return true; }
bool SR110U_IsReady(void) { return false; }
bool SR110U_Command(const char *, char *, size_t, unsigned) { return false; }
int SR110U_ReadRssi(void) { return -1; }

#endif
