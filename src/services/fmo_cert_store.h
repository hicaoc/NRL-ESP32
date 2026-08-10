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

#ifdef __cplusplus
}
#endif

#endif
