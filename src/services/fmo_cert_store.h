#ifndef SRC_SERVICES_FMO_CERT_STORE_H
#define SRC_SERVICES_FMO_CERT_STORE_H

#include "services/fmo_service.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMO_CERT_USER = 0,
    FMO_CERT_INTERMEDIATE = 1,
    FMO_CERT_DEVICE_KEY = 2,
} FmoCertificateKind;

typedef struct {
    bool user_present;
    bool intermediate_present;
    bool device_key_present;
    bool ready;
    char callsign[16];
    uint32_t uid;
    uint64_t issued_at;
    uint64_t expires_at;
    uint8_t fingerprint[32];
} FmoIdentityStatus;

esp_err_t FMO_CERT_Put(FmoCertificateKind kind, const char *json,
                       size_t json_size, char *error, size_t error_size);
esp_err_t FMO_CERT_GetStatus(FmoIdentityStatus *status);
esp_err_t FMO_CERT_BuildCredentials(const FmoServer *server,
                                    const char *role, char *username,
                                    size_t username_size, char **password);
// Rebuild the deterministic 10-element user-certificate CBOR blob from the
// stored cert_user JSON (the "CERT:" payload of the FMO-V4 STATION broadcast;
// certBlobHash = SHA-256 over these exact bytes).
esp_err_t FMO_CERT_RebuildUserCertBlob(uint8_t *out, size_t capacity,
                                       size_t *out_size);
// Sign an arbitrary TBS with the device Ed25519 key derived from the stored
// deviceKey seed (the "SIG:" payload of the STATION broadcast).
esp_err_t FMO_CERT_SignWithDeviceKey(const uint8_t *tbs, size_t tbs_size,
                                     uint8_t signature[64]);

#ifdef __cplusplus
}
#endif

#endif
