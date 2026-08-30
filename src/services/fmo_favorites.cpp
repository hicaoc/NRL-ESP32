#include "services/fmo_favorites.h"

#include "services/config_notify.h"
#include "services/fmo_favorites_core.h"

#include "lib/nrl_psram.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

static const char *TAG = "FMOFAV";

namespace {

// Callers arrive from the web server and LVGL tasks; the lock covers list
// mutation and copy-out so a Remove can't shift entries under a reader.
SemaphoreHandle_t s_lock = nullptr;
NRL_PSRAM_BSS static FmoFavoriteList s_list = {};

constexpr const char *kNvsNamespace = "fmofav";

void lock(void)
{
    if (s_lock != nullptr) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void unlock(void)
{
    if (s_lock != nullptr) {
        (void)xSemaphoreGive(s_lock);
    }
}

// Persist under the lock. Count rides alongside the blob so a partial write
// can't pair a stale count with new entries.
void save_locked(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "favorites persist failed");
        return;
    }
    (void)nvs_set_u8(nvs, "count", static_cast<uint8_t>(s_list.count));
    (void)nvs_set_blob(nvs, "list", s_list.entries,
                       s_list.count * sizeof(FmoFavorite));
    (void)nvs_commit(nvs);
    nvs_close(nvs);
    CONFIG_NOTIFY_Bump();
}

} // namespace

extern "C" void FMO_FAV_Init(void)
{
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
    }
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t count = 0;
    if (nvs_get_u8(nvs, "count", &count) == ESP_OK && count <= FMO_FAV_MAX) {
        size_t blob_size = count * sizeof(FmoFavorite);
        if (count == 0u ||
            nvs_get_blob(nvs, "list", s_list.entries, &blob_size) == ESP_OK) {
            s_list.count = count;
        }
    }
    nvs_close(nvs);
    // NUL-terminate defensively in case the blob came from a future layout.
    for (size_t i = 0; i < s_list.count; ++i) {
        FmoFavorite &entry = s_list.entries[i];
        entry.name[FMO_SERVER_NAME_MAX - 1] = '\0';
        entry.host[FMO_SERVER_HOST_MAX - 1] = '\0';
        entry.callsign[FMO_SERVER_CALLSIGN_MAX - 1] = '\0';
    }
    ESP_LOGI(TAG, "%u favorite FMO servers loaded", static_cast<unsigned>(s_list.count));
}

extern "C" size_t FMO_FAV_Count(void)
{
    return s_list.count;
}

extern "C" bool FMO_FAV_Get(const size_t index, FmoFavorite *out)
{
    if (out == nullptr) {
        return false;
    }
    lock();
    const bool ok = index < s_list.count;
    if (ok) {
        *out = s_list.entries[index];
    }
    unlock();
    return ok;
}

extern "C" bool FMO_FAV_Add(const FmoFavorite *entry)
{
    lock();
    const bool ok = FMO_FAV_CORE_Add(&s_list, entry);
    if (ok) {
        save_locked();
    }
    unlock();
    return ok;
}

extern "C" bool FMO_FAV_Remove(const size_t index)
{
    lock();
    const bool ok = FMO_FAV_CORE_Remove(&s_list, index);
    if (ok) {
        save_locked();
    }
    unlock();
    return ok;
}

extern "C" int FMO_FAV_Find(const uint32_t uid, const char *host, const uint16_t port)
{
    lock();
    const int found = FMO_FAV_CORE_Find(&s_list, uid, host, port);
    unlock();
    return found;
}
