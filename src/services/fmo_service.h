#ifndef SRC_SERVICES_FMO_SERVICE_H
#define SRC_SERVICES_FMO_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMO_SERVER_NAME_MAX 96
#define FMO_SERVER_HOST_MAX 64
#define FMO_SERVER_CALLSIGN_MAX 16

typedef struct {
    char name[FMO_SERVER_NAME_MAX];
    char host[FMO_SERVER_HOST_MAX];
    char callsign[FMO_SERVER_CALLSIGN_MAX];
    uint16_t port;
    uint32_t uid;
    uint32_t online;
    uint32_t total;
    uint8_t fingerprint[32];
    bool has_fingerprint;
    int64_t last_seen;
} FmoServer;

typedef struct {
    bool enabled;
    bool transmit;
    FmoServer server;
} FmoConfig;

typedef struct {
    bool configured;
    bool connected;
    bool receiving;
    bool transmitting;
    int last_error;
    char voice_callsign[8];
    char voice_codec[8];
    uint32_t rx_frames;
    uint32_t parse_errors;
} FmoLinkStatus;

bool FMO_Init(void);
void FMO_GetConfig(FmoConfig *config);
bool FMO_SetConfig(const FmoConfig *config, bool persist);
void FMO_GetLinkStatus(FmoLinkStatus *status);
bool FMO_IsTransmitSelected(void);
// Dedicated touch PTT used by the S31 split home control. It keys FMO without
// changing the persisted target of the physical PTT button.
void FMO_SetPtt(bool held);
bool FMO_PttActive(void);

size_t FMO_ServerCount(void);
bool FMO_GetServer(size_t index, FmoServer *server);
bool FMO_SelectServer(size_t index, bool persist);

#ifdef __cplusplus
}
#endif

#endif
