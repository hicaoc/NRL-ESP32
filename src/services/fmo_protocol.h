#ifndef SRC_SERVICES_FMO_PROTOCOL_H
#define SRC_SERVICES_FMO_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char callsign[16];
    uint32_t uid;
    uint32_t algorithm;
    uint8_t public_key[32];
    uint8_t fingerprint[32];
    uint64_t issued_at;
    uint64_t expires_at;
} FmoPublicCertificate;

bool FMO_PROTOCOL_ParseBeaconCertificate(const char *base64url,
                                         FmoPublicCertificate *certificate);

#ifdef __cplusplus
}
#endif

#endif
