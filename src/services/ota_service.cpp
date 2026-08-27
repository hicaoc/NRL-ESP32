#include "ota_service.h"

#include "../app/driver/board_pins.h"
#include "../app/driver/external_radio.h"
#include "../lib/nrl_net_compat.h"
#include "../lib/nrl_version.h"
#include "../lib/nrl_wifi.h"
#include "config_notify.h"
#include "display_notice.h"
#include "lib/nrl_psram.h"
#include "music_player.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>

namespace {

constexpr const char *TAG = "OTA";
constexpr const char *kNvsNamespace = "nrl_ota";
constexpr const char *kDefaultServerUrl = "https://ota.nrlptt.com";
constexpr uint32_t kCheckPeriodMs = 60u * 60u * 1000u;
constexpr uint32_t kBootCheckDelayMs = 30u * 1000u;
// A failed check retries in 5 minutes instead of waiting a full period: boot
// checks land in the SMB-mount/scan window where internal RAM is transiently
// exhausted, and that failure heals itself once boot settles.
constexpr uint32_t kFailRetryMs = 5u * 60u * 1000u;
// Automatic checks additionally wait for internal RAM headroom: below these
// thresholds the check predictably dies in getaddrinfo/TLS allocation (see
// the "check failed" heap logs). User-requested checks bypass the gate.
constexpr uint32_t kMinInternalFreeForCheck = 16u * 1024u;
constexpr uint32_t kMinInternalLargestForCheck = 8u * 1024u;
// HTTPS receive, image validation and encrypted-flash writes otherwise form a
// permanently-ready loop. A small per-block pause leaves idle time on both
// cores for audio/UI/network work while only marginally extending an update.
constexpr uint32_t kOtaBlockPaceMs = 3u;
// ESP-IDF otherwise uses its 1 KiB minimum OTA buffer. Four KiB amortizes TLS,
// HTTP and scheduler overhead enough to approach local Wi-Fi upload speed
// while keeping the same per-block CPU pacing.
constexpr int kOtaHttpBufferBytes = 4096;
// HTTPS OTA reaches through HTTP, TLS, image validation and flash-writing
// frames from this task. 9 KB was enough for ordinary release checks but left
// no margin on ESP32-S3 once an install started, and FreeRTOS reported a
// corrupted nrl_ota stack. Keep this stack in internal RAM: flash writes can
// disable the cache, so a PSRAM-backed task stack is not safe here.
constexpr uint32_t kOtaTaskStackBytes = 16u * 1024u;

struct OtaState {
    NrlOtaStatus status = {};
    char token[96] = {};
    char requested_version[NRL_OTA_VERSION_MAX] = {};
    bool check_requested = false;
    bool update_requested = false;
    bool update_after_check = false;
    SemaphoreHandle_t lock = nullptr;
};

NRL_PSRAM_BSS OtaState s_ota;
// Parsing eight releases needs roughly 3.3 KB. It used to be a local array in
// parseManifest(), consuming over a third of the OTA task's old 9 KB stack
// before any HTTP/TLS frames were counted. The OTA task is the sole writer, so
// a PSRAM scratch buffer is safe and keeps that cost off the task stack.
NRL_PSRAM_BSS NrlOtaRelease s_manifest_releases[NRL_OTA_RELEASE_MAX];
// The OTA task is created lazily by s_spawn_timer when a check/update is
// due and the internal heap has room, and deletes itself when idle.
// Boot-time WiFi/codec bring-up on tight boards (BH4TDV-RF) lost to
// the old always-on 16 KB static stack. The stack still must be
// internal RAM: flash writes can disable the cache, so a
// PSRAM-backed task stack is not safe here.
TaskHandle_t s_ota_task = nullptr;
portMUX_TYPE s_task_lock = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t s_spawn_timer = nullptr;
uint32_t s_boot_ms = 0;

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void copyText(char *out, size_t out_size, const char *value)
{
    if (out == nullptr || out_size == 0u) return;
    snprintf(out, out_size, "%s", value != nullptr ? value : "");
}

bool parseReleaseVersion(const char *version, unsigned long (&parts)[3])
{
    if (version == nullptr) return false;
    if (*version == 'v' || *version == 'V') ++version;
    char trailing = '\0';
    return sscanf(version, "%lu.%lu.%lu%c", &parts[0], &parts[1], &parts[2],
                  &trailing) == 3;
}

bool isNewerFirmwareVersion(const char *candidate, const char *current)
{
    unsigned long candidate_parts[3] = {};
    unsigned long current_parts[3] = {};
    if (!parseReleaseVersion(candidate, candidate_parts) ||
        !parseReleaseVersion(current, current_parts)) {
        // Unknown version formats remain available for an explicit install,
        // but must never trigger an unattended upgrade or downgrade.
        return false;
    }
    for (size_t i = 0; i < 3u; ++i) {
        if (candidate_parts[i] != current_parts[i]) {
            return candidate_parts[i] > current_parts[i];
        }
    }
    return false;
}

const char *boardType()
{
#if NRL_BOARD == NRL_BOARD_GEZIPAI_4G
    return "gezipai_4g";
#elif NRL_BOARD == NRL_BOARD_GEZIPAI
    return "gezipai";
#elif NRL_BOARD == NRL_BOARD_BH4TDV_RF
    return "bh4tdv_rf";
#elif NRL_BOARD_IS_BI4UMD_FAMILY
    return "bi4umd";
#elif NRL_BOARD == NRL_BOARD_BH4TDV
    return "bh4tdv";
#elif NRL_BOARD == NRL_BOARD_S31_KORVO
    return "s31_korvo";
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
    return "s31_function_coreboard";
#else
    return "unknown";
#endif
}

// The service controls both ends of this compact protocol and only accepts
// JSON strings generated by ota-server. It is intentionally bounded and does
// not allocate a general JSON DOM on the ESP32 heap.
bool jsonStringAt(const char *begin, const char *end, const char *key,
                  char *out, size_t out_size)
{
    if (begin == nullptr || end == nullptr || key == nullptr || out == nullptr || out_size == 0u) return false;
    char needle[96] = {};
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *value = strstr(begin, needle);
    if (value == nullptr || value >= end) return false;
    value += strlen(needle);
    size_t pos = 0;
    while (value < end && *value != '\0' && *value != '"' && pos + 1u < out_size) {
        // ota-server emits JSON escapes only if an operator puts them in a
        // release note. URL/version fields do not permit them.
        if (*value == '\\' && value + 1 < end) ++value;
        out[pos++] = *value++;
    }
    out[pos] = '\0';
    return value < end && *value == '"';
}

bool parseManifest(const char *json)
{
    if (json == nullptr) return false;
    memset(s_manifest_releases, 0, sizeof(s_manifest_releases));
    size_t count = 0;
    const char *cursor = json;
    while (count < NRL_OTA_RELEASE_MAX) {
        const char *entry = strstr(cursor, "{\"version\":");
        if (entry == nullptr) break;
        const char *end = strchr(entry, '}');
        if (end == nullptr) break;
        if (jsonStringAt(entry, end, "version", s_manifest_releases[count].version,
                         sizeof(s_manifest_releases[count].version)) &&
            jsonStringAt(entry, end, "url", s_manifest_releases[count].url,
                         sizeof(s_manifest_releases[count].url))) {
            (void)jsonStringAt(entry, end, "notes", s_manifest_releases[count].notes,
                               sizeof(s_manifest_releases[count].notes));
            ++count;
        }
        cursor = end + 1;
    }
    char latest[NRL_OTA_VERSION_MAX] = {};
    (void)jsonStringAt(json, json + strlen(json), "latest_version", latest, sizeof(latest));
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.status.release_count = count;
    memcpy(s_ota.status.releases, s_manifest_releases, sizeof(s_manifest_releases));
    copyText(s_ota.status.latest_version, sizeof(s_ota.status.latest_version), latest);
    xSemaphoreGive(s_ota.lock);
    // A latest version without a matching release URL would notify the user
    // about an update that cannot be selected or installed. Treat that as a
    // malformed manifest instead of reporting a successful check.
    return (count == 0u) == (latest[0] == '\0');
}

void setError(const char *error)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copyText(s_ota.status.last_error, sizeof(s_ota.status.last_error), error);
    xSemaphoreGive(s_ota.lock);
}

void setUpdateProgress(uint32_t bytes, uint32_t size)
{
    uint32_t percent = size > 0u
                           ? static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 100u) / size)
                           : 0u;
    if (percent > 100u) percent = 100u;

    bool percent_changed = false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    percent_changed = s_ota.status.update_size != size ||
                      s_ota.status.update_percent != percent;
    s_ota.status.update_bytes = bytes;
    s_ota.status.update_size = size;
    s_ota.status.update_percent = static_cast<uint8_t>(percent);
    xSemaphoreGive(s_ota.lock);

    // Redrawing the progress overlay 100 times is disproportionately costly on
    // display boards. Status consumers still see every percentage; the notice
    // layer advances in 2% steps (plus the final 100%).
    if (percent_changed && size > 0u && (percent == 100u || (percent % 2u) == 0u)) {
        char notice[40] = {};
        snprintf(notice, sizeof(notice), "OTA UPDATING %u%%",
                 static_cast<unsigned>(percent));
        DISPLAY_NOTICE_PostProgress(notice, DISPLAY_NOTICE_WARNING, 0u,
                                    static_cast<uint8_t>(percent));
    }
}

void finishReleaseCheck(bool ok)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.status.checking = false;
    // Backdate failures so the next automatic check comes in kFailRetryMs
    // rather than a full kCheckPeriodMs after a transient (memory-shaped) miss.
    s_ota.status.last_check_ms = ok ? nowMs() : nowMs() - (kCheckPeriodMs - kFailRetryMs);
    if (ok) s_ota.status.last_error[0] = '\0';
    xSemaphoreGive(s_ota.lock);
}

std::string apiUrl(const char *suffix)
{
    char base[NRL_OTA_URL_MAX] = {};
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copyText(base, sizeof(base), s_ota.status.server_url);
    xSemaphoreGive(s_ota.lock);
    size_t len = strlen(base);
    while (len > 0u && base[len - 1u] == '/') base[--len] = '\0';
    return std::string(base) + suffix;
}

bool isHttpUrl(const char *url)
{
    return url != nullptr &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

std::string absoluteUrl(const char *url)
{
    if (url == nullptr || url[0] == '\0') return {};
    if (isHttpUrl(url)) return url;
    // Download URLs are server-relative so a release can move between hosts.
    return apiUrl("").append(url[0] == '/' ? url : (std::string("/") + url));
}

bool checkForReleases()
{
    DISPLAY_NOTICE_Post("OTA CHECKING...", DISPLAY_NOTICE_INFO, 20000u);
    if (!nrlNetworkConnected()) {
        setError("Network is not connected");
        DISPLAY_NOTICE_Post("OTA CHECK FAILED", DISPLAY_NOTICE_ERROR, 8000u);
        return false;
    }
    char url[NRL_OTA_URL_MAX] = {};
    char token[sizeof(s_ota.token)] = {};
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copyText(url, sizeof(url), s_ota.status.server_url);
    copyText(token, sizeof(token), s_ota.token);
    s_ota.status.checking = true;
    xSemaphoreGive(s_ota.lock);
    if (url[0] == '\0') {
        finishReleaseCheck(false);
        return false;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const ExternalRadioConfig *radio = EXTERNAL_RADIO_GetConfig();
    char body[512] = {};
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%02X%02X%02X%02X%02X%02X\",\"board_type\":\"%s\","
             "\"firmware_version\":\"%s\",\"metadata\":{\"nrl_callsign\":\"%s\","
             "\"nrl_ssid\":%u,\"firmware_name\":\"%s\"}}",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], boardType(),
             NRL_FIRMWARE_VERSION, radio != nullptr ? radio->callsign : "",
             static_cast<unsigned>(radio != nullptr ? radio->callsign_ssid : 0u), NRL_FIRMWARE_NAME);
    const std::string endpoint = apiUrl("/api/v1/devices/check");
    constexpr size_t kResponseCapacity = 8192u;
    void *response_memory = heap_caps_malloc(kResponseCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response_memory == nullptr) {
        response_memory = heap_caps_malloc(kResponseCapacity, MALLOC_CAP_8BIT);
    }
    std::unique_ptr<char, decltype(&heap_caps_free)> response(
        static_cast<char *>(response_memory), &heap_caps_free);
    if (!response) {
        setError("cannot allocate OTA manifest buffer");
        finishReleaseCheck(false);
        DISPLAY_NOTICE_Post("OTA CHECK FAILED", DISPLAY_NOTICE_ERROR, 8000u);
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = endpoint.c_str(); config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000; config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        setError("cannot create HTTP client");
        finishReleaseCheck(false);
        DISPLAY_NOTICE_Post("OTA CHECK FAILED", DISPLAY_NOTICE_ERROR, 8000u);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (token[0] != '\0') esp_http_client_set_header(client, "X-Device-Token", token);
    bool ok = false;
    if (esp_http_client_open(client, strlen(body)) == ESP_OK &&
        esp_http_client_write(client, body, strlen(body)) == static_cast<int>(strlen(body)) &&
        esp_http_client_fetch_headers(client) >= 0 &&
        esp_http_client_get_status_code(client) == 200) {
        int total = 0;
        while (total < static_cast<int>(kResponseCapacity - 1u)) {
            const int n = esp_http_client_read(
                client, response.get() + total, kResponseCapacity - 1u - static_cast<size_t>(total));
            if (n <= 0) break;
            total += n;
        }
        response.get()[total] = '\0';
        ok = total > 0 && parseManifest(response.get());
        if (!ok) setError("invalid OTA manifest");
    } else {
        char error[80] = {};
        snprintf(error, sizeof(error), "OTA check HTTP status %d", esp_http_client_get_status_code(client));
        setError(error);
    }
    esp_http_client_cleanup(client);
    finishReleaseCheck(ok);
    if (ok) {
        char latest[NRL_OTA_VERSION_MAX] = {};
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        copyText(latest, sizeof(latest), s_ota.status.latest_version);
        xSemaphoreGive(s_ota.lock);
        if (isNewerFirmwareVersion(latest, NRL_FIRMWARE_VERSION)) {
            char notice[96] = {};
            snprintf(notice, sizeof(notice), "NEW FIRMWARE %.64s", latest);
            DISPLAY_NOTICE_Post(notice, DISPLAY_NOTICE_SUCCESS, 10000u);
        } else {
            DISPLAY_NOTICE_Post("FIRMWARE IS UP TO DATE", DISPLAY_NOTICE_SUCCESS, 5000u);
        }
    } else {
        DISPLAY_NOTICE_Post("OTA CHECK FAILED", DISPLAY_NOTICE_ERROR, 8000u);
    }
    return ok;
}

bool installVersion(const char *version)
{
    ESP_LOGI(TAG, "install start: OTA stack minimum free=%u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    NrlOtaRelease release = {};
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    for (size_t i = 0; i < s_ota.status.release_count; ++i) {
        if (strcmp(s_ota.status.releases[i].version, version) == 0) {
            release = s_ota.status.releases[i];
            break;
        }
    }
    s_ota.status.updating = true;
    s_ota.status.update_bytes = 0u;
    s_ota.status.update_size = 0u;
    s_ota.status.update_percent = 0u;
    xSemaphoreGive(s_ota.lock);
    if (release.url[0] == '\0') {
        setError("requested version is not in the current manifest");
        DISPLAY_NOTICE_Post("OTA UPDATE FAILED", DISPLAY_NOTICE_ERROR, 10000u);
        return false;
    }
    const std::string url = absoluteUrl(release.url);
    // HTTP is supported for trusted LAN deployments. HTTPS remains the safe
    // choice for any network where the server or traffic cannot be trusted.
    if (!isHttpUrl(url.c_str())) {
        setError("OTA firmware URL must use HTTP or HTTPS");
        DISPLAY_NOTICE_Post("OTA UPDATE FAILED", DISPLAY_NOTICE_ERROR, 10000u);
        return false;
    }
    DISPLAY_NOTICE_Post("OTA UPDATING...", DISPLAY_NOTICE_WARNING, 0u);
    esp_http_client_config_t http = {};
    http.url = url.c_str(); http.timeout_ms = 30000; http.crt_bundle_attach = esp_crt_bundle_attach;
    http.buffer_size = kOtaHttpBufferBytes;
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;
    ota.bulk_flash_erase = false;
    esp_https_ota_handle_t ota_handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota, &ota_handle);
    ESP_LOGI(TAG, "HTTPS OTA begin: %s, stack minimum free=%u bytes",
             esp_err_to_name(err),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    uint32_t image_size = 0u;
    if (err == ESP_OK) {
        const int reported_size = esp_https_ota_get_image_size(ota_handle);
        if (reported_size > 0) image_size = static_cast<uint32_t>(reported_size);
        setUpdateProgress(0u, image_size);

        do {
            err = esp_https_ota_perform(ota_handle);
            const int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
            const int current_size = esp_https_ota_get_image_size(ota_handle);
            if (current_size > 0) image_size = static_cast<uint32_t>(current_size);
            if (bytes_read >= 0) {
                setUpdateProgress(static_cast<uint32_t>(bytes_read), image_size);
            }
            if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                vTaskDelay(pdMS_TO_TICKS(kOtaBlockPaceMs));
            }
        } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

        ESP_LOGI(TAG, "HTTPS OTA perform: %s, stack minimum free=%u bytes",
                 esp_err_to_name(err),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

        if (err == ESP_OK && !esp_https_ota_is_complete_data_received(ota_handle)) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK) {
            err = esp_https_ota_finish(ota_handle);
        } else {
            esp_https_ota_abort(ota_handle);
        }
    }
    if (err != ESP_OK) {
        char error[96] = {};
        snprintf(error, sizeof(error), "firmware update failed: %s", esp_err_to_name(err));
        setError(error);
        DISPLAY_NOTICE_Post("OTA UPDATE FAILED", DISPLAY_NOTICE_ERROR, 10000u);
        return false;
    }
    if (image_size > 0u) setUpdateProgress(image_size, image_size);
    ESP_LOGI(TAG, "OTA image %s written; rebooting", release.version);
    DISPLAY_NOTICE_Post("OTA COMPLETE - REBOOTING", DISPLAY_NOTICE_SUCCESS, 0u);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return true;
}

void otaTask(void *);

bool otaWorkPending()
{
    bool pending = false;
    if (xSemaphoreTake(s_ota.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        pending = s_ota.check_requested || s_ota.update_requested ||
                  (s_ota.status.configured &&
                   (nowMs() - s_ota.status.last_check_ms >= kCheckPeriodMs ||
                    (s_ota.status.last_check_ms == 0u && nowMs() - s_boot_ms >= kBootCheckDelayMs)));
        xSemaphoreGive(s_ota.lock);
    }
    return pending;
}

void otaSpawnTimerCb(void *)
{
    taskENTER_CRITICAL(&s_task_lock);
    const bool running = s_ota_task != nullptr;
    taskEXIT_CRITICAL(&s_task_lock);
    if (running || !otaWorkPending()) return;
    // Never compete with boot WiFi/codec bring-up or a busy audio
    // pipeline for the 16 KB internal stack: stay pending and let the
    // timer retry once the heap has room.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) <
        kOtaTaskStackBytes + 8u * 1024u) {
        return;
    }
    TaskHandle_t created = nullptr;
    if (xTaskCreate(otaTask, "nrl_ota", kOtaTaskStackBytes, nullptr, 3,
                    &created) == pdPASS) {
        taskENTER_CRITICAL(&s_task_lock);
        s_ota_task = created;
        taskEXIT_CRITICAL(&s_task_lock);
    }
}

void otaTask(void *)
{
    bool s_low_mem_defer_logged = false;
    while (true) {
        bool do_check = false, do_update = false, update_after_check = false;
        char version[NRL_OTA_VERSION_MAX] = {};
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        const bool due = s_ota.status.configured &&
                         (nowMs() - s_ota.status.last_check_ms >= kCheckPeriodMs ||
                          (s_ota.status.last_check_ms == 0u && nowMs() - s_boot_ms >= kBootCheckDelayMs));
        // A TLS handshake spikes tens of KB of internal RAM; running one
        // while music plays starves lwIP/GMF (SMB stalls, decode OOM, failed
        // DNS). Defer checks to an idle moment instead of failing mid-track.
        const bool playing = MUSIC_IsPlaying();
        do_check = (s_ota.check_requested || due) && !playing;
        if (do_check && !s_ota.check_requested) {
            // Internal RAM must have room for the TLS handshake/lwIP churn;
            // a boot-time scan storm can squeeze it to single-digit KB for a
            // while. Automatic checks defer (they stay due and fire as soon
            // as the heap recovers); manual checks run regardless.
            const uint32_t free_int =
                static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            const uint32_t largest_int =
                static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (free_int < kMinInternalFreeForCheck ||
                largest_int < kMinInternalLargestForCheck) {
                do_check = false;
                if (!s_low_mem_defer_logged) {
                    s_low_mem_defer_logged = true;
                    ESP_LOGI(TAG, "check deferred: internal free=%u largest=%u",
                             static_cast<unsigned>(free_int),
                             static_cast<unsigned>(largest_int));
                }
            }
        }
        if (do_check) {
            s_low_mem_defer_logged = false;
            s_ota.check_requested = false;
        }
        do_update = s_ota.update_requested;
        update_after_check = s_ota.update_after_check;
        copyText(version, sizeof(version), s_ota.requested_version);
        s_ota.update_requested = false;
        s_ota.update_after_check = false;
        xSemaphoreGive(s_ota.lock);
        if (do_check) {
            // TLS failures on this device are memory-shaped; log the heap so
            // "PSA -141 / alloc failed" can be told apart from server issues.
            ESP_LOGI(TAG, "check start: internal free=%u largest=%u | psram free=%u largest=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        }
        const bool check_ok = !do_check || checkForReleases();
        if (do_check && !check_ok) {
            ESP_LOGW(TAG, "check failed: internal free=%u largest=%u min=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
        }
        if (do_update) {
            (void)installVersion(version);
        } else if (update_after_check && check_ok) {
            char latest[NRL_OTA_VERSION_MAX] = {};
            xSemaphoreTake(s_ota.lock, portMAX_DELAY);
            copyText(latest, sizeof(latest), s_ota.status.latest_version);
            xSemaphoreGive(s_ota.lock);
            if (!isNewerFirmwareVersion(latest, NRL_FIRMWARE_VERSION)) {
                setError("no newer OTA release available");
            } else {
                (void)installVersion(latest);
            }
        }
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        s_ota.status.updating = false;
        xSemaphoreGive(s_ota.lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!otaWorkPending()) {
            break;  // return the 16 KB stack; the spawn timer re-creates us
        }
    }
    taskENTER_CRITICAL(&s_task_lock);
    s_ota_task = nullptr;
    taskEXIT_CRITICAL(&s_task_lock);
    vTaskDelete(nullptr);
}
} // namespace

bool OtaService_Init()
{
    if (s_ota.lock != nullptr) return true;
    s_ota.lock = xSemaphoreCreateMutex();
    if (s_ota.lock == nullptr) return false;
    nvs_handle_t nvs;
    bool server_url_saved = false;
    const esp_err_t open_err = nvs_open(kNvsNamespace, NVS_READONLY, &nvs);
    if (open_err == ESP_OK) {
        size_t size = sizeof(s_ota.status.server_url);
        server_url_saved = nvs_get_str(nvs, "url", s_ota.status.server_url, &size) == ESP_OK;
        size = sizeof(s_ota.token);
        (void)nvs_get_str(nvs, "token", s_ota.token, &size);
        nvs_close(nvs);
    } else if (open_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "cannot read OTA config: %s", esp_err_to_name(open_err));
    }
    if (!server_url_saved) {
        copyText(s_ota.status.server_url, sizeof(s_ota.status.server_url), kDefaultServerUrl);
    }
    s_ota.status.configured = s_ota.status.server_url[0] != '\0';
    ESP_LOGI(TAG, "OTA config loaded: configured=%d server=%s",
             s_ota.status.configured, s_ota.status.server_url);
    s_boot_ms = nowMs();
    const esp_timer_create_args_t timer_args = {
        .callback = &otaSpawnTimerCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nrl_ota_spawn",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &s_spawn_timer) != ESP_OK) {
        return false;
    }
    return esp_timer_start_periodic(s_spawn_timer, 2000u * 1000u) == ESP_OK;
}

bool OtaService_SetConfig(const char *server_url, const char *device_token)
{
    if (server_url == nullptr || strlen(server_url) >= NRL_OTA_URL_MAX) return false;
    if (server_url[0] != '\0' && !isHttpUrl(server_url)) {
        ESP_LOGW(TAG, "OTA server URL rejected (HTTP or HTTPS required): %s", server_url);
        return false;
    }
    if (device_token == nullptr || strlen(device_token) >= sizeof(s_ota.token)) return false;
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const esp_err_t err = nvs_set_str(nvs, "url", server_url);
    const esp_err_t token_err = err == ESP_OK ? nvs_set_str(nvs, "token", device_token) : err;
    const esp_err_t commit_err = token_err == ESP_OK ? nvs_commit(nvs) : token_err;
    nvs_close(nvs);
    if (commit_err != ESP_OK) {
        ESP_LOGE(TAG, "cannot save OTA config: %s", esp_err_to_name(commit_err));
        return false;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copyText(s_ota.status.server_url, sizeof(s_ota.status.server_url), server_url);
    copyText(s_ota.token, sizeof(s_ota.token), device_token);
    s_ota.status.configured = server_url[0] != '\0';
    s_ota.status.last_error[0] = '\0';
    s_ota.status.latest_version[0] = '\0';
    s_ota.status.release_count = 0u;
    memset(s_ota.status.releases, 0, sizeof(s_ota.status.releases));
    xSemaphoreGive(s_ota.lock);
    CONFIG_NOTIFY_Bump();
    ESP_LOGI(TAG, "OTA server config saved: configured=%d server=%s",
             server_url[0] != '\0', server_url);
    return true;
}

bool OtaService_SetServerUrl(const char *server_url)
{
    if (s_ota.lock == nullptr) return false;
    char token[sizeof(s_ota.token)] = {};
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copyText(token, sizeof(token), s_ota.token);
    xSemaphoreGive(s_ota.lock);
    return OtaService_SetConfig(server_url, token);
}

void OtaService_GetStatus(NrlOtaStatus *out)
{
    if (out == nullptr || s_ota.lock == nullptr) return;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    *out = s_ota.status;
    xSemaphoreGive(s_ota.lock);
}

bool OtaService_CheckNow()
{
    if (s_ota.lock == nullptr) return false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    if (!s_ota.status.configured) {
        xSemaphoreGive(s_ota.lock);
        return false;
    }
    s_ota.status.last_error[0] = '\0';
    s_ota.check_requested = true;
    xSemaphoreGive(s_ota.lock);
    return true;
}

bool OtaService_CheckAndUpdateLatest()
{
    if (s_ota.lock == nullptr) return false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    if (!s_ota.status.configured) {
        xSemaphoreGive(s_ota.lock);
        return false;
    }
    s_ota.status.last_error[0] = '\0';
    s_ota.check_requested = true;
    s_ota.update_after_check = true;
    xSemaphoreGive(s_ota.lock);
    return true;
}

bool OtaService_UpdateVersion(const char *version)
{
    if (version == nullptr || version[0] == '\0' || strlen(version) >= NRL_OTA_VERSION_MAX) return false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    if (!s_ota.status.configured) {
        xSemaphoreGive(s_ota.lock);
        return false;
    }
    s_ota.status.last_error[0] = '\0';
    copyText(s_ota.requested_version, sizeof(s_ota.requested_version), version);
    s_ota.update_requested = true;
    xSemaphoreGive(s_ota.lock);
    return true;
}
