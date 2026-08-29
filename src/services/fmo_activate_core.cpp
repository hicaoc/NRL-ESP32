#include "services/fmo_activate_core.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Deterministic CBOR writer with the exact semantics of the one in
// fmo_cert_store.cpp (shortest-form length encoding, major types 0/2/3/4).
struct CborWriter {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool ok;
};

void cborBytes(CborWriter *writer, const void *data, const size_t size)
{
    if (!writer->ok || writer->size + size > writer->capacity) {
        writer->ok = false;
        return;
    }
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
}

void cborHead(CborWriter *writer, const uint8_t major, const uint64_t value)
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

void cborUint(CborWriter *writer, const uint64_t value)
{
    cborHead(writer, 0u, value);
}

void cborText(CborWriter *writer, const char *text)
{
    const size_t size = strlen(text);
    cborHead(writer, 3u, size);
    cborBytes(writer, text, size);
}

void cborBlob(CborWriter *writer, const uint8_t *data, const size_t size)
{
    cborHead(writer, 2u, size);
    cborBytes(writer, data, size);
}

void hexEncode(const uint8_t *data, const size_t size, char *out,
               const bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2u] = digits[data[i] >> 4u];
        out[i * 2u + 1u] = digits[data[i] & 15u];
    }
    out[size * 2u] = '\0';
}

} // namespace

extern "C" bool FMO_ACTIVATE_CORE_BuildRequestCbor(
    const FmoActivateRequest *request, uint8_t *out, const size_t capacity,
    size_t *out_size)
{
    if (request == nullptr || request->firmware_version == nullptr ||
        request->country_code == nullptr || out == nullptr ||
        out_size == nullptr) {
        return false;
    }
    CborWriter writer = {out, capacity, 0u, true};
    cborHead(&writer, 4u, 10u);
    cborText(&writer, "FMO");
    cborUint(&writer, 4u);
    cborText(&writer, "activateReq");
    cborBlob(&writer, request->mac, sizeof(request->mac));
    cborUint(&writer, request->timestamp);
    cborBlob(&writer, request->nonce, sizeof(request->nonce));
    cborText(&writer, request->firmware_version);
    cborBlob(&writer, request->firmware_hash, sizeof(request->firmware_hash));
    cborText(&writer, request->country_code);
    cborBlob(&writer, request->device_public_key,
             sizeof(request->device_public_key));
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

extern "C" char *FMO_ACTIVATE_CORE_BuildRequestJson(
    const FmoActivateRequest *request)
{
    if (request == nullptr || request->firmware_version == nullptr ||
        request->country_code == nullptr) {
        return nullptr;
    }
    char mac[13], nonce[13], hash[65], pub[65], sig[129];
    hexEncode(request->mac, sizeof(request->mac), mac, true);
    hexEncode(request->nonce, sizeof(request->nonce), nonce, false);
    hexEncode(request->firmware_hash, sizeof(request->firmware_hash), hash,
              false);
    hexEncode(request->device_public_key, sizeof(request->device_public_key),
              pub, false);
    hexEncode(request->signature, sizeof(request->signature), sig, false);

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) return nullptr;
    cJSON_AddStringToObject(root, "version", "4");
    cJSON_AddStringToObject(root, "action", "activate");
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "timestamp",
                            static_cast<double>(request->timestamp));
    cJSON_AddStringToObject(root, "firmwareVersion",
                            request->firmware_version);
    cJSON_AddStringToObject(root, "firmwareHash", hash);
    cJSON_AddStringToObject(root, "countryCode", request->country_code);
    cJSON_AddStringToObject(root, "devicePublicKey", pub);
    cJSON_AddStringToObject(root, "nonce", nonce);
    cJSON_AddStringToObject(root, "signature", sig);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

extern "C" bool FMO_ACTIVATE_CORE_ParseResponse(const char *json,
                                                const size_t json_size,
                                                FmoActivateResult *result)
{
    if (json == nullptr || json_size == 0u || result == nullptr) return false;
    memset(result, 0, sizeof(*result));
    cJSON *root = cJSON_ParseWithLength(json, json_size);
    if (root == nullptr) return false;
    bool ok = false;
    const cJSON *result_field = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsString(result_field) && cJSON_IsNumber(code)) {
        snprintf(result->result, sizeof(result->result), "%s",
                 result_field->valuestring);
        result->code = code->valueint;
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        if (cJSON_IsString(reason)) {
            snprintf(result->reason, sizeof(result->reason), "%s",
                     reason->valuestring);
        }
        const cJSON *server_time =
            cJSON_GetObjectItemCaseSensitive(root, "serverTime");
        if (cJSON_IsNumber(server_time) && server_time->valuedouble >= 0.0) {
            result->server_time =
                static_cast<uint64_t>(server_time->valuedouble);
        }
        ok = true;
        if (strcmp(result->result, "ok") == 0) {
            const cJSON *package =
                cJSON_GetObjectItemCaseSensitive(root, "certPackage");
            const cJSON *user =
                cJSON_GetObjectItemCaseSensitive(package, "userCert");
            const cJSON *intermediate =
                cJSON_GetObjectItemCaseSensitive(package, "intermediateCert");
            if (cJSON_IsObject(user) && cJSON_IsObject(intermediate)) {
                result->user_cert = cJSON_PrintUnformatted(user);
                result->intermediate_cert = cJSON_PrintUnformatted(intermediate);
            }
            if (result->user_cert == nullptr ||
                result->intermediate_cert == nullptr) {
                FMO_ACTIVATE_CORE_FreeResult(result);
                ok = false;
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}

extern "C" void FMO_ACTIVATE_CORE_FreeResult(FmoActivateResult *result)
{
    if (result == nullptr) return;
    free(result->user_cert);
    free(result->intermediate_cert);
    result->user_cert = nullptr;
    result->intermediate_cert = nullptr;
}
