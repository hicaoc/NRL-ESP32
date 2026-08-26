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
int s_cached_rssi = -1;

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

bool commandLocked(const uint8_t *data, const size_t data_len,
                   char *response, const size_t response_size,
                   const unsigned timeout_ms)
{
    if (data == nullptr || data_len == 0u ||
        response == nullptr || response_size < 2u) {
        return false;
    }

    drainInput();
    // DMOGRP embeds raw tone bytes, so the trailing CR/LF is appended as a
    // separate write instead of going through a NUL-terminated snprintf.
    if (SCI_SERIAL_Write(data, data_len) != data_len ||
        SCI_SERIAL_Write(reinterpret_cast<const uint8_t *>("\r\n"), 2u) != 2u) {
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

bool SR110U_CommandRaw(const uint8_t *data, const size_t data_len,
                       char *response, const size_t response_size,
                       const unsigned timeout_ms)
{
    if (s_mutex == nullptr) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms + 100u)) != pdTRUE) return false;
    const bool ok = commandLocked(data, data_len, response, response_size, timeout_ms);
    xSemaphoreGive(s_mutex);
    return ok;
}

bool SR110U_Command(const char *command, char *response, const size_t response_size,
                    const unsigned timeout_ms)
{
    if (command == nullptr || command[0] == '\0') return false;
    return SR110U_CommandRaw(reinterpret_cast<const uint8_t *>(command),
                             strlen(command), response, response_size, timeout_ms);
}

void SR110U_NotifyPowerCycled(void)
{
    // The handshake was lost with the module power; the next SR110U_Init()
    // must redo it (and pay the 550 ms boot delay) instead of trusting
    // s_ready.
    s_ready = false;
    s_version[0] = '\0';
    s_cached_rssi = -1;
}

bool SR110U_Init(void)
{
    if (s_ready) return true;
    if (!BH4TDV_RF_IO_Init() || !BH4TDV_RF_IO_SetRadioPtt(false) ||
        !BH4TDV_RF_IO_SetLowPower(true) || !BH4TDV_RF_IO_SetRadioPower(true)) {
        ESP_LOGE(TAG, "radio power-up failed");
        return false;
    }
    // The module ignores parameter commands during roughly the first
    // second after power-up; the previous 550 ms wait raced that window
    // and could fail the handshake, leaving the radio unconfigured.
    vTaskDelay(pdMS_TO_TICKS(1100));
    if (!SCI_SERIAL_Init()) {
        ESP_LOGE(TAG, "UART initialization failed");
        return false;
    }

    char response[64] = {};
    bool handshake = false;
    for (int attempt = 0; attempt < 3 && !handshake; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(300));
        response[0] = '\0';
        handshake = SR110U_Command("AT+DMOCONT", response, sizeof(response), 500u) &&
                    strstr(response, ":0") != nullptr;
    }
    if (!handshake) {
        ESP_LOGE(TAG, "handshake failed: %s", response);
        return false;
    }
    response[0] = '\0';
    if (SR110U_Command("AT+DMOVERQ", response, sizeof(response), 500u)) {
        // Keep only the payload of "+DMOVERQ:110U-V303\r\n".
        const char *version = strchr(response, ':');
        version = version != nullptr ? version + 1 : response;
        snprintf(s_version, sizeof(s_version), "%s", version);
        char *end = s_version + strlen(s_version);
        while (end > s_version &&
               (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) {
            *--end = '\0';
        }
    }
    s_ready = true;
    ESP_LOGI(TAG, "ready: %s", s_version[0] != '\0' ? s_version : "version unknown");
    return true;
}

bool SR110U_IsReady(void) { return s_ready; }

const char *SR110U_GetVersion(void) { return s_version; }

int SR110U_GetCachedRssi(void) { return s_cached_rssi; }

// Datasheet sensitivity table: RSSI register value -> dBm anchors on a
// 10 dB grid. Values between anchors are interpolated linearly; ends clamp
// (the table bottoms out at -120 dBm and tops out at 127 = -20 dBm).
int SR110U_RssiToDbm(const int rssi)
{
    static const int8_t kAnchors[][2] = {
        {0, -120}, {30, -120}, {36, -110}, {46, -100}, {55, -90},
        {65, -80}, {75, -70}, {85, -60}, {94, -50}, {103, -40},
        {115, -30}, {127, -20},
    };
    if (rssi <= kAnchors[0][0]) return kAnchors[0][1];
    for (size_t i = 1u; i < sizeof(kAnchors) / sizeof(kAnchors[0]); ++i) {
        if (rssi <= kAnchors[i][0]) {
            const int x0 = kAnchors[i - 1u][0], y0 = kAnchors[i - 1u][1];
            const int x1 = kAnchors[i][0], y1 = kAnchors[i][1];
            if (x1 == x0) return y1;
            return y0 + (rssi - x0) * (y1 - y0) / (x1 - x0);
        }
    }
    return -20;
}

void SR110U_PollRssi(void)
{
    if (!s_ready) {
        s_cached_rssi = -1;
        return;
    }
    const int value = SR110U_ReadRssi();
    if (value >= 0) s_cached_rssi = value;
}

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
bool SR110U_CommandRaw(const uint8_t *, size_t, char *, size_t, unsigned)
{
    return false;
}
const char *SR110U_GetVersion(void) { return ""; }
int SR110U_GetCachedRssi(void) { return -1; }
int SR110U_RssiToDbm(int rssi) { return rssi >= 0 ? -120 : 0; }
void SR110U_PollRssi(void) {}
void SR110U_NotifyPowerCycled(void) {}
int SR110U_ReadRssi(void) { return -1; }

#endif
