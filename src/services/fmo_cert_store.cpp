#include "services/fmo_cert_store.h"

#include <string.h>

#include "services/fmo_station_broadcast_core.h"
#include "services/server_list_store.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sodium.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace {

constexpr size_t kCertificateMax = 24u * 1024u;

struct UserCertificate {
    char callsign[16];
    uint32_t uid;
    uint64_t iat;
    uint64_t exp;
    uint32_t issuer_sn;
    uint8_t public_key[32];
    uint8_t signature[64];
    uint8_t fingerprint[32];
};

struct DeviceKey {
    uint8_t seed[32];
    uint8_t public_key[32];
};

struct CborWriter {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool ok;
};

static portMUX_TYPE s_status_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_status_cache_valid = false;
static uint32_t s_status_cache_generation[3] = {};
static FmoIdentityStatus s_status_cache = {};
static esp_err_t s_status_cache_error = ESP_OK;

static void certificateGenerations(uint32_t generations[3])
{
    generations[0] = SERVER_LIST_STORE_Generation(SERVER_FILE_FMO_USER_CERT);
    generations[1] = SERVER_LIST_STORE_Generation(SERVER_FILE_FMO_INTERMEDIATE_CERT);
    generations[2] = SERVER_LIST_STORE_Generation(SERVER_FILE_FMO_DEVICE_KEY);
}

static bool cachedStatus(const uint32_t generations[3],
                         FmoIdentityStatus *status, esp_err_t *error)
{
    bool valid = false;
    portENTER_CRITICAL(&s_status_cache_lock);
    if (s_status_cache_valid &&
        memcmp(generations, s_status_cache_generation,
               sizeof(s_status_cache_generation)) == 0) {
        *status = s_status_cache;
        *error = s_status_cache_error;
        valid = true;
    }
    portEXIT_CRITICAL(&s_status_cache_lock);
    return valid;
}

static void storeCachedStatus(const uint32_t generations[3],
                              const FmoIdentityStatus &status,
                              const esp_err_t error)
{
    portENTER_CRITICAL(&s_status_cache_lock);
    memcpy(s_status_cache_generation, generations,
           sizeof(s_status_cache_generation));
    s_status_cache = status;
    s_status_cache_error = error;
    s_status_cache_valid = true;
    portEXIT_CRITICAL(&s_status_cache_lock);
}

static ServerListKind fileKind(const FmoCertificateKind kind)
{
    if (kind == FMO_CERT_USER) return SERVER_FILE_FMO_USER_CERT;
    if (kind == FMO_CERT_INTERMEDIATE)
        return SERVER_FILE_FMO_INTERMEDIATE_CERT;
    return SERVER_FILE_FMO_DEVICE_KEY;
}

static void setError(char *error, const size_t error_size, const char *message)
{
    if (error != nullptr && error_size > 0u) {
        snprintf(error, error_size, "%s", message);
    }
}

static bool numberU64(const cJSON *object, const char *name, uint64_t *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0 ||
        value->valuedouble > static_cast<double>(UINT64_MAX)) {
        return false;
    }
    const uint64_t parsed = static_cast<uint64_t>(value->valuedouble);
    if (static_cast<double>(parsed) != value->valuedouble) return false;
    *out = parsed;
    return true;
}

static bool decodeFixed(const char *text, uint8_t *out, const size_t expected)
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
        if (sodium_base642bin(out, expected, text, strlen(text), nullptr, &size,
                              nullptr, variant) == 0 && size == expected) {
            return true;
        }
    }
    return false;
}

static void cborBytes(CborWriter *writer, const void *data, const size_t size)
{
    if (!writer->ok || writer->size + size > writer->capacity) {
        writer->ok = false;
        return;
    }
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
}

static void cborHead(CborWriter *writer, const uint8_t major,
                     const uint64_t value)
{
    uint8_t encoded[9];
    size_t size = 1u;
    if (value < 24u) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | value);
    } else if (value <= UINT8_MAX) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 24u);
        encoded[1] = static_cast<uint8_t>(value);
        size = 2u;
    } else if (value <= UINT16_MAX) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 25u);
        encoded[1] = static_cast<uint8_t>(value >> 8u);
        encoded[2] = static_cast<uint8_t>(value);
        size = 3u;
    } else if (value <= UINT32_MAX) {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 26u);
        for (size_t i = 0; i < 4u; ++i) {
            encoded[1u + i] = static_cast<uint8_t>(value >> (24u - i * 8u));
        }
        size = 5u;
    } else {
        encoded[0] = static_cast<uint8_t>((major << 5u) | 27u);
        for (size_t i = 0; i < 8u; ++i) {
            encoded[1u + i] = static_cast<uint8_t>(value >> (56u - i * 8u));
        }
        size = 9u;
    }
    cborBytes(writer, encoded, size);
}

static void cborUint(CborWriter *writer, const uint64_t value)
{
    cborHead(writer, 0u, value);
}

static void cborText(CborWriter *writer, const char *text)
{
    const size_t size = strlen(text);
    cborHead(writer, 3u, size);
    cborBytes(writer, text, size);
}

static void cborBlob(CborWriter *writer, const uint8_t *data,
                     const size_t size)
{
    cborHead(writer, 2u, size);
    cborBytes(writer, data, size);
}

static bool buildUserTbs(const UserCertificate *cert, uint8_t *out,
                         const size_t capacity, size_t *out_size)
{
    CborWriter writer = {out, capacity, 0u, true};
    cborHead(&writer, 4u, 9u);
    cborText(&writer, "FMO");
    cborUint(&writer, 4u);
    cborText(&writer, "userCert");
    cborUint(&writer, cert->issuer_sn);
    cborText(&writer, cert->callsign);
    cborUint(&writer, cert->uid);
    cborBlob(&writer, cert->public_key, sizeof(cert->public_key));
    cborUint(&writer, cert->iat);
    cborUint(&writer, cert->exp);
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

static bool parseUser(const cJSON *root, UserCertificate *cert)
{
    memset(cert, 0, sizeof(*cert));
    uint64_t issuer = 0u, uid = 0u;
    const cJSON *subject = cJSON_GetObjectItemCaseSensitive(root, "subject");
    const cJSON *callsign = cJSON_GetObjectItemCaseSensitive(subject, "callsign");
    const cJSON *public_key = cJSON_GetObjectItemCaseSensitive(subject, "publicKey");
    const cJSON *signature = cJSON_GetObjectItemCaseSensitive(root, "signature");
    if (!cJSON_IsObject(root) || !cJSON_IsObject(subject) ||
        !cJSON_IsString(callsign) || callsign->valuestring[0] == '\0' ||
        strlen(callsign->valuestring) >= sizeof(cert->callsign) ||
        !cJSON_IsString(public_key) || !cJSON_IsString(signature) ||
        !numberU64(root, "issuerSn", &issuer) || issuer > UINT32_MAX ||
        !numberU64(subject, "uid", &uid) || uid > UINT32_MAX ||
        !numberU64(root, "iat", &cert->iat) ||
        !numberU64(root, "exp", &cert->exp) || cert->exp <= cert->iat ||
        !decodeFixed(public_key->valuestring, cert->public_key, 32u) ||
        !decodeFixed(signature->valuestring, cert->signature, 64u)) {
        return false;
    }
    for (const char *p = callsign->valuestring; *p != '\0'; ++p) {
        if (!isalnum(static_cast<unsigned char>(*p)) && *p != '-') return false;
    }
    snprintf(cert->callsign, sizeof(cert->callsign), "%s", callsign->valuestring);
    cert->uid = static_cast<uint32_t>(uid);
    cert->issuer_sn = static_cast<uint32_t>(issuer);
    uint8_t tbs[256];
    size_t tbs_size = 0u;
    if (!buildUserTbs(cert, tbs, sizeof(tbs), &tbs_size)) return false;
    crypto_hash_sha256(cert->fingerprint, tbs, tbs_size);
    return true;
}

static bool parseKey(const cJSON *root, DeviceKey *key)
{
    const cJSON *seed = cJSON_GetObjectItemCaseSensitive(root, "seed");
    const cJSON *public_key = cJSON_GetObjectItemCaseSensitive(root, "pubKey");
    return cJSON_IsObject(root) && cJSON_IsString(seed) &&
           cJSON_IsString(public_key) &&
           decodeFixed(seed->valuestring, key->seed, 32u) &&
           decodeFixed(public_key->valuestring, key->public_key, 32u);
}

static bool parseIntermediate(const cJSON *root, uint8_t public_key[32])
{
    const cJSON *subject = cJSON_GetObjectItemCaseSensitive(root, "subject");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(subject, "publicKey");
    return cJSON_IsObject(root) && cJSON_IsObject(subject) &&
           cJSON_IsString(key) && decodeFixed(key->valuestring, public_key, 32u);
}

static bool validateCertificate(const FmoCertificateKind kind,
                                const cJSON *root)
{
    if (kind == FMO_CERT_USER) {
        UserCertificate cert = {};
        return parseUser(root, &cert);
    }
    if (kind == FMO_CERT_DEVICE_KEY) {
        DeviceKey key = {};
        return parseKey(root, &key);
    }
    uint8_t public_key[32] = {};
    return parseIntermediate(root, public_key);
}

static bool storePresent(const FmoCertificateKind kind)
{
    uint8_t *data = nullptr;
    size_t size = 0u;
    const bool present = SERVER_LIST_STORE_Read(fileKind(kind), &data, &size);
    free(data);
    return present && size > 0u && size <= kCertificateMax;
}

static esp_err_t readJson(const FmoCertificateKind kind, uint8_t **raw,
                          size_t *size, cJSON **json)
{
    if (!SERVER_LIST_STORE_Read(fileKind(kind), raw, size))
        return ESP_ERR_NOT_FOUND;
    if (*size == 0u || *size > kCertificateMax) {
        free(*raw);
        *raw = nullptr;
        *size = 0u;
        return ESP_ERR_INVALID_SIZE;
    }
    *json = cJSON_ParseWithLength(reinterpret_cast<const char *>(*raw), *size);
    if (*json == nullptr || !validateCertificate(kind, *json)) {
        cJSON_Delete(*json);
        *json = nullptr;
        free(*raw);
        *raw = nullptr;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool buildProofTbs(const FmoServer *server, const char *role,
                          const uint64_t timestamp,
                          const UserCertificate *cert, uint8_t *out,
                          const size_t capacity, size_t *out_size)
{
    char upper[sizeof(server->callsign)];
    snprintf(upper, sizeof(upper), "%s", server->callsign);
    for (char *p = upper; *p != '\0'; ++p) {
        *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
    }
    CborWriter writer = {out, capacity, 0u, true};
    cborHead(&writer, 4u, 12u);
    cborText(&writer, "FMO");
    cborUint(&writer, 4u);
    cborText(&writer, "serverAuthorizerReqHttp");
    cborUint(&writer, server->uid);
    cborText(&writer, upper);
    cborUint(&writer, server->uid);
    cborText(&writer, role);
    cborText(&writer, server->host);
    cborUint(&writer, server->port);
    cborBlob(&writer, server->fingerprint, 32u);
    cborUint(&writer, timestamp);
    cborBlob(&writer, cert->fingerprint, 32u);
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

static char *base64UrlAlloc(const uint8_t *data, const size_t size)
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

} // namespace

extern "C" esp_err_t FMO_CERT_Put(const FmoCertificateKind kind,
                                  const char *json, const size_t json_size,
                                  char *error, const size_t error_size)
{
    if (sodium_init() < 0) {
        setError(error, error_size, "libsodium initialization failed");
        return ESP_ERR_INVALID_STATE;
    }
    if (kind < FMO_CERT_USER || kind > FMO_CERT_DEVICE_KEY || json == nullptr ||
        json_size == 0u || json_size > kCertificateMax) {
        setError(error, error_size, "empty or oversized certificate JSON");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength(json, json_size);
    if (root == nullptr) {
        setError(error, error_size, "invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }
    const bool valid = validateCertificate(kind, root);
    cJSON_Delete(root);
    if (!valid) {
        setError(error, error_size, "invalid certificate fields or Base64 data");
        return ESP_ERR_INVALID_ARG;
    }
    if (!SERVER_LIST_STORE_Write(fileKind(kind), json, json_size)) {
        setError(error, error_size, "certificate filesystem write failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" esp_err_t FMO_CERT_GetStatus(FmoIdentityStatus *status)
{
    if (status == nullptr) return ESP_ERR_INVALID_ARG;
    uint32_t generations[3] = {};
    certificateGenerations(generations);
    esp_err_t cached_error = ESP_OK;
    if (cachedStatus(generations, status, &cached_error)) return cached_error;
    memset(status, 0, sizeof(*status));
    status->user_present = storePresent(FMO_CERT_USER);
    status->intermediate_present = storePresent(FMO_CERT_INTERMEDIATE);
    status->device_key_present = storePresent(FMO_CERT_DEVICE_KEY);
    if (!status->user_present || !status->intermediate_present ||
        !status->device_key_present) {
        storeCachedStatus(generations, *status, ESP_OK);
        return ESP_OK;
    }

    uint8_t *user_raw = nullptr, *int_raw = nullptr, *key_raw = nullptr;
    size_t user_size = 0u, int_size = 0u, key_size = 0u;
    cJSON *user_json = nullptr, *int_json = nullptr, *key_json = nullptr;
    esp_err_t result = readJson(FMO_CERT_USER, &user_raw, &user_size, &user_json);
    if (result != ESP_OK) goto done;
    result = readJson(FMO_CERT_INTERMEDIATE, &int_raw, &int_size, &int_json);
    if (result != ESP_OK) goto done;
    result = readJson(FMO_CERT_DEVICE_KEY, &key_raw, &key_size, &key_json);
    if (result != ESP_OK) goto done;
    {
        UserCertificate cert = {};
        DeviceKey key = {};
        uint8_t intermediate_key[32], derived_public[32], secret[64];
        // Ed25519 operations are CPU-heavy on S31. Give the idle task a turn
        // between validation stages so first boot cannot starve task_wdt.
        vTaskDelay(1);
        if (!parseUser(user_json, &cert) || !parseKey(key_json, &key) ||
            !parseIntermediate(int_json, intermediate_key) ||
            crypto_sign_seed_keypair(derived_public, secret, key.seed) != 0 ||
            sodium_memcmp(derived_public, key.public_key, 32u) != 0 ||
            sodium_memcmp(derived_public, cert.public_key, 32u) != 0) {
            sodium_memzero(secret, sizeof(secret));
            result = ESP_ERR_INVALID_CRC;
            goto done;
        }
        vTaskDelay(1);
        uint8_t tbs[256];
        size_t tbs_size = 0u;
        if (!buildUserTbs(&cert, tbs, sizeof(tbs), &tbs_size) ||
            crypto_sign_verify_detached(cert.signature, tbs, tbs_size,
                                        intermediate_key) != 0) {
            sodium_memzero(secret, sizeof(secret));
            result = ESP_ERR_INVALID_CRC;
            goto done;
        }
        vTaskDelay(1);
        sodium_memzero(secret, sizeof(secret));
        const time_t now = time(nullptr);
        if (now >= 1700000000 &&
            (static_cast<uint64_t>(now) < cert.iat ||
             static_cast<uint64_t>(now) >= cert.exp)) {
            result = ESP_ERR_INVALID_STATE;
            goto done;
        }
        snprintf(status->callsign, sizeof(status->callsign), "%s", cert.callsign);
        status->uid = cert.uid;
        status->issued_at = cert.iat;
        status->expires_at = cert.exp;
        memcpy(status->fingerprint, cert.fingerprint, 32u);
        status->ready = true;
        result = ESP_OK;
    }

done:
    cJSON_Delete(user_json);
    cJSON_Delete(int_json);
    cJSON_Delete(key_json);
    free(user_raw);
    free(int_raw);
    free(key_raw);
    storeCachedStatus(generations, *status, result);
    return result;
}

extern "C" esp_err_t FMO_CERT_BuildCredentials(
    const FmoServer *server, const char *role, char *username,
    const size_t username_size, char **password)
{
    if (server == nullptr || role == nullptr || username == nullptr ||
        username_size == 0u || password == nullptr ||
        !server->has_fingerprint || server->uid == 0u ||
        server->host[0] == '\0' || server->callsign[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *password = nullptr;
    FmoIdentityStatus status = {};
    esp_err_t result = FMO_CERT_GetStatus(&status);
    if (result != ESP_OK || !status.ready) {
        return result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(1);

    uint8_t *user_raw = nullptr, *int_raw = nullptr, *key_raw = nullptr;
    size_t user_size = 0u, int_size = 0u, key_size = 0u;
    cJSON *user_json = nullptr, *int_json = nullptr, *key_json = nullptr;
    result = readJson(FMO_CERT_USER, &user_raw, &user_size, &user_json);
    if (result != ESP_OK) goto done;
    result = readJson(FMO_CERT_INTERMEDIATE, &int_raw, &int_size, &int_json);
    if (result != ESP_OK) goto done;
    result = readJson(FMO_CERT_DEVICE_KEY, &key_raw, &key_size, &key_json);
    if (result != ESP_OK) goto done;
    {
        UserCertificate cert = {};
        DeviceKey key = {};
        if (!parseUser(user_json, &cert) || !parseKey(key_json, &key)) {
            result = ESP_ERR_INVALID_ARG;
            goto done;
        }
        const uint64_t timestamp = static_cast<uint64_t>(time(nullptr));
        if (timestamp < 1700000000ULL) {
            result = ESP_ERR_INVALID_STATE;
            goto done;
        }
        uint8_t tbs[512], secret[64], public_key[32], signature[64];
        size_t tbs_size = 0u;
        vTaskDelay(1);
        if (!buildProofTbs(server, role, timestamp, &cert, tbs, sizeof(tbs),
                           &tbs_size) ||
            crypto_sign_seed_keypair(public_key, secret, key.seed) != 0 ||
            crypto_sign_detached(signature, nullptr, tbs, tbs_size, secret) != 0) {
            sodium_memzero(secret, sizeof(secret));
            result = ESP_FAIL;
            goto done;
        }
        vTaskDelay(1);
        sodium_memzero(secret, sizeof(secret));
        char *server_fp = base64UrlAlloc(server->fingerprint, 32u);
        char *signature_text = base64UrlAlloc(signature, 64u);
        if (server_fp == nullptr || signature_text == nullptr) {
            free(server_fp);
            free(signature_text);
            result = ESP_ERR_NO_MEM;
            goto done;
        }
        cJSON *payload_json = cJSON_CreateObject();
        cJSON *package = cJSON_CreateObject();
        cJSON *proof = cJSON_CreateObject();
        cJSON *int_copy = cJSON_Duplicate(int_json, true);
        cJSON *user_copy = cJSON_Duplicate(user_json, true);
        if (payload_json == nullptr || package == nullptr || proof == nullptr ||
            int_copy == nullptr || user_copy == nullptr) {
            cJSON_Delete(payload_json);
            cJSON_Delete(package);
            cJSON_Delete(proof);
            cJSON_Delete(int_copy);
            cJSON_Delete(user_copy);
            free(server_fp);
            free(signature_text);
            result = ESP_ERR_NO_MEM;
            goto done;
        }
        cJSON_AddItemToObject(package, "intermediateCert", int_copy);
        cJSON_AddItemToObject(package, "userCert", user_copy);
        cJSON_AddItemToObject(payload_json, "certPackage", package);
        cJSON_AddStringToObject(payload_json, "targetCallsign", server->callsign);
        cJSON_AddNumberToObject(payload_json, "targetUID", server->uid);
        cJSON_AddStringToObject(payload_json, "role", role);
        cJSON_AddStringToObject(payload_json, "targetUrl", server->host);
        cJSON_AddNumberToObject(payload_json, "targetPort", server->port);
        cJSON_AddStringToObject(payload_json, "serverFingerprint", server_fp);
        cJSON_AddNumberToObject(payload_json, "timestamp",
                                static_cast<double>(timestamp));
        cJSON_AddStringToObject(proof, "signature", signature_text);
        cJSON_AddItemToObject(payload_json, "proof", proof);
        free(server_fp);
        free(signature_text);
        char *payload = cJSON_PrintUnformatted(payload_json);
        cJSON_Delete(payload_json);
        if (payload == nullptr) {
            result = ESP_ERR_NO_MEM;
            goto done;
        }
        *password = base64UrlAlloc(reinterpret_cast<const uint8_t *>(payload),
                                   strlen(payload));
        cJSON_free(payload);
        if (*password == nullptr) {
            result = ESP_ERR_NO_MEM;
            goto done;
        }
        snprintf(username, username_size, "%s", cert.callsign);
        result = ESP_OK;
    }

done:
    cJSON_Delete(user_json);
    cJSON_Delete(int_json);
    cJSON_Delete(key_json);
    free(user_raw);
    free(int_raw);
    free(key_raw);
    return result;
}

extern "C" esp_err_t FMO_CERT_RebuildUserCertBlob(uint8_t *out,
                                                  const size_t capacity,
                                                  size_t *out_size)
{
    if (out == nullptr || out_size == nullptr) return ESP_ERR_INVALID_ARG;
    FmoIdentityStatus status = {};
    esp_err_t result = FMO_CERT_GetStatus(&status);
    if (result != ESP_OK || !status.ready) {
        return result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
    }
    uint8_t *raw = nullptr;
    size_t size = 0u;
    cJSON *json = nullptr;
    result = readJson(FMO_CERT_USER, &raw, &size, &json);
    if (result != ESP_OK) return result;
    UserCertificate cert = {};
    FmoStationCertFields fields = {};
    if (parseUser(json, &cert)) {
        fields.issuer_sn = cert.issuer_sn;
        snprintf(fields.callsign, sizeof(fields.callsign), "%s", cert.callsign);
        fields.uid = cert.uid;
        memcpy(fields.public_key, cert.public_key, sizeof(fields.public_key));
        fields.issued_at = cert.iat;
        fields.expires_at = cert.exp;
        memcpy(fields.signature, cert.signature, sizeof(fields.signature));
        if (!FMO_STATION_CORE_BuildCertBlob(&fields, out, capacity, out_size)) {
            result = ESP_ERR_INVALID_SIZE;
        }
    } else {
        result = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(json);
    free(raw);
    return result;
}

extern "C" esp_err_t FMO_CERT_SignWithDeviceKey(const uint8_t *tbs,
                                                const size_t tbs_size,
                                                uint8_t signature[64])
{
    if (tbs == nullptr || tbs_size == 0u || signature == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    FmoIdentityStatus status = {};
    esp_err_t result = FMO_CERT_GetStatus(&status);
    if (result != ESP_OK || !status.ready) {
        return result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
    }
    uint8_t *raw = nullptr;
    size_t size = 0u;
    cJSON *json = nullptr;
    result = readJson(FMO_CERT_DEVICE_KEY, &raw, &size, &json);
    if (result != ESP_OK) return result;
    DeviceKey key = {};
    uint8_t secret[64], public_key[32];
    if (parseKey(json, &key) &&
        crypto_sign_seed_keypair(public_key, secret, key.seed) == 0 &&
        crypto_sign_detached(signature, nullptr, tbs, tbs_size, secret) == 0) {
        result = ESP_OK;
    } else {
        result = ESP_FAIL;
    }
    sodium_memzero(secret, sizeof(secret));
    cJSON_Delete(json);
    free(raw);
    return result;
}
