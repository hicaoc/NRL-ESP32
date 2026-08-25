#ifndef SRC_SERVICES_FMO_QSO_CORE_H
#define SRC_SERVICES_FMO_QSO_CORE_H

// Pure, host-compilable FMO QSO call-signaling engine (APRS APFMO0 messages),
// ported from the nrl-pulse Rust reference (src-tauri/src/fmo/qso.rs) and the
// wire facts in fmo-sim docs/firmware-analysis.md §8.2:
//   - APRS message, AX.25 destination APFMO0, path TCPIP*:
//       `:目标呼号<空格补齐9>:<载荷>{msgId`  （应答回显来信 msgId）
//   - QTHQRY,Q<主叫uid>,U<被叫uid> → 被叫自动回
//       QTHANS,F1,U<uid>,S<服务器uid>,LA<ts>,<服务器名UTF-8>
//   - CALL,Q<uid>,U<uid>,S<s>,<str> → 被叫回 CALLANS,RING + 弹屏，人工
//     ACCEPT/REJECT；CALLCANCEL,Q<uid>,U<uid>
//   - 超时：QTHANS 10s（每 3s 重发，msgId 递增）/ RING 7s / ACCEPT 60s /
//     被叫振铃 60s（不发 TIMEOUT）
//   - 接通语义：主叫跳到被叫服务器（JUMP 动作），被叫不跳台
// No ESP-IDF dependency: the state machine queues FmoQsoAction records that
// the firmware layer (fmo_qso.cpp) drains and executes (APRS uplink, server
// jump, MQTT record publish, local log). The unit test in
// tests/fmo_qso_test.cpp exercises the exact wire layouts and transitions.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMO_QSO_CALLSIGN_MAX 16u
#define FMO_QSO_SRV_NAME_MAX 96u   // UTF-8 bytes
#define FMO_QSO_MSG_ID_MAX 12u
#define FMO_QSO_DETAIL_MAX 128u
#define FMO_QSO_PAYLOAD_MAX 240u
#define FMO_QSO_LINE_MAX 512u

#define FMO_QSO_QUERY_TIMEOUT_S 10
#define FMO_QSO_QUERY_RETRY_S 3
#define FMO_QSO_RING_TIMEOUT_S 7
#define FMO_QSO_ACCEPT_TIMEOUT_S 60
#define FMO_QSO_IN_RING_TIMEOUT_S 60

typedef enum {
    FMO_QSO_PHASE_IDLE = 0,
    FMO_QSO_PHASE_OUT_QUERY,    // 已发 QTHQRY，等 QTHANS
    FMO_QSO_PHASE_OUT_CALL,     // 已发 CALL，等 RING / ACCEPT
    FMO_QSO_PHASE_IN_RING,      // 收到 CALL，振铃中
    FMO_QSO_PHASE_ESTABLISHED,  // 已接通
} FmoQsoPhase;

typedef enum {
    FMO_QSO_ACT_SEND = 0,       // 发 APRS APFMO0 消息: to/payload/msg_id
    FMO_QSO_ACT_JUMP,           // 主叫跳台: uid = 目标服务器 uid
    FMO_QSO_ACT_ESTABLISHED,    // QSO 建立: to = 对方呼号, uid = 对方 uid
    FMO_QSO_ACT_LOG,            // 本地通联日志: to/uid/out + detail 已含结果
} FmoQsoActionType;

typedef struct {
    uint8_t type;                        // FmoQsoActionType
    char to[FMO_QSO_CALLSIGN_MAX];       // SEND: 目标呼号; 其余: 对方呼号
    char payload[FMO_QSO_PAYLOAD_MAX];   // SEND: 消息体（动词开头, UTF-8）
    char msg_id[FMO_QSO_MSG_ID_MAX];     // SEND: msgId（回显来信或本机自增）
    uint32_t uid;                        // JUMP: 服务器 uid; 其余: 对方 uid
    bool out;                            // LOG: 方向（true = 呼出）
    char text[32];                       // LOG: 结果（"接通"/"已拒绝"/...）
} FmoQsoAction;

typedef struct {
    FmoQsoPhase phase;
    bool wait_accept;   // OUT_CALL 子阶段: false = 等 RING, true = 等 ACCEPT
    bool outgoing;      // ESTABLISHED 方向
    char peer[FMO_QSO_CALLSIGN_MAX];
    uint32_t peer_uid;
    uint32_t srv_uid;                      // 对方所在服务器（OUT_CALL/IN_RING）
    int64_t deadline;                      // 当前阶段超时点（epoch 秒）
    int64_t last_sent;                     // OUT_QUERY 上次发送时间
    uint32_t seq;                          // 本机出站 msgId 序列
    char reply_msg_id[FMO_QSO_MSG_ID_MAX]; // 来信 msgId（应答回显用）
    char detail[FMO_QSO_DETAIL_MAX];       // 人读状态行（UTF-8）
    char my_call[FMO_QSO_CALLSIGN_MAX];    // 本机身份（SetIdentity 写入）
    uint32_t my_uid;
} FmoQsoState;

// 应答 QTHQRY 所需的本机上下文（固件层每次现取）
typedef struct {
    uint32_t srv_uid;                          // 当前服务器 uid；0 = 未选定（不应答）
    char srv_name[FMO_QSO_SRV_NAME_MAX + 1u];  // UTF-8 服务器名
    char la[24];                               // UTC "YYYYMMDDHHMMSSZ"
} FmoQsoContext;

void FMO_QSO_CORE_Init(FmoQsoState *st);
void FMO_QSO_CORE_SetIdentity(FmoQsoState *st, const char *callsign,
                              uint32_t uid);

// ---------------------------------------------------------------------------
// 工具

// 去掉 SSID 的大写基呼号（"bd4xgt-15" → "BD4XGT"）
void FMO_QSO_CORE_BaseCall(const char *in, char *out, size_t cap);

// 经纬度 → 6 位梅登黑德网格（39.9,116.4 → "OM89ev"）；非法输入输出空串。
void FMO_QSO_CORE_Maidenhead(double lat, double lon, char out[7]);

// ---------------------------------------------------------------------------
// 报文构造/解析

// 完整 TNC2 行: "<from>>APFMO0,TCPIP*::<to 补齐9>:<payload>{<msg_id>"
// 返回写入长度（不含 NUL），缓冲不足返回 0。
size_t FMO_QSO_CORE_BuildLine(char *out, size_t cap, const char *from_call,
                              const char *to_call, const char *payload,
                              const char *msg_id);

// 解析 APFMO0 消息行（from>dest,path::TO<9>:payload{msgId）。
// to 去掉尾部空格；msg_id 允许为空（无 '{' 时输出空串）。
bool FMO_QSO_CORE_ParseLine(const char *line, char *from, size_t from_cap,
                            char *to, size_t to_cap, char *payload,
                            size_t payload_cap, char *msg_id,
                            size_t msg_id_cap);

// 载荷构造（均为 NUL 结尾文本；服务器名按 UTF-8 原样拼接）。
// BuildQthqry: peer_uid == 0 时省略 U 字段（靠 addressee 呼号路由）。
size_t FMO_QSO_CORE_BuildQthqry(char *out, size_t cap, uint32_t my_uid,
                                uint32_t peer_uid);
size_t FMO_QSO_CORE_BuildQthans(char *out, size_t cap, uint32_t my_uid,
                                uint32_t srv_uid, const char *la,
                                const char *srv_name_utf8);
size_t FMO_QSO_CORE_BuildCall(char *out, size_t cap, uint32_t my_uid,
                              uint32_t peer_uid, uint32_t srv_uid,
                              const char *srv_name_utf8);
size_t FMO_QSO_CORE_BuildCallans(char *out, size_t cap, const char *answer);
size_t FMO_QSO_CORE_BuildCallcancel(char *out, size_t cap, uint32_t my_uid,
                                    uint32_t peer_uid);

// 逗号分隔字段提取（原地切割 fields 缓冲）: Q/U/S 数字标记 + 第一个剩余
// 字段当名称；LA*/F* 跳过。has_* 输出对应字段是否存在。
void FMO_QSO_CORE_ParseFields(char *fields, bool *has_q, uint32_t *q,
                              bool *has_u, uint32_t *u, bool *has_s,
                              uint32_t *s, char *name, size_t name_cap);

// established 后发布到 FMO/QSO/UID/<对方uid> 的记录 JSON
// （字段顺序与固件模板一致）。返回写入长度，缓冲不足返回 0。
size_t FMO_QSO_CORE_BuildRecordJson(char *out, size_t cap, uint32_t log_id,
                                    uint64_t timestamp, uint64_t freq_hz,
                                    const char *from_call,
                                    const char *from_grid,
                                    const char *to_call, const char *to_grid,
                                    const char *to_comment, const char *mode,
                                    const char *relay_name,
                                    const char *relay_admin);

// ---------------------------------------------------------------------------
// 状态机（所有函数把平台动作追加到 acts，返回动作数；detail 同步更新）

// UI 发起呼叫。peer_uid 传 0 表示未知（QTHQRY 省略 U，从 QTHANS 学习）。
// 失败返回 false 并写 err（"请输入对方呼号"/"当前有进行中的 QSO，请先取消/结束"/
// "不能呼叫自己"）。
bool FMO_QSO_CORE_StartCall(FmoQsoState *st, const char *peer,
                            uint32_t peer_uid, int64_t now_s,
                            FmoQsoAction *acts, size_t max_acts,
                            size_t *act_count, char *err, size_t err_cap);

// 接听/拒绝来电（仅 IN_RING 有效，返回动作数 0 = 当前没有来电）。
size_t FMO_QSO_CORE_Answer(FmoQsoState *st, bool accept, int64_t now_s,
                           FmoQsoAction *acts, size_t max_acts);

// 取消出站呼叫 / 结束已建立的 QSO / 拒绝来电。
size_t FMO_QSO_CORE_Cancel(FmoQsoState *st, int64_t now_s, FmoQsoAction *acts,
                           size_t max_acts);

// 入站信令。from = 来源呼号（可带 SSID），verb = 动词，fields = 动词后
// 逗号分隔余串（会被原地切割），msg_id = 来信 msgId（应答回显）。
size_t FMO_QSO_CORE_OnMessage(FmoQsoState *st, const FmoQsoContext *ctx,
                              const char *from, const char *verb, char *fields,
                              const char *msg_id, int64_t now_s,
                              FmoQsoAction *acts, size_t max_acts);

// 1 秒 tick：超时与 QTHQRY 重发（msgId 递增，与原厂固件一致）。
size_t FMO_QSO_CORE_Tick(FmoQsoState *st, int64_t now_s, FmoQsoAction *acts,
                         size_t max_acts);

// 跳台失败（固件层按 JUMP 的 uid 查不到服务器时调用）：中止本次呼叫。
size_t FMO_QSO_CORE_JumpFailed(FmoQsoState *st, int64_t now_s,
                               FmoQsoAction *acts, size_t max_acts);

const char *FMO_QSO_CORE_PhaseName(FmoQsoPhase phase);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_FMO_QSO_CORE_H
