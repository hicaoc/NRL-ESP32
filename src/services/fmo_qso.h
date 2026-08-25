#ifndef SRC_SERVICES_FMO_QSO_H
#define SRC_SERVICES_FMO_QSO_H

// FMO QSO 呼叫信令（APRS APFMO0 消息）固件层：
//   - 状态机/报文在 fmo_qso_core（host 可测）；这里负责身份、APRS 上行、
//     跳台、MQTT 记录发布、NVS 通联记录、振铃提示音和 1s tick。
//   - 主叫：FMO_QSO_StartCall（Web /fmo 页、AT+FMO_CALL）。
//   - 被叫：display 来电弹屏 + PTT 短按接听（FMO_QSO_Answer(true)）/
//     长按拒绝（FMO_QSO_Answer(false)），60s 未接自动结束（不发 TIMEOUT）。
//   - established 后向 FMO/QSO/UID/<对方uid> 发记录 JSON；fmo_service 订阅
//     FMO/QSO/UID/<本机uid>，收到对方记录经 FMO_QSO_OnMqttRecord 落 NVS。

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t phase;             // FmoQsoPhase (fmo_qso_core.h)
    char phase_name[12];       // "idle"/"querying"/"calling"/"incoming"/"established"
    char peer[16];
    uint32_t peer_uid;
    bool outgoing;
    char detail[128];          // 人读状态行（UTF-8）
} FmoQsoSnapshot;

// NVS 环形通联记录条目。kind 0 = 本地信令事件（result 有效），
// kind 1 = 对方发来的完整通联记录（comment/grid/relay 有效）。
typedef struct {
    int64_t ts;
    uint8_t kind;
    uint8_t out;               // kind 0: 1 = 呼出
    char peer[16];
    uint32_t peer_uid;
    char result[24];
    char comment[193];         // 对方 toComment（bcn qso_msg 的祝福）
    char grid[8];              // 对方 fromGrid
    char relay[97];            // 对方 relayName
} FmoQsoLogEntry;

bool FMO_QSO_Init(void);

void FMO_QSO_GetSnapshot(FmoQsoSnapshot *out);

// 发起呼叫（peer 呼号可带 SSID；peer_uid 传 0 = 未知，从 QTHANS 学习）。
// 失败返回 false 并写 err（含 APRS 未验证/证书未就绪/忙等前置检查）。
bool FMO_QSO_StartCall(const char *peer, uint32_t peer_uid, char *err,
                       size_t err_cap);
// 接听/拒绝当前来电。返回 false = 当前没有来电。
bool FMO_QSO_Answer(bool accept);
// 取消出站呼叫 / 结束已建立的 QSO。
void FMO_QSO_Cancel(void);
// 被叫振铃中（status_io 用它劫持 PTT 短按=接听/长按=拒绝）。
bool FMO_QSO_IncomingRing(void);

// fmo_service 挂点：APRS-IS 发现连接收到的 APFMO0 消息行（整行 TNC2）。
void FMO_QSO_HandleAprsLine(const char *line);
// fmo_service 挂点：FMO/QSO/UID/<本机uid> 收到的记录 JSON 载荷。
void FMO_QSO_OnMqttRecord(const char *data, int len);

// 本地通联记录（NVS 环形，上限 16 条；index 0 = 最新）。
size_t FMO_QSO_LogCount(void);
bool FMO_QSO_LogGet(size_t index, FmoQsoLogEntry *out);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_QSO_H
