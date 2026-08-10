#include "services/server_list_store.h"

#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

namespace {

constexpr const char *TAG = "SERVER_FS";
constexpr const char *kMountPoint = "/serverfs";
constexpr const char *kPartitionLabel = "serverfs";
constexpr uint32_t kFileMagic = 0x54534c53u; // "SLST", little endian
constexpr uint16_t kFileVersion = 1u;
constexpr size_t kMaxPayload = 64u * 1024u;

struct FileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t payload_size;
    uint32_t crc32;
    uint8_t reserved[16];
};
static_assert(sizeof(FileHeader) == 32u, "server-list file header size");

static SemaphoreHandle_t s_mutex = nullptr;
static bool s_mount_attempted = false;
static bool s_ready = false;
static volatile uint32_t s_generation[SERVER_FILE_FMO_DEVICE_KEY + 1u] = {};

static const char *skipSpace(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                            *cursor == '\r' || *cursor == '\n')) {
        ++cursor;
    }
    return cursor;
}

static bool jsonStringField(const char *begin, const char *end,
                            const char *key, char *out, const size_t out_size)
{
    if (begin == nullptr || end == nullptr || key == nullptr || out == nullptr ||
        out_size == 0u) return false;
    char needle[40] = {};
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) <= 0) return false;
    const size_t needle_size = strlen(needle);
    const char *cursor = begin;
    while (cursor + needle_size < end) {
        const char *found = strstr(cursor, needle);
        if (found == nullptr || found + needle_size >= end) return false;
        cursor = skipSpace(found + needle_size, end);
        if (cursor >= end || *cursor != ':') {
            cursor = found + needle_size;
            continue;
        }
        cursor = skipSpace(cursor + 1, end);
        if (cursor >= end || *cursor != '"') return false;
        ++cursor;
        size_t written = 0u;
        bool escaped = false;
        while (cursor < end) {
            const char ch = *cursor++;
            if (!escaped && ch == '"') {
                out[written] = '\0';
                return true;
            }
            if (!escaped && ch == '\\') {
                escaped = true;
                continue;
            }
            char decoded = ch;
            if (escaped) {
                if (ch == 'n') decoded = '\n';
                else if (ch == 'r') decoded = '\r';
                else if (ch == 't') decoded = '\t';
                else if (ch == 'b') decoded = '\b';
                else if (ch == 'f') decoded = '\f';
                else if (ch == 'u') {
                    // Platform-server responses use UTF-8 directly. Keep a
                    // readable placeholder for an uncommon escaped codepoint
                    // without allocating a general JSON decoder here.
                    decoded = '?';
                    for (unsigned i = 0u; i < 4u && cursor < end; ++i) ++cursor;
                }
                escaped = false;
            }
            if (written + 1u < out_size) out[written++] = decoded;
        }
        return false;
    }
    return false;
}

static bool jsonUintField(const char *begin, const char *end,
                          const char *key, unsigned long *value)
{
    if (begin == nullptr || end == nullptr || key == nullptr || value == nullptr)
        return false;
    char needle[40] = {};
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) <= 0) return false;
    const size_t needle_size = strlen(needle);
    const char *found = strstr(begin, needle);
    if (found == nullptr || found + needle_size >= end) return false;
    const char *cursor = skipSpace(found + needle_size, end);
    if (cursor >= end || *cursor != ':') return false;
    cursor = skipSpace(cursor + 1, end);
    if (cursor < end && *cursor == '"') ++cursor;
    if (cursor >= end || *cursor < '0' || *cursor > '9') return false;
    unsigned long parsed = 0u;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        parsed = parsed * 10u + static_cast<unsigned long>(*cursor++ - '0');
    }
    *value = parsed;
    return true;
}

static bool jsonBoolField(const char *begin, const char *end,
                          const char *key, bool *value)
{
    if (begin == nullptr || end == nullptr || key == nullptr || value == nullptr)
        return false;
    char needle[40] = {};
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) <= 0) return false;
    const size_t needle_size = strlen(needle);
    const char *found = strstr(begin, needle);
    if (found == nullptr || found + needle_size >= end) return false;
    const char *cursor = skipSpace(found + needle_size, end);
    if (cursor >= end || *cursor != ':') return false;
    cursor = skipSpace(cursor + 1, end);
    if (cursor + 4u <= end && memcmp(cursor, "true", 4u) == 0) {
        *value = true;
        return true;
    }
    if (cursor + 5u <= end && memcmp(cursor, "false", 5u) == 0) {
        *value = false;
        return true;
    }
    return false;
}

static void normalizeNrlHost(const char *raw, char *out, const size_t out_size)
{
    if (out == nullptr || out_size == 0u) return;
    snprintf(out, out_size, "%s", raw != nullptr ? raw : "");
    char *colon = strrchr(out, ':');
    if (colon == nullptr || strchr(out, ':') != colon || colon[1] == '\0') return;
    for (const char *p = colon + 1; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return;
    }
    *colon = '\0'; // reporting port embedded in the API host field
}

static bool findNrlNameInJson(const char *json, const size_t size,
                              const char *wanted_host, const uint16_t wanted_port,
                              char *name, const size_t name_size)
{
    const char *end = json + size;
    const unsigned object_depth = strstr(json, "\"items\"") != nullptr ? 3u : 2u;
    bool in_string = false;
    bool escaped = false;
    unsigned depth = 0u;
    const char *object = nullptr;
    for (const char *cursor = json; cursor < end; ++cursor) {
        const char ch = *cursor;
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            if (++depth == object_depth) object = cursor;
        } else if (ch == '}' && depth > 0u) {
            if (depth == object_depth && object != nullptr) {
                char item_host[96] = {};
                unsigned long item_port = 0u;
                if (jsonStringField(object, cursor + 1, "host", item_host,
                                    sizeof(item_host)) &&
                    jsonUintField(object, cursor + 1, "port", &item_port) &&
                    item_port == wanted_port) {
                    char normalized[96] = {};
                    normalizeNrlHost(item_host, normalized, sizeof(normalized));
                    if (strcasecmp(normalized, wanted_host) == 0) {
                        if (!jsonStringField(object, cursor + 1, "name", name,
                                             name_size) || name[0] == '\0') {
                            snprintf(name, name_size, "%s", normalized);
                        }
                        return true;
                    }
                }
                object = nullptr;
            }
            --depth;
        }
    }
    return false;
}

static bool findNrlPlatformAuthorityInJson(
    const char *json, const size_t size, const char *wanted_host,
    const uint16_t wanted_port, char *authority, const size_t authority_size)
{
    const char *end = json + size;
    const unsigned object_depth = strstr(json, "\"items\"") != nullptr ? 3u : 2u;
    bool in_string = false;
    bool escaped = false;
    unsigned depth = 0u;
    const char *object = nullptr;
    for (const char *cursor = json; cursor < end; ++cursor) {
        const char ch = *cursor;
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') in_string = true;
        else if (ch == '{') {
            if (++depth == object_depth) object = cursor;
        } else if (ch == '}' && depth > 0u) {
            if (depth == object_depth && object != nullptr) {
                char raw_host[96] = {};
                char normalized[96] = {};
                unsigned long item_port = 0u;
                if (jsonStringField(object, cursor + 1, "host", raw_host,
                                    sizeof(raw_host)) &&
                    jsonUintField(object, cursor + 1, "port", &item_port)) {
                    normalizeNrlHost(raw_host, normalized, sizeof(normalized));
                    if (item_port == wanted_port &&
                        strcasecmp(normalized, wanted_host) == 0) {
                        snprintf(authority, authority_size, "%s", raw_host);
                        return authority[0] != '\0';
                    }
                }
                object = nullptr;
            }
            --depth;
        }
    }
    return false;
}

static bool parseNrlServerObject(const char *begin, const char *end,
                                 NrlServerInfo *server)
{
    if (server == nullptr) return false;
    *server = {};
    bool hidden = false;
    if (jsonBoolField(begin, end, "hidden", &hidden) && hidden) return false;
    char raw_host[96] = {};
    unsigned long port = 0u;
    if (!jsonStringField(begin, end, "host", raw_host, sizeof(raw_host)) ||
        !jsonUintField(begin, end, "port", &port) ||
        port == 0u || port > 65535u) {
        return false;
    }
    normalizeNrlHost(raw_host, server->host, sizeof(server->host));
    if (server->host[0] == '\0') return false;
    if (!jsonStringField(begin, end, "name", server->name,
                         sizeof(server->name)) || server->name[0] == '\0') {
        snprintf(server->name, sizeof(server->name), "%s", server->host);
    }
    server->port = static_cast<uint16_t>(port);
    unsigned long value = 0u;
    if (jsonUintField(begin, end, "online", &value))
        server->online = static_cast<uint32_t>(value);
    if (jsonUintField(begin, end, "total", &value))
        server->total = static_cast<uint32_t>(value);
    return true;
}

static size_t loadNrlServersFromJson(const char *json, const size_t size,
                                     NrlServerInfo *servers,
                                     const size_t capacity)
{
    const char *end = json + size;
    const unsigned object_depth = strstr(json, "\"items\"") != nullptr ? 3u : 2u;
    bool in_string = false;
    bool escaped = false;
    unsigned depth = 0u;
    const char *object = nullptr;
    size_t count = 0u;
    for (const char *cursor = json; cursor < end; ++cursor) {
        const char ch = *cursor;
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            if (++depth == object_depth) object = cursor;
        } else if (ch == '}' && depth > 0u) {
            if (depth == object_depth && object != nullptr) {
                NrlServerInfo parsed = {};
                if (parseNrlServerObject(object, cursor + 1, &parsed)) {
                    if (servers != nullptr && count < capacity)
                        servers[count] = parsed;
                    ++count;
                }
                object = nullptr;
            }
            --depth;
        }
    }
    return count;
}

static bool paths(const ServerListKind kind, char *main_path, size_t main_size,
                  char *temp_path, size_t temp_size, char *backup_path,
                  size_t backup_size)
{
    const char *stem = nullptr;
    if (kind == SERVER_LIST_NRL) stem = "nrl_servers";
    if (kind == SERVER_LIST_FMO) stem = "fmo_servers";
    if (kind == SERVER_FILE_FMO_USER_CERT) stem = "fmo_user_cert";
    if (kind == SERVER_FILE_FMO_INTERMEDIATE_CERT)
        stem = "fmo_intermediate_cert";
    if (kind == SERVER_FILE_FMO_DEVICE_KEY) stem = "fmo_device_key";
    if (stem == nullptr) return false;
    return snprintf(main_path, main_size, "%s/%s.bin", kMountPoint, stem) > 0 &&
           snprintf(temp_path, temp_size, "%s/%s.tmp", kMountPoint, stem) > 0 &&
           snprintf(backup_path, backup_size, "%s/%s.bak", kMountPoint, stem) > 0;
}

static uint8_t *allocatePayload(const size_t size)
{
    // Server lists are intentionally never allowed to consume internal SRAM.
    // Callers retain their current/live list when PSRAM is temporarily
    // unavailable, so failing is safer than falling back to MALLOC_CAP_INTERNAL.
    return static_cast<uint8_t *>(
        heap_caps_malloc(size + 1u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static bool validHttpsAuthority(const char *authority)
{
    if (authority == nullptr || authority[0] == '\0' ||
        strlen(authority) >= 96u) return false;
    const char *colon = strrchr(authority, ':');
    const char *host_end = colon != nullptr ? colon : authority + strlen(authority);
    if (host_end == authority) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(authority);
         reinterpret_cast<const char *>(p) < host_end; ++p) {
        if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_') return false;
    }
    if (colon != nullptr) {
        if (colon[1] == '\0') return false;
        for (const char *p = colon + 1; *p != '\0'; ++p)
            if (!isdigit(static_cast<unsigned char>(*p))) return false;
    }
    return true;
}

static bool readFile(const char *path, const ServerListKind kind,
                     uint8_t **payload, size_t *payload_size)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return false;
    FileHeader header = {};
    bool ok = fread(&header, 1u, sizeof(header), file) == sizeof(header) &&
              header.magic == kFileMagic && header.version == kFileVersion &&
              header.kind == static_cast<uint16_t>(kind) &&
              header.payload_size > 0u && header.payload_size <= kMaxPayload;
    uint8_t *data = nullptr;
    if (ok) {
        data = allocatePayload(header.payload_size);
        ok = data != nullptr &&
             fread(data, 1u, header.payload_size, file) == header.payload_size &&
             esp_rom_crc32_le(0u, data, header.payload_size) == header.crc32;
    }
    fclose(file);
    if (!ok) {
        free(data);
        return false;
    }
    data[header.payload_size] = 0u;
    *payload = data;
    *payload_size = header.payload_size;
    return true;
}

static bool writeFile(const char *path, const ServerListKind kind,
                      const void *payload, const size_t payload_size)
{
    FILE *file = fopen(path, "wb");
    if (file == nullptr) return false;
    FileHeader header = {};
    header.magic = kFileMagic;
    header.version = kFileVersion;
    header.kind = static_cast<uint16_t>(kind);
    header.payload_size = static_cast<uint32_t>(payload_size);
    header.crc32 = esp_rom_crc32_le(
        0u, static_cast<const uint8_t *>(payload), payload_size);
    bool ok = fwrite(&header, 1u, sizeof(header), file) == sizeof(header);
    const auto *bytes = static_cast<const uint8_t *>(payload);
    size_t written = 0u;
    // LittleFS may erase/program several blocks for the FMO directory. Split
    // the operation so IDLE0 can run between chunks instead of tripping the
    // task watchdog while a 30-60 KiB cache is replaced atomically.
    while (ok && written < payload_size) {
        const size_t chunk = payload_size - written > 2048u
                                 ? 2048u : payload_size - written;
        ok = fwrite(bytes + written, 1u, chunk, file) == chunk;
        written += ok ? chunk : 0u;
        vTaskDelay(1);
    }
    if (ok) ok = fflush(file) == 0;
    if (ok) ok = fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}

} // namespace

extern "C" bool SERVER_LIST_STORE_Init(void)
{
    if (s_mount_attempted) return s_ready;
    s_mount_attempted = true;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "mutex allocation failed");
        return false;
    }
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
        kPartitionLabel);
    if (partition == nullptr) {
        ESP_LOGE(TAG, "LittleFS partition '%s' is missing", kPartitionLabel);
        return false;
    }
    esp_vfs_littlefs_conf_t config = {};
    config.base_path = kMountPoint;
    config.partition_label = kPartitionLabel;
    config.format_if_mount_failed = true;
    config.grow_on_mount = true;
    const esp_err_t error = esp_vfs_littlefs_register(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(error));
        return false;
    }
    s_ready = true;
    size_t total = 0u;
    size_t used = 0u;
    (void)esp_littlefs_info(kPartitionLabel, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at 0x%lx (%lu/%lu bytes used)",
             static_cast<unsigned long>(partition->address),
             static_cast<unsigned long>(used),
             static_cast<unsigned long>(total));
    return true;
}

extern "C" bool SERVER_LIST_STORE_Ready(void)
{
    return s_ready;
}

extern "C" bool SERVER_LIST_STORE_Read(const ServerListKind kind,
                                        uint8_t **payload,
                                        size_t *payload_size)
{
    if (payload == nullptr || payload_size == nullptr) return false;
    *payload = nullptr;
    *payload_size = 0u;
    if (!s_ready && !SERVER_LIST_STORE_Init()) return false;
    char main_path[48] = {};
    char temp_path[48] = {};
    char backup_path[48] = {};
    if (!paths(kind, main_path, sizeof(main_path), temp_path, sizeof(temp_path),
               backup_path, sizeof(backup_path))) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = readFile(main_path, kind, payload, payload_size);
    if (!ok) ok = readFile(backup_path, kind, payload, payload_size);
    xSemaphoreGive(s_mutex);
    return ok;
}

extern "C" bool SERVER_LIST_STORE_Write(const ServerListKind kind,
                                         const void *payload,
                                         const size_t payload_size)
{
    if (payload == nullptr || payload_size == 0u || payload_size > kMaxPayload)
        return false;
    if (!s_ready && !SERVER_LIST_STORE_Init()) return false;
    char main_path[48] = {};
    char temp_path[48] = {};
    char backup_path[48] = {};
    if (!paths(kind, main_path, sizeof(main_path), temp_path, sizeof(temp_path),
               backup_path, sizeof(backup_path))) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    (void)remove(temp_path);
    bool ok = writeFile(temp_path, kind, payload, payload_size);
    if (ok) {
        (void)remove(backup_path);
        if (rename(main_path, backup_path) != 0 && errno != ENOENT) ok = false;
    }
    if (ok && rename(temp_path, main_path) != 0) {
        (void)rename(backup_path, main_path);
        ok = false;
    }
    if (ok) (void)remove(backup_path);
    if (!ok) (void)remove(temp_path);
    if (ok && static_cast<unsigned>(kind) <
                  sizeof(s_generation) / sizeof(s_generation[0])) {
        const unsigned generation_index = static_cast<unsigned>(kind);
        s_generation[generation_index] = s_generation[generation_index] + 1u;
    }
    xSemaphoreGive(s_mutex);
    if (!ok) ESP_LOGE(TAG, "failed to persist object kind %u",
                      static_cast<unsigned>(kind));
    return ok;
}

extern "C" bool SERVER_LIST_STORE_FindNrlServerName(
    const char *host, const uint16_t port, char *name, const size_t name_size)
{
    if (name == nullptr || name_size == 0u) return false;
    name[0] = '\0';
    if (host == nullptr || host[0] == '\0' || port == 0u) return false;
    char wanted_host[96] = {};
    normalizeNrlHost(host, wanted_host, sizeof(wanted_host));
    uint8_t *payload = nullptr;
    size_t payload_size = 0u;
    if (!SERVER_LIST_STORE_Read(SERVER_LIST_NRL, &payload, &payload_size))
        return false;
    const bool found = findNrlNameInJson(
        reinterpret_cast<const char *>(payload), payload_size, wanted_host,
        port, name, name_size);
    free(payload);
    return found;
}

extern "C" bool SERVER_LIST_STORE_LoadNrlServers(NrlServerInfo **servers,
                                                   size_t *count)
{
    if (servers == nullptr || count == nullptr) return false;
    *servers = nullptr;
    *count = 0u;
    uint8_t *payload = nullptr;
    size_t payload_size = 0u;
    if (!SERVER_LIST_STORE_Read(SERVER_LIST_NRL, &payload, &payload_size))
        return false;
    const char *json = reinterpret_cast<const char *>(payload);
    const size_t found = loadNrlServersFromJson(json, payload_size, nullptr, 0u);
    if (found == 0u || found > SIZE_MAX / sizeof(NrlServerInfo)) {
        free(payload);
        return false;
    }
    auto *result = static_cast<NrlServerInfo *>(heap_caps_calloc(
        found, sizeof(NrlServerInfo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (result == nullptr) {
        free(payload);
        return false;
    }
    const size_t loaded = loadNrlServersFromJson(json, payload_size,
                                                 result, found);
    free(payload);
    if (loaded == 0u) {
        free(result);
        return false;
    }
    *servers = result;
    *count = loaded < found ? loaded : found;
    return true;
}

extern "C" size_t SERVER_LIST_STORE_ValidateNrlJson(const char *json,
                                                       const size_t size)
{
    if (json == nullptr || size == 0u || size > kMaxPayload) return 0u;
    return loadNrlServersFromJson(json, size, nullptr, 0u);
}

extern "C" bool SERVER_LIST_STORE_RefreshNrlHttps(const char *host,
                                                    const uint16_t port,
                                                    size_t *count)
{
    if (count != nullptr) *count = 0u;
    char platform_host[96] = {};
    normalizeNrlHost(host, platform_host, sizeof(platform_host));
    if (!validHttpsAuthority(platform_host)) {
        ESP_LOGW(TAG, "invalid NRL platform host: %s", platform_host);
        return false;
    }
    char authority[96] = {};
    snprintf(authority, sizeof(authority), "%s", platform_host);
    uint8_t *cached = nullptr;
    size_t cached_size = 0u;
    if (SERVER_LIST_STORE_Read(SERVER_LIST_NRL, &cached, &cached_size)) {
        (void)findNrlPlatformAuthorityInJson(
            reinterpret_cast<const char *>(cached), cached_size, platform_host,
            port, authority, sizeof(authority));
        free(cached);
    }
    if (!validHttpsAuthority(authority)) return false;
    char *response = static_cast<char *>(heap_caps_malloc(
        kMaxPayload + 1u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (response == nullptr) {
        ESP_LOGW(TAG, "no PSRAM for NRL HTTPS directory");
        return false;
    }
    bool ok = false;
    size_t found = 0u;
    const auto download = [&](const char *source_authority) {
        char url[144] = {};
        snprintf(url, sizeof(url), "https://%s/platform/list", source_authority);
        esp_http_client_config_t config = {};
        config.url = url;
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = 10000;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.user_agent = "NRL-ESP32/server-directory";
        config.buffer_size = 2048;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        size_t received = 0u;
        bool downloaded = false;
        if (client != nullptr && esp_http_client_open(client, 0) == ESP_OK &&
            esp_http_client_fetch_headers(client) >= 0 &&
            esp_http_client_get_status_code(client) == 200) {
            while (received < kMaxPayload) {
                const int got = esp_http_client_read(
                    client, response + received, kMaxPayload - received);
                if (got < 0) {
                    received = 0u;
                    break;
                }
                if (got == 0) break;
                received += static_cast<size_t>(got);
            }
            response[received] = '\0';
            found = SERVER_LIST_STORE_ValidateNrlJson(response, received);
            downloaded = found > 0u && found <= 512u;
        }
        if (client != nullptr) esp_http_client_cleanup(client);
        if (downloaded &&
            SERVER_LIST_STORE_Write(SERVER_LIST_NRL, response, received)) {
            ESP_LOGI(TAG, "downloaded %u NRL servers from %s",
                     static_cast<unsigned>(found), url);
            return true;
        }
        ESP_LOGW(TAG, "NRL directory HTTPS request failed: %s", url);
        return false;
    };
    ok = download(authority);
    // Legacy installations may contain only a numeric UDP address, while
    // independently operated reporting endpoints can also use an untrusted
    // certificate. Keep the last cache and use the trusted main directory as
    // a bootstrap/fallback; normal domain configurations always try their own
    // endpoint first.
    if (!ok && strcasecmp(authority, "m.nrlptt.com") != 0) {
        ok = download("m.nrlptt.com");
    }
    if (ok && count != nullptr) *count = found;
    free(response);
    return ok;
}

extern "C" uint32_t SERVER_LIST_STORE_Generation(const ServerListKind kind)
{
    const unsigned index = static_cast<unsigned>(kind);
    return index < sizeof(s_generation) / sizeof(s_generation[0])
               ? s_generation[index] : 0u;
}
