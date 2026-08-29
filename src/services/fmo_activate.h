#ifndef SRC_SERVICES_FMO_ACTIVATE_H
#define SRC_SERVICES_FMO_ACTIVATE_H

// FMO device activation: signs an activateReq array with the device Ed25519
// key and POSTs it to the certificate platform's public
// /api/device/activate endpoint, which issues user/intermediate certificates
// automatically for MACs that are registered and bound to a user account.
// Triggered manually from the Web portal ("/fmo/activate").

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMO_ACTIVATE_HOST_MAX 128

// Certificate server host, persisted in NVS namespace "fmoact". The default
// is the public platform ("www.hamptt.com").
void FMO_ACTIVATE_GetHost(char *out, size_t out_size);
// Trims whitespace; rejects empty or overlong values. Bumps the config
// generation so the Web status page reflects the change.
bool FMO_ACTIVATE_SetHost(const char *host);

// Summary of the most recent activation attempt (RAM only, for the Web
// status page). `last` is a short human-readable result line; `last_epoch`
// is its time (0 = never attempted).
void FMO_ACTIVATE_GetStatus(char *last, size_t last_size, uint64_t *last_epoch);

// Runs one synchronous activation round: ensures a device key (generating and
// storing one via FMO_CERT_Put when absent), signs the activateReq CBOR,
// POSTs to https://<host>/api/device/activate, and on "ok" stores the issued
// user/intermediate certificates and bounces the FMO link so it reconnects
// with the new identity. `message` always receives a short result line
// ("OK ..." summary on success, a Chinese explanation on failure).
// Takes a few seconds (one HTTPS round trip); call from a task with a
// generous stack (the Web portal httpd worker uses 8 KB).
esp_err_t FMO_ACTIVATE_Run(char *message, size_t message_size);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_ACTIVATE_H
