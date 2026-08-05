#include "services/radio_favorites.h"

#include "services/config_notify.h"
#include "services/music_player.h"

#include "lib/nrl_psram.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

#include <stdio.h>
#include <string.h>

static const char *TAG = "RADIOFAV";

namespace {

struct FavEntry {
    char name[RADIO_FAV_NAME_SIZE];
    char url[RADIO_FAV_URL_SIZE];
};

// Callers arrive from the web server, AT handler, and LVGL tasks; the lock
// covers list mutation and copy-out so a Remove can't shift entries under a
// concurrent reader. Playback itself runs outside the lock.
static SemaphoreHandle_t s_lock = nullptr;
NRL_PSRAM_BSS static FavEntry s_entries[RADIO_FAV_MAX] = {};
static size_t s_count = 0;
static int s_current = -1;

constexpr const char *kNvsNamespace = "radiofav";

static void lock(void)
{
    if (s_lock != nullptr) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != nullptr) {
        (void)xSemaphoreGive(s_lock);
    }
}

static bool url_valid(const char *url)
{
    return url != nullptr &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) &&
           strlen(url) < RADIO_FAV_URL_SIZE;
}

// Persist under the lock. Count and current index ride alongside the fixed
// blob so a partial write can't pair a stale count with new entries.
static void save_locked(void)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "favorites persist failed");
        return;
    }
    (void)nvs_set_u8(nvs, "count", static_cast<uint8_t>(s_count));
    (void)nvs_set_i8(nvs, "cur", static_cast<int8_t>(s_current));
    (void)nvs_set_blob(nvs, "list", s_entries, s_count * sizeof(FavEntry));
    (void)nvs_commit(nvs);
    nvs_close(nvs);
    CONFIG_NOTIFY_Bump();
}

} // namespace

extern "C" void RADIO_FAV_Init(void)
{
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
    }
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t count = 0;
    if (nvs_get_u8(nvs, "count", &count) == ESP_OK && count <= RADIO_FAV_MAX) {
        size_t blob_size = count * sizeof(FavEntry);
        if (count == 0u ||
            nvs_get_blob(nvs, "list", s_entries, &blob_size) == ESP_OK) {
            s_count = count;
        }
    }
    int8_t current = -1;
    if (nvs_get_i8(nvs, "cur", &current) == ESP_OK &&
        current >= 0 && static_cast<size_t>(current) < s_count) {
        s_current = current;
    }
    nvs_close(nvs);
    // NUL-terminate defensively in case the blob came from a future layout.
    for (size_t i = 0; i < s_count; ++i) {
        s_entries[i].name[RADIO_FAV_NAME_SIZE - 1] = '\0';
        s_entries[i].url[RADIO_FAV_URL_SIZE - 1] = '\0';
    }
    ESP_LOGI(TAG, "%u favorite stations loaded", static_cast<unsigned>(s_count));
}

extern "C" size_t RADIO_FAV_Count(void)
{
    return s_count;
}

extern "C" bool RADIO_FAV_Get(const size_t index,
                              char *name, const size_t name_size,
                              char *url, const size_t url_size)
{
    lock();
    const bool ok = index < s_count;
    if (ok) {
        if (name != nullptr && name_size > 0u) {
            snprintf(name, name_size, "%s", s_entries[index].name);
        }
        if (url != nullptr && url_size > 0u) {
            snprintf(url, url_size, "%s", s_entries[index].url);
        }
    }
    unlock();
    return ok;
}

extern "C" bool RADIO_FAV_Set(const int index, const char *name, const char *url, int *out_index)
{
    if (!url_valid(url) ||
        (name != nullptr && strlen(name) >= RADIO_FAV_NAME_SIZE)) {
        return false;
    }
    lock();
    size_t slot;
    if (index < 0) {
        if (s_count >= RADIO_FAV_MAX) {
            unlock();
            return false;
        }
        slot = s_count++;
    } else if (static_cast<size_t>(index) < s_count) {
        slot = static_cast<size_t>(index);
    } else {
        unlock();
        return false;
    }
    snprintf(s_entries[slot].name, sizeof(s_entries[slot].name), "%s",
             (name != nullptr && name[0] != '\0') ? name : url);
    snprintf(s_entries[slot].url, sizeof(s_entries[slot].url), "%s", url);
    save_locked();
    unlock();
    if (out_index != nullptr) {
        *out_index = static_cast<int>(slot);
    }
    return true;
}

extern "C" bool RADIO_FAV_Remove(const size_t index)
{
    lock();
    if (index >= s_count) {
        unlock();
        return false;
    }
    memmove(&s_entries[index], &s_entries[index + 1],
            (s_count - index - 1u) * sizeof(FavEntry));
    --s_count;
    if (s_current == static_cast<int>(index)) {
        s_current = -1;
    } else if (s_current > static_cast<int>(index)) {
        --s_current;
    }
    save_locked();
    unlock();
    return true;
}

extern "C" int RADIO_FAV_CurrentIndex(void)
{
    return s_current;
}

extern "C" int RADIO_FAV_IndexOfUrl(const char *url)
{
    if (url == nullptr || url[0] == '\0') {
        return -1;
    }
    lock();
    int found = -1;
    for (size_t i = 0; i < s_count; ++i) {
        if (strcmp(s_entries[i].url, url) == 0) {
            found = static_cast<int>(i);
            break;
        }
    }
    unlock();
    return found;
}

extern "C" bool RADIO_FAV_PlayIndex(const size_t index)
{
    char url[RADIO_FAV_URL_SIZE];
    if (!RADIO_FAV_Get(index, nullptr, 0, url, sizeof(url))) {
        return false;
    }
    // Keep the single saved station in sync so the web/LCD URL field and a
    // plain reboot-retune land on the same stream.
    (void)MUSIC_SetRadioUrl(url);
    if (!MUSIC_PlayFile(url)) {
        return false;
    }
    lock();
    if (s_current != static_cast<int>(index)) {
        s_current = static_cast<int>(index);
        save_locked();
    }
    unlock();
    return true;
}

extern "C" bool RADIO_FAV_Next(void)
{
    if (s_count == 0u) {
        return false;
    }
    const size_t next = (s_current < 0) ? 0u : (static_cast<size_t>(s_current) + 1u) % s_count;
    return RADIO_FAV_PlayIndex(next);
}

extern "C" bool RADIO_FAV_Prev(void)
{
    if (s_count == 0u) {
        return false;
    }
    const size_t prev = (s_current <= 0) ? (s_count - 1u) : (static_cast<size_t>(s_current) - 1u);
    return RADIO_FAV_PlayIndex(prev);
}
