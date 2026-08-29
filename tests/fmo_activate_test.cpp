// Host unit test for the FMO device-activation request/response codec
// (src/services/fmo_activate_core.cpp).
//
// The CBOR golden vector is the server-side one from
// fmo-certificate-tools server/cert/golden_test.go (goldenActivateReqHex);
// byte-level equality is what guarantees the platform accepts the device
// signature over the activateReq array.
//
// Build & run (from the repo root, MSYS2/MinGW g++ works; cJSON is compiled
// straight from the vendored managed_components copy):
//   g++ -std=c++17 -Wall -Wextra -I src -I tests/shims
//       -I managed_components/espressif__cjson/cJSON
//       tests/fmo_activate_test.cpp
//       src/services/fmo_activate_core.cpp
//       managed_components/espressif__cjson/cJSON/cJSON.c
//       -o .tmp/fmo_activate_test.exe
//   ./.tmp/fmo_activate_test.exe

#include "services/fmo_activate_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

size_t hexToBytes(const char *hex, uint8_t *out, const size_t capacity)
{
    size_t count = 0u;
    while (hex[0] != '\0' && hex[1] != '\0' && count < capacity) {
        unsigned value = 0u;
        (void)sscanf(hex, "%2x", &value);
        out[count++] = static_cast<uint8_t>(value);
        hex += 2;
    }
    return count;
}

void bytesToHex(const uint8_t *data, const size_t size, char *out)
{
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2u] = kDigits[data[i] >> 4u];
        out[i * 2u + 1u] = kDigits[data[i] & 15u];
    }
    out[size * 2u] = '\0';
}

// The exact inputs of the server-side golden vector
// (fmo-certificate-tools server/cert/golden_test.go goldenActivateReqHex).
FmoActivateRequest goldenRequest()
{
    FmoActivateRequest request = {};
    assert(hexToBytes("D0CF13510C4C", request.mac, sizeof(request.mac)) == 6u);
    request.timestamp = 0x6A4ADDFFu;
    assert(hexToBytes("010203040506", request.nonce, sizeof(request.nonce)) == 6u);
    request.firmware_version = "4.0.0";
    memset(request.firmware_hash, 0x07, sizeof(request.firmware_hash));
    request.country_code = "CN";
    assert(hexToBytes(
               "ED4928C628D1C2C6EAE90338905995612959273A5C63F93636C14614AC8737D1",
               request.device_public_key,
               sizeof(request.device_public_key)) == 32u);
    for (size_t i = 0; i < sizeof(request.signature); ++i) {
        request.signature[i] = static_cast<uint8_t>(i);
    }
    return request;
}

void testGoldenCbor()
{
    FmoActivateRequest request = goldenRequest();
    uint8_t cbor[256];
    size_t cbor_size = 0u;
    assert(FMO_ACTIVATE_CORE_BuildRequestCbor(&request, cbor, sizeof(cbor),
                                              &cbor_size));
    uint8_t expected[256];
    const size_t expected_size = hexToBytes(
        "8A63464D4F046B616374697661746552657146D0CF13510C4C1A6A4ADDFF4601"
        "020304050665342E302E30582007070707070707070707070707070707070707"
        "0707070707070707070707070762434E5820ED4928C628D1C2C6EAE903389059"
        "95612959273A5C63F93636C14614AC8737D1",
        expected, sizeof(expected));
    assert(cbor_size == expected_size);
    if (memcmp(cbor, expected, cbor_size) != 0) {
        char got[513], want[513];
        bytesToHex(cbor, cbor_size, got);
        bytesToHex(expected, expected_size, want);
        printf("CBOR mismatch:\n got  %s\n want %s\n", got, want);
        assert(false);
    }
    // Truncated output buffer must fail cleanly.
    assert(!FMO_ACTIVATE_CORE_BuildRequestCbor(&request, cbor, cbor_size - 1u,
                                               &cbor_size));
    printf("ok: activateReq CBOR matches server golden vector\n");
}

void testRequestJson()
{
    FmoActivateRequest request = goldenRequest();
    char *json = FMO_ACTIVATE_CORE_BuildRequestJson(&request);
    assert(json != nullptr);
    // version/action/mac shape.
    assert(strstr(json, "\"version\":\"4\"") != nullptr);
    assert(strstr(json, "\"action\":\"activate\"") != nullptr);
    assert(strstr(json, "\"mac\":\"D0CF13510C4C\"") != nullptr);
    assert(strstr(json, "\"firmwareVersion\":\"4.0.0\"") != nullptr);
    assert(strstr(json, "\"countryCode\":\"CN\"") != nullptr);
    assert(strstr(json, "\"timestamp\":1783291391") != nullptr);
    // Byte fields are hex of the exact expected length.
    assert(strstr(json, "\"firmwareHash\":\"07070707070707070707070707070707"
                        "07070707070707070707070707070707\"") != nullptr);
    assert(strstr(json, "\"nonce\":\"010203040506\"") != nullptr);
    assert(strstr(json, "\"devicePublicKey\":\"ed4928c628d1c2c6eae903389059"
                        "95612959273a5c63f93636c14614ac8737d1\"") != nullptr);
    const char *sig = strstr(json, "\"signature\":\"");
    assert(sig != nullptr);
    const char *sig_end = strchr(sig + 13, '"');
    assert(sig_end != nullptr && sig_end - (sig + 13) == 128);
    free(json);
    printf("ok: request JSON fields\n");
}

void testParseOk()
{
    const char body[] =
        "{\"result\":\"ok\",\"code\":0,\"serverTime\":1785890310,"
        "\"authTag\":\"abc123\",\"certPackage\":{"
        "\"userCert\":{\"type\":\"userCert\",\"issuerSn\":5000,\"subject\":{"
        "\"callsign\":\"BG1ABC\",\"uid\":5001,\"publicKey\":\"AAAA\"}},"
        "\"intermediateCert\":{\"type\":\"intermediate\",\"sn\":5000}}}";
    FmoActivateResult result = {};
    assert(FMO_ACTIVATE_CORE_ParseResponse(body, strlen(body), &result));
    assert(strcmp(result.result, "ok") == 0);
    assert(result.code == 0);
    assert(result.server_time == 1785890310u);
    assert(result.user_cert != nullptr);
    assert(result.intermediate_cert != nullptr);
    assert(strstr(result.user_cert, "\"callsign\":\"BG1ABC\"") != nullptr);
    assert(strstr(result.intermediate_cert, "\"sn\":5000") != nullptr);
    FMO_ACTIVATE_CORE_FreeResult(&result);
    assert(result.user_cert == nullptr && result.intermediate_cert == nullptr);
    printf("ok: parse ok response\n");
}

void testParseErrorAndPending()
{
    const char error_body[] =
        "{\"result\":\"error\",\"code\":2,"
        "\"reason\":\"Device not registered\",\"serverTime\":1785890311}";
    FmoActivateResult result = {};
    assert(FMO_ACTIVATE_CORE_ParseResponse(error_body, strlen(error_body),
                                           &result));
    assert(strcmp(result.result, "error") == 0);
    assert(result.code == 2);
    assert(strcmp(result.reason, "Device not registered") == 0);
    assert(result.user_cert == nullptr && result.intermediate_cert == nullptr);
    FMO_ACTIVATE_CORE_FreeResult(&result);

    const char pending_body[] =
        "{\"result\":\"pending\",\"code\":100,"
        "\"reason\":\"manual review required\",\"serverTime\":1785890312}";
    assert(FMO_ACTIVATE_CORE_ParseResponse(pending_body, strlen(pending_body),
                                           &result));
    assert(strcmp(result.result, "pending") == 0);
    assert(result.code == 100);
    FMO_ACTIVATE_CORE_FreeResult(&result);
    printf("ok: parse error/pending responses\n");
}

void testParseInvalid()
{
    FmoActivateResult result = {};
    assert(!FMO_ACTIVATE_CORE_ParseResponse("not json", 8u, &result));
    // Missing code.
    assert(!FMO_ACTIVATE_CORE_ParseResponse("{\"result\":\"ok\"}", 16u,
                                            &result));
    // ok without a cert package is a structural failure, not a business one.
    const char no_package[] = "{\"result\":\"ok\",\"code\":0}";
    assert(!FMO_ACTIVATE_CORE_ParseResponse(no_package, strlen(no_package),
                                            &result));
    printf("ok: parse rejects malformed responses\n");
}

} // namespace

int main()
{
    testGoldenCbor();
    testRequestJson();
    testParseOk();
    testParseErrorAndPending();
    testParseInvalid();
    printf("all fmo_activate tests passed\n");
    return 0;
}
