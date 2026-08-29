#ifndef SRC_SERVICES_FMO_ACTIVATE_CORE_H
#define SRC_SERVICES_FMO_ACTIVATE_CORE_H

// Pure, host-compilable request/response codec for the FMO device activation
// endpoint (POST /api/device/activate on the certificate platform, see
// fmo-certificate-tools server/platform/device.go). No ESP-IDF or libsodium
// dependency: the caller signs the CBOR payload and hands the signature in,
// so the unit test in tests/ can exercise the exact wire layout against the
// server-side golden vector (server/cert/golden_test.go goldenActivateReqHex).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mac[6];              // ESP_MAC_WIFI_STA
    uint64_t timestamp;          // time(NULL)
    uint8_t nonce[6];            // random per request (replay protection)
    const char *firmware_version; // e.g. NRL_FIRMWARE_VERSION
    uint8_t firmware_hash[32];   // app ELF SHA-256, raw bytes
    const char *country_code;    // 2-letter code, e.g. "CN"
    uint8_t device_public_key[32]; // Ed25519 public key of the device key
    uint8_t signature[64];       // Ed25519 over the BuildRequestCbor bytes
} FmoActivateRequest;

// Deterministic CBOR of the 10-element activateReq signature array:
//   ["FMO",4,"activateReq",mac(6),timestamp,nonce(6),firmwareVersion,
//    firmwareHash(32),countryCode,devicePublicKey(32)]
// Byte-identical to server/cert BuildActivateReqPayload (golden vector:
// goldenActivateReqHex, verified by tests/fmo_activate_test.cpp).
bool FMO_ACTIVATE_CORE_BuildRequestCbor(const FmoActivateRequest *request,
                                        uint8_t *out, size_t capacity,
                                        size_t *out_size);

// JSON request body for POST /api/device/activate: version "4", action
// "activate", mac as 12 uppercase hex chars, and all byte fields
// (firmwareHash/devicePublicKey/nonce/signature) as lowercase hex -- the
// server's decodeFlexibleBytes tries hex first, which avoids any base64
// variant ambiguity. Returns a malloc'd string the caller frees with free()
// (NULL on allocation failure).
char *FMO_ACTIVATE_CORE_BuildRequestJson(const FmoActivateRequest *request);

typedef struct {
    char result[12];   // "ok" / "error" / "pending"
    long code;         // 0 on ok; 1=unbound 2=unregistered 3=timestamp
                       // 4=replay 8=rate limit 9=signature 100=pending ...
    char reason[96];   // server-provided reason text (may be empty)
    uint64_t server_time;
    // On "ok": certPackage.userCert / certPackage.intermediateCert re-printed
    // as standalone compact JSON strings, ready for FMO_CERT_Put. malloc'd,
    // released by FMO_ACTIVATE_CORE_FreeResult.
    char *user_cert;
    char *intermediate_cert;
} FmoActivateResult;

// Parses the response body. Returns false only when the body is not a
// structurally valid activate response (result/code missing); business
// failures are reported through result/code instead.
bool FMO_ACTIVATE_CORE_ParseResponse(const char *json, size_t json_size,
                                     FmoActivateResult *result);
void FMO_ACTIVATE_CORE_FreeResult(FmoActivateResult *result);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_ACTIVATE_CORE_H
