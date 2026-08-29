#include "services/fmo_activate.h"

#include "../lib/nrl_net_compat.h"
#include "../lib/nrl_version.h"
#include "services/config_notify.h"
#include "services/fmo_activate_core.h"
#include "services/fmo_cert_store.h"
#include "services/fmo_service.h"
#include "services/server_list_store.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <nvs.h>
#include <sodium.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

constexpr const char *TAG = "FMO_ACT";
constexpr const char *kNvsNamespace = "fmoact";
constexpr const char *kNvsHostKey = "host";
constexpr const char *kDefaultHost = "www.hamptt.com";
constexpr const char *kActivatePath = "/api/device/activate";
constexpr size_t kDeviceKeyJsonMax = 1024u;
constexpr size_t kResponseCapacity = 32u * 1024u;

char s_last[128] = {};
uint64_t s_last_epoch = 0;

void setMessage(char *message, const size_t message_size, const char *text)
{
    if (message != nullptr && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

void recordStatus(const char *text)
{
    snprintf(s_last, sizeof(s_last), "%s", text);
    s_last_epoch = static_cast<uint64_t>(time(nullptr));
}

// Same base64 flexibility as fmo_cert_store.cpp's decodeFixed (URL-safe and
// original alphabets, with or without padding).
bool decodeFixed(const char *text, uint8_t *out, const size_t expected)
{
    if (text == nullptr) return false;
    const int variants[] = {
        sodium_base64_VARIANT_URLSAFE_NO_PADDING,
        sodium_base64_VARIANT_URLSAFE,
        sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
        sodium_base64_VARIANT_ORIGINAL,
    };
    for (const int variant : variants) {
        size_t size = 0u;
        if (sodium_base642bin(out, expected, text, strlen(text), nullptr,
                              &size, nullptr, variant) == 0 &&
            size == expected) {
            return true;
        }
    }
    return false;
}

bool hexDecode32(const char *hex, uint8_t out[32])
{
    if (hex == nullptr || strlen(hex) != 64u) return false;
    for (size_t i = 0; i < 32u; ++i) {
        unsigned value = 0u;
        if (sscanf(hex + i * 2u, "%2x", &value) != 1) return false;
        out[i] = static_cast<uint8_t>(value);
    }
    return true;
}

char *base64UrlAlloc(const uint8_t *data, const size_t size)
{
    const size_t capacity = sodium_base64_ENCODED_LEN(
        size, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char *encoded = static_cast<char *>(malloc(capacity));
    if (encoded != nullptr) {
        sodium_bin2base64(encoded, capacity, data, size,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    }
    return encoded;
}

// Loads the device Ed25519 key from the cert store, or generates and stores a
// fresh one on first activation. The secret never leaves the device; only the
// 32-byte seed is persisted (inside the deviceKey JSON on LittleFS).
esp_err_t ensureDeviceKey(uint8_t seed[32], uint8_t public_key[32],
                          char *message, const size_t message_size)
{
    uint8_t *raw = nullptr;
    size_t size = 0u;
    if (SERVER_LIST_STORE_Read(SERVER_FILE_FMO_DEVICE_KEY, &raw, &size)) {
        esp_err_t result = ESP_ERR_INVALID_ARG;
        cJSON *json = size > 0u && size <= kDeviceKeyJsonMax
                          ? cJSON_ParseWithLength(
                                reinterpret_cast<const char *>(raw), size)
                          : nullptr;
        if (json != nullptr) {
            const cJSON *seed_field =
                cJSON_GetObjectItemCaseSensitive(json, "seed");
            const cJSON *pub_field =
                cJSON_GetObjectItemCaseSensitive(json, "pubKey");
            if (cJSON_IsString(seed_field) && cJSON_IsString(pub_field) &&
                decodeFixed(seed_field->valuestring, seed, 32u) &&
                decodeFixed(pub_field->valuestring, public_key, 32u)) {
                result = ESP_OK;
            }
            cJSON_Delete(json);
        }
        free(raw);
        if (result == ESP_OK) return ESP_OK;
        setMessage(message, message_size, "设备密钥文件损坏，请手动上传 deviceKey JSON");
        return result;
    }

    randombytes_buf(seed, 32u);
    uint8_t secret[crypto_sign_SECRETKEYBYTES];
    if (crypto_sign_seed_keypair(public_key, secret, seed) != 0) {
        setMessage(message, message_size, "设备密钥生成失败");
        return ESP_FAIL;
    }
    sodium_memzero(secret, sizeof(secret));
    char *seed_text = base64UrlAlloc(seed, 32u);
    char *pub_text = base64UrlAlloc(public_key, 32u);
    if (seed_text == nullptr || pub_text == nullptr) {
        free(seed_text);
        free(pub_text);
        setMessage(message, message_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    char key_json[192];
    const int written = snprintf(key_json, sizeof(key_json),
                                 "{\"type\":\"deviceKey\",\"seed\":\"%s\","
                                 "\"pubKey\":\"%s\"}",
                                 seed_text, pub_text);
    free(seed_text);
    free(pub_text);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(key_json)) {
        setMessage(message, message_size, "设备密钥 JSON 组装失败");
        return ESP_FAIL;
    }
    const esp_err_t result =
        FMO_CERT_Put(FMO_CERT_DEVICE_KEY, key_json,
                     static_cast<size_t>(written), message, message_size);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "generated a new device key");
    }
    return result;
}

const char *codeMessage(const long code)
{
    switch (code) {
    case 1: return "本机 MAC 未绑定用户：请先在 hamptt.com 登记并绑定本机 MAC";
    case 2: return "本机 MAC 未登记：请先在 hamptt.com 登记本机 MAC";
    case 3: return "时间戳误差过大：请检查设备时间（SNTP）同步";
    case 4: return "请求被判为重放：请稍后重试";
    case 5: return "请求格式错误";
    case 6: return "国家码受限";
    case 7: return "设备或用户已被封禁";
    case 8: return "超出申请频率限制（每 MAC 每小时 5 次）：请稍后重试";
    case 9: return "设备签名验证失败";
    case 10: return "平台 CA 未配置";
    case 100: return "已转人工审核：请审核通过后重试";
    default: return nullptr;
    }
}

esp_err_t runInner(char *message, const size_t message_size)
{
    if (sodium_init() < 0) {
        setMessage(message, message_size, "libsodium 初始化失败");
        return ESP_ERR_INVALID_STATE;
    }
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        // Same SNTP-synced criterion as fmo_cert_store.cpp's validity check.
        setMessage(message, message_size,
                   "设备时间未同步（SNTP），请联网稍后再试");
        return ESP_ERR_INVALID_STATE;
    }
    if (!nrlNetworkConnected()) {
        setMessage(message, message_size, "网络未连接");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t seed[32], public_key[32];
    esp_err_t result =
        ensureDeviceKey(seed, public_key, message, message_size);
    if (result != ESP_OK) return result;

    FmoActivateRequest request = {};
    esp_read_mac(request.mac, ESP_MAC_WIFI_STA);
    request.timestamp = static_cast<uint64_t>(now);
    esp_fill_random(request.nonce, sizeof(request.nonce));
    request.firmware_version = NRL_FIRMWARE_VERSION;
    char elf_sha[65] = {};
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    bool hash_ok = hexDecode32(elf_sha, request.firmware_hash);
    if (!hash_ok) {
        // sdkconfigs predating CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64 only store
        // 9 hex chars; fall back to the running partition's image SHA-256.
        const esp_partition_t *running = esp_ota_get_running_partition();
        hash_ok = running != nullptr &&
                  esp_partition_get_sha256(running, request.firmware_hash) ==
                      ESP_OK;
    }
    if (!hash_ok) {
        setMessage(message, message_size, "固件哈希读取失败");
        return ESP_FAIL;
    }
    request.country_code = "CN";
    memcpy(request.device_public_key, public_key, sizeof(public_key));

    uint8_t cbor[256];
    size_t cbor_size = 0u;
    if (!FMO_ACTIVATE_CORE_BuildRequestCbor(&request, cbor, sizeof(cbor),
                                            &cbor_size)) {
        setMessage(message, message_size, "激活请求编码失败");
        return ESP_FAIL;
    }
    uint8_t secret[crypto_sign_SECRETKEYBYTES], derived_public[32];
    if (crypto_sign_seed_keypair(derived_public, secret, seed) != 0 ||
        sodium_memcmp(derived_public, public_key, sizeof(public_key)) != 0 ||
        crypto_sign_detached(request.signature, nullptr, cbor, cbor_size,
                             secret) != 0) {
        sodium_memzero(secret, sizeof(secret));
        setMessage(message, message_size, "设备签名失败");
        return ESP_FAIL;
    }
    sodium_memzero(secret, sizeof(secret));

    char *body = FMO_ACTIVATE_CORE_BuildRequestJson(&request);
    if (body == nullptr) {
        setMessage(message, message_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    char host[FMO_ACTIVATE_HOST_MAX + 1] = {};
    FMO_ACTIVATE_GetHost(host, sizeof(host));
    char url[192];
    if (strncmp(host, "http://", 7u) == 0 ||
        strncmp(host, "https://", 8u) == 0) {
        snprintf(url, sizeof(url), "%s%s", host, kActivatePath);
    } else {
        snprintf(url, sizeof(url), "https://%s%s", host, kActivatePath);
    }
    ESP_LOGI(TAG, "activate POST %s", url);

    void *response_memory = heap_caps_malloc(
        kResponseCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response_memory == nullptr) {
        response_memory = heap_caps_malloc(kResponseCapacity, MALLOC_CAP_8BIT);
    }
    char *response = static_cast<char *>(response_memory);
    if (response == nullptr) {
        free(body);
        setMessage(message, message_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    // Same esp_http_client POST pattern as ota_service.cpp's release check.
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    int total = 0;
    int status_code = 0;
    if (client != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (esp_http_client_open(client, strlen(body)) == ESP_OK &&
            esp_http_client_write(client, body, strlen(body)) ==
                static_cast<int>(strlen(body)) &&
            esp_http_client_fetch_headers(client) >= 0) {
            status_code = esp_http_client_get_status_code(client);
            while (total < static_cast<int>(kResponseCapacity - 1u)) {
                const int n = esp_http_client_read(
                    client, response + total,
                    kResponseCapacity - 1u - static_cast<size_t>(total));
                if (n <= 0) break;
                total += n;
            }
            response[total] = '\0';
        }
        esp_http_client_cleanup(client);
    }
    free(body);
    if (client == nullptr || total <= 0 || status_code != 200) {
        free(response);
        snprintf(message, message_size, "激活请求失败（HTTP %d）", status_code);
        return ESP_FAIL;
    }

    FmoActivateResult parsed = {};
    if (!FMO_ACTIVATE_CORE_ParseResponse(response, static_cast<size_t>(total),
                                         &parsed)) {
        free(response);
        setMessage(message, message_size, "激活响应解析失败");
        return ESP_FAIL;
    }
    free(response);

    if (strcmp(parsed.result, "ok") != 0) {
        const char *text = codeMessage(parsed.code);
        if (text != nullptr) {
            snprintf(message, message_size, "%s", text);
        } else if (parsed.reason[0] != '\0') {
            snprintf(message, message_size, "激活失败 code=%ld：%s",
                     parsed.code, parsed.reason);
        } else {
            snprintf(message, message_size, "激活失败 code=%ld", parsed.code);
        }
        ESP_LOGW(TAG, "activate result=%s code=%ld reason=%s", parsed.result,
                 parsed.code, parsed.reason);
        FMO_ACTIVATE_CORE_FreeResult(&parsed);
        return ESP_FAIL;
    }

    result = FMO_CERT_Put(FMO_CERT_USER, parsed.user_cert,
                          strlen(parsed.user_cert), message, message_size);
    if (result == ESP_OK) {
        result = FMO_CERT_Put(FMO_CERT_INTERMEDIATE, parsed.intermediate_cert,
                              strlen(parsed.intermediate_cert), message,
                              message_size);
    }
    FMO_ACTIVATE_CORE_FreeResult(&parsed);
    if (result != ESP_OK) return result;

    // Reconnect the FMO link with the new identity (same as the manual
    // certificate upload handler).
    FmoConfig fmo = {};
    FMO_GetConfig(&fmo);
    (void)FMO_SetConfig(&fmo, false);

    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) == ESP_OK && identity.ready) {
        snprintf(message, message_size, "OK 已获取证书：%s / UID %u",
                 identity.callsign, static_cast<unsigned>(identity.uid));
    } else {
        setMessage(message, message_size, "OK 证书已写入");
    }
    ESP_LOGI(TAG, "%s", message);
    return ESP_OK;
}

} // namespace

extern "C" void FMO_ACTIVATE_GetHost(char *out, const size_t out_size)
{
    if (out == nullptr || out_size == 0u) return;
    snprintf(out, out_size, "%s", kDefaultHost);
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = out_size;
    if (nvs_get_str(nvs, kNvsHostKey, out, &size) != ESP_OK || out[0] == '\0') {
        snprintf(out, out_size, "%s", kDefaultHost);
    }
    nvs_close(nvs);
}

extern "C" bool FMO_ACTIVATE_SetHost(const char *host)
{
    if (host == nullptr) return false;
    while (isspace(static_cast<unsigned char>(*host))) ++host;
    const size_t length = strlen(host);
    if (length == 0u || length > FMO_ACTIVATE_HOST_MAX) return false;
    char cleaned[FMO_ACTIVATE_HOST_MAX + 1];
    snprintf(cleaned, sizeof(cleaned), "%s", host);
    size_t end = length;
    while (end > 0u && isspace(static_cast<unsigned char>(cleaned[end - 1u]))) {
        cleaned[--end] = '\0';
    }
    if (end == 0u) return false;
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_str(nvs, kNvsHostKey, cleaned) == ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    if (ok) CONFIG_NOTIFY_Bump();
    return ok;
}

extern "C" void FMO_ACTIVATE_GetStatus(char *last, const size_t last_size,
                                       uint64_t *last_epoch)
{
    if (last != nullptr && last_size > 0u) {
        snprintf(last, last_size, "%s", s_last);
    }
    if (last_epoch != nullptr) *last_epoch = s_last_epoch;
}

extern "C" esp_err_t FMO_ACTIVATE_Run(char *message,
                                      const size_t message_size)
{
    const esp_err_t result = runInner(message, message_size);
    if (message != nullptr && message[0] != '\0') {
        recordStatus(message);
    }
    return result;
}
