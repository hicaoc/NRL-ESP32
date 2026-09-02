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
    bool mqtt_no_local;
    FmoServer server;
} FmoConfig;

typedef struct {
    bool configured;
    bool connected;
    bool receiving;
    bool transmitting;
    int last_error;
    char client_id[48];
    char role[8];           // role the current link logged in with ("user"/"super"/"admin"), "" when disconnected
    char voice_callsign[8];
    char voice_codec[8];
    uint32_t rx_frames;
    uint32_t parse_errors;
} FmoLinkStatus;

bool FMO_Init(void);
void FMO_GetConfig(FmoConfig *config);
bool FMO_SetConfig(const FmoConfig *config, bool persist);
void FMO_GetLinkStatus(FmoLinkStatus *status);
// True only while the link is connected AND the role it logged in with is
// "super" AND the configured server callsign equals this device's certificate
// callsign (i.e. we are operating our own server). "admin" deliberately does
// NOT count as "super" here (open question whether the reference firmware
// treats it equivalently for the STATION broadcast).
bool FMO_IsSuperOnOwnServer(void);
bool FMO_IsTransmitSelected(void);
// Dedicated touch PTT used by the S31 split home control. It keys FMO without
// changing the persisted target of the physical PTT button.
void FMO_SetPtt(bool held);
bool FMO_PttActive(void);

size_t FMO_ServerCount(void);
bool FMO_GetServer(size_t index, FmoServer *server);
bool FMO_SelectServer(size_t index, bool persist);

// Publish on the FMO MQTT link (used by the QSO signaling to post the
// established-QSO record to FMO/QSO/UID/<peer uid>). Returns false when the
// link is down or the publish could not be queued.
bool FMO_PublishMessage(const char *topic, const char *data, int len);

// 成员网格花名册（FMO/QSO/UID/# 成员 JSON，含 NRL 桥 [json] 跨服务器转发）。
// 按呼号（忽略大小写/-SSID）查网格，供说话人位置显示；未收录返回 false。
bool FMO_LookupMemberGrid(const char *callsign, char out_grid[7]);

#ifdef __cplusplus
}
#endif

#endif
