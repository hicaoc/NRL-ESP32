// Host unit test for the FMO QSO call-signaling core
// (src/services/fmo_qso_core.cpp): wire layouts, msgId 关联/回显, 状态机转移
// （含超时表）与梅登黑德网格换算。参考实现 nrl-pulse src-tauri/src/fmo/qso.rs。
//
// Build & run (from the repo root, MSYS2/MinGW g++ works):
//   g++ -std=c++17 -Wall -Wextra -I src tests/fmo_qso_test.cpp
//       src/services/fmo_qso_core.cpp -o .tmp/fmo_qso_test.exe
//   ./.tmp/fmo_qso_test.exe

#include "services/fmo_qso_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {

void testBaseCall()
{
    char out[16];
    FMO_QSO_CORE_BaseCall("bd4xgt-15", out, sizeof(out));
    assert(strcmp(out, "BD4XGT") == 0);
    FMO_QSO_CORE_BaseCall("BG9JYT", out, sizeof(out));
    assert(strcmp(out, "BG9JYT") == 0);
    FMO_QSO_CORE_BaseCall("", out, sizeof(out));
    assert(out[0] == '\0');
    printf("ok: base callsign\n");
}

void testMaidenhead()
{
    char grid[7];
    FMO_QSO_CORE_Maidenhead(39.9, 116.4, grid);
    assert(strcmp(grid, "OM89ev") == 0);
    FMO_QSO_CORE_Maidenhead(32.3932, 119.3706, grid);
    assert(strcmp(grid, "OM92qj") == 0);
    // 南半球/西经（手算向量：-33.865,-74.006 → FF26xd）
    FMO_QSO_CORE_Maidenhead(-33.865, -74.006, grid);
    assert(strcmp(grid, "FF26xd") == 0);
    // 非法输入返回空串
    FMO_QSO_CORE_Maidenhead(0.0 / 0.0, 116.4, grid);
    assert(grid[0] == '\0');
    printf("ok: maidenhead grid\n");
}

void testBuildLine()
{
    char line[FMO_QSO_LINE_MAX];
    const size_t n = FMO_QSO_CORE_BuildLine(
        line, sizeof(line), "bd4xgt", "BG8LLD", "QTHQRY,Q3187,U2533", "2");
    assert(n > 0u);
    // addressee 空格补齐 9 字符，载荷后 '{' + msgId。
    assert(strcmp(line,
                  "BD4XGT>APFMO0,TCPIP*::BG8LLD   :QTHQRY,Q3187,U2533{2") == 0);
    assert(n == strlen(line));
    // 缓冲不足返回 0。
    char tiny[16];
    assert(FMO_QSO_CORE_BuildLine(tiny, sizeof(tiny), "A", "B", "C", "1") == 0u);
    printf("ok: message line build\n");
}

void testParseLine()
{
    char from[20], to[20], payload[256], msg_id[16];
    assert(FMO_QSO_CORE_ParseLine(
        "BG9JYT-15>APFMO0,qAR,BD4XGT::BD4XGT   :QTHANS,F1,U2725,S2579,"
        "LA20260806010157Z,河北某地{7",
        from, sizeof(from), to, sizeof(to), payload, sizeof(payload), msg_id,
        sizeof(msg_id)));
    assert(strcmp(from, "BG9JYT-15") == 0);
    assert(strcmp(to, "BD4XGT") == 0);
    assert(strcmp(payload,
                  "QTHANS,F1,U2725,S2579,LA20260806010157Z,河北某地") == 0);
    assert(strcmp(msg_id, "7") == 0);
    // 无 msgId 的消息也合法。
    assert(FMO_QSO_CORE_ParseLine("A>APFMO0,TCPIP*::B        :CALLANS,RING",
                                  from, sizeof(from), to, sizeof(to), payload,
                                  sizeof(payload), msg_id, sizeof(msg_id)));
    assert(strcmp(to, "B") == 0);
    assert(strcmp(payload, "CALLANS,RING") == 0);
    assert(msg_id[0] == '\0');
    // 非消息包（无 "::TO:" 结构）拒绝。
    assert(!FMO_QSO_CORE_ParseLine("A>APFMO4,TCPIP*:=3952.80NF11931.57EiFMO",
                                   from, sizeof(from), to, sizeof(to), payload,
                                   sizeof(payload), msg_id, sizeof(msg_id)));
    printf("ok: message line parse\n");
}

void testPayloadBuilders()
{
    char payload[256];
    assert(FMO_QSO_CORE_BuildQthqry(payload, sizeof(payload), 3187u, 2533u) > 0u);
    assert(strcmp(payload, "QTHQRY,Q3187,U2533") == 0);
    // uid 未知时省略 U（靠 addressee 呼号路由）。
    assert(FMO_QSO_CORE_BuildQthqry(payload, sizeof(payload), 3187u, 0u) > 0u);
    assert(strcmp(payload, "QTHQRY,Q3187") == 0);
    assert(FMO_QSO_CORE_BuildQthans(payload, sizeof(payload), 2725u, 2579u,
                                    "20260806010157Z", "河北某地") > 0u);
    assert(strcmp(payload,
                  "QTHANS,F1,U2725,S2579,LA20260806010157Z,河北某地") == 0);
    assert(FMO_QSO_CORE_BuildCall(payload, sizeof(payload), 796u, 2533u, 2579u,
                                  "测试台") > 0u);
    assert(strcmp(payload, "CALL,Q796,U2533,S2579,测试台") == 0);
    assert(FMO_QSO_CORE_BuildCallans(payload, sizeof(payload), "ACCEPT") > 0u);
    assert(strcmp(payload, "CALLANS,ACCEPT") == 0);
    assert(FMO_QSO_CORE_BuildCallcancel(payload, sizeof(payload), 3187u, 2533u) > 0u);
    assert(strcmp(payload, "CALLCANCEL,Q3187,U2533") == 0);
    printf("ok: payload builders\n");
}

void testParseFields()
{
    char fields[256];
    bool has_q, has_u, has_s;
    uint32_t q, u, s;
    char name[97];

    snprintf(fields, sizeof(fields), "Q3187,U2533");
    FMO_QSO_CORE_ParseFields(fields, &has_q, &q, &has_u, &u, &has_s, &s, name,
                             sizeof(name));
    assert(has_q && q == 3187u);
    assert(has_u && u == 2533u);
    assert(!has_s);
    assert(name[0] == '\0');

    // 实捕：QTHANS,F1,U2725,S2579,LA20260806010157Z,<服务器名>
    snprintf(fields, sizeof(fields), "F1,U2725,S2579,LA20260806010157Z,河北某地");
    FMO_QSO_CORE_ParseFields(fields, &has_q, &q, &has_u, &u, &has_s, &s, name,
                             sizeof(name));
    assert(!has_q);
    assert(has_u && u == 2725u);
    assert(has_s && s == 2579u);
    assert(strcmp(name, "河北某地") == 0);

    snprintf(fields, sizeof(fields), "Q796,U2533,S2579,测试台");
    FMO_QSO_CORE_ParseFields(fields, &has_q, &q, &has_u, &u, &has_s, &s, name,
                             sizeof(name));
    assert(has_q && q == 796u && has_u && u == 2533u && has_s && s == 2579u);
    assert(strcmp(name, "测试台") == 0);
    printf("ok: field parsing\n");
}

void testRecordJson()
{
    char json[1024];
    const size_t n = FMO_QSO_CORE_BuildRecordJson(
        json, sizeof(json), 7u, 1786318787ULL, 438500000ULL, "BD4XGT",
        "OM89ev", "BG8LLD", "", "73 通联愉快", "FMO", "测试台", "BG9JYT");
    assert(n > 0u);
    assert(n == strlen(json));
    assert(strstr(json, "\"logId\":7,") != nullptr);
    assert(strstr(json, "\"timestamp\":1786318787,") != nullptr);
    assert(strstr(json, "\"freqHz\":438500000,") != nullptr);
    assert(strstr(json, "\"fromCallsign\":\"BD4XGT\"") != nullptr);
    assert(strstr(json, "\"fromGrid\":\"OM89ev\"") != nullptr);
    assert(strstr(json, "\"toCallsign\":\"BG8LLD\"") != nullptr);
    assert(strstr(json, "\"toGrid\":\"\"") != nullptr);
    assert(strstr(json, "\"toComment\":\"73 通联愉快\"") != nullptr);
    assert(strstr(json, "\"mode\":\"FMO\"") != nullptr);
    assert(strstr(json, "\"relayName\":\"测试台\"") != nullptr);
    assert(strstr(json, "\"relayAdmin\":\"BG9JYT\"}") != nullptr);
    // 空祝福不破坏记录；引号/反斜杠转义。
    assert(FMO_QSO_CORE_BuildRecordJson(json, sizeof(json), 1u, 0u, 0u, "A", "",
                                        "B", "", "", "FMO", "", "") > 0u);
    assert(strstr(json, "\"toComment\":\"\"") != nullptr);
    assert(FMO_QSO_CORE_BuildRecordJson(json, sizeof(json), 1u, 0u, 0u, "A", "",
                                        "B", "", "say \"hi\" \\ok", "FMO", "",
                                        "") > 0u);
    assert(strstr(json, "\"toComment\":\"say \\\"hi\\\" \\\\ok\"") != nullptr);
    printf("ok: qso record json\n");
}

// ------------------------------------------------------------ 状态机辅助 ---

FmoQsoState makeState(const char *call = "BD4XGT", uint32_t uid = 3187u)
{
    FmoQsoState st;
    FMO_QSO_CORE_Init(&st);
    FMO_QSO_CORE_SetIdentity(&st, call, uid);
    return st;
}

FmoQsoContext makeCtx(uint32_t srv_uid = 2579u, const char *name = "测试台")
{
    FmoQsoContext ctx = {};
    ctx.srv_uid = srv_uid;
    snprintf(ctx.srv_name, sizeof(ctx.srv_name), "%s", name);
    snprintf(ctx.la, sizeof(ctx.la), "20260821010101Z");
    return ctx;
}

size_t onMsg(FmoQsoState *st, const FmoQsoContext *ctx, const char *from,
             const char *payload, const char *msg_id, int64_t now,
             FmoQsoAction *acts, size_t max)
{
    // 模拟固件层：载荷切动词 + fields。
    char scratch[256];
    snprintf(scratch, sizeof(scratch), "%s", payload);
    char *comma = strchr(scratch, ',');
    char *fields = scratch + strlen(scratch);
    if (comma != nullptr) {
        *comma = '\0';
        fields = comma + 1;
    }
    return FMO_QSO_CORE_OnMessage(st, ctx, from, scratch, fields, msg_id, now,
                                  acts, max);
}

void testOutgoingFlow()
{
    FmoQsoState st = makeState();
    FmoQsoContext ctx = makeCtx();
    FmoQsoAction acts[8];
    size_t n = 0;
    char err[64];

    // 发起呼叫（uid 已知）：SEND QTHQRY，msgId "1"。
    assert(FMO_QSO_CORE_StartCall(&st, "bg8lld", 2533u, 1000, acts, 8, &n, err,
                                  sizeof(err)));
    assert(n == 1u && acts[0].type == FMO_QSO_ACT_SEND);
    assert(strcmp(acts[0].to, "BG8LLD") == 0);
    assert(strcmp(acts[0].payload, "QTHQRY,Q3187,U2533") == 0);
    assert(strcmp(acts[0].msg_id, "1") == 0);
    assert(st.phase == FMO_QSO_PHASE_OUT_QUERY && st.deadline == 1010);

    // 3s 无应答 → 重发，msgId 递增（实捕 {2 {3 {4 一致）。
    n = FMO_QSO_CORE_Tick(&st, 1003, acts, 8);
    assert(n == 1u && acts[0].type == FMO_QSO_ACT_SEND);
    assert(strcmp(acts[0].payload, "QTHQRY,Q3187,U2533") == 0);
    assert(strcmp(acts[0].msg_id, "2") == 0);

    // QTHANS（回显我们的 msgId 无关紧要来去；这里验证 JUMP+CALL 与 uid 学习）。
    n = onMsg(&st, &ctx, "BG8LLD-7", "QTHANS,F1,U2533,S2579,LA20260821010105Z,测试台",
              "2", 1004, acts, 8);
    assert(n == 2u);
    assert(acts[0].type == FMO_QSO_ACT_JUMP && acts[0].uid == 2579u);
    assert(acts[1].type == FMO_QSO_ACT_SEND);
    assert(strcmp(acts[1].payload, "CALL,Q3187,U2533,S2579,测试台") == 0);
    assert(st.phase == FMO_QSO_PHASE_OUT_CALL && !st.wait_accept);
    assert(st.deadline == 1004 + FMO_QSO_RING_TIMEOUT_S);
    assert(st.peer_uid == 2533u);

    // RING → 等 ACCEPT，deadline 60s。
    n = onMsg(&st, &ctx, "BG8LLD", "CALLANS,RING", "3", 1005, acts, 8);
    assert(n == 0u && st.wait_accept);
    assert(st.deadline == 1005 + FMO_QSO_ACCEPT_TIMEOUT_S);

    // ACCEPT → LOG(接通) + ESTABLISHED。
    n = onMsg(&st, &ctx, "BG8LLD", "CALLANS,ACCEPT", "3", 1006, acts, 8);
    assert(n == 2u);
    assert(acts[0].type == FMO_QSO_ACT_LOG && strcmp(acts[0].text, "接通") == 0);
    assert(acts[0].out);
    assert(acts[1].type == FMO_QSO_ACT_ESTABLISHED);
    assert(strcmp(acts[1].to, "BG8LLD") == 0 && acts[1].uid == 2533u);
    assert(st.phase == FMO_QSO_PHASE_ESTABLISHED && st.outgoing);
    printf("ok: outgoing call flow\n");
}

void testOutgoingUnknownUid()
{
    FmoQsoState st = makeState();
    FmoQsoContext ctx = makeCtx();
    FmoQsoAction acts[8];
    size_t n = 0;
    char err[64];
    // uid 未知：QTHQRY 省略 U，从 QTHANS 的 U 学习。
    assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 0u, 1000, acts, 8, &n, err,
                                  sizeof(err)));
    assert(n == 1u && strcmp(acts[0].payload, "QTHQRY,Q3187") == 0);
    n = onMsg(&st, &ctx, "BG8LLD", "QTHANS,F1,U2533,S2579,LA20260821010105Z,测试台",
              "1", 1001, acts, 8);
    assert(n == 2u && st.peer_uid == 2533u);
    assert(strcmp(acts[1].payload, "CALL,Q3187,U2533,S2579,测试台") == 0);
    printf("ok: outgoing call with unknown uid\n");
}

void testOutgoingRejectAndBusy()
{
    FmoQsoAction acts[8];
    char err[64];
    // REJECT。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        n = onMsg(&st, &ctx, "BG8LLD", "CALLANS,REJECT", "1", 1001, acts, 8);
        // 阶段不符（还在 OUT_QUERY）：CALLANS 被忽略。
        assert(n == 0u && st.phase == FMO_QSO_PHASE_OUT_QUERY);
        (void)onMsg(&st, &ctx, "BG8LLD",
                    "QTHANS,F1,U2533,S2579,LA20260821010105Z,测试台", "1", 1002,
                    acts, 8);
        n = onMsg(&st, &ctx, "BG8LLD", "CALLANS,REJECT", "1", 1003, acts, 8);
        assert(n == 1u && acts[0].type == FMO_QSO_ACT_LOG);
        assert(strcmp(acts[0].text, "对方拒绝") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    // 忙时收到 CALL → 回 CALLANS,BUSY（回显来信 msgId）。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        n = onMsg(&st, &ctx, "BG9JYT", "CALL,Q999,U3187,S2579,测试台", "55",
                  1001, acts, 8);
        assert(n == 1u && acts[0].type == FMO_QSO_ACT_SEND);
        assert(strcmp(acts[0].payload, "CALLANS,BUSY") == 0);
        assert(strcmp(acts[0].msg_id, "55") == 0);
        assert(st.phase == FMO_QSO_PHASE_OUT_QUERY); // 主叫流程不受影响
    }
    // CALLANS 来源呼号不符 → 忽略。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        (void)onMsg(&st, &ctx, "BG8LLD",
                    "QTHANS,F1,U2533,S2579,LA20260821010105Z,测试台", "1", 1001,
                    acts, 8);
        n = onMsg(&st, &ctx, "BG9JYT", "CALLANS,ACCEPT", "9", 1002, acts, 8);
        assert(n == 0u && st.phase == FMO_QSO_PHASE_OUT_CALL);
    }
    printf("ok: reject / busy / wrong-peer paths\n");
}

void testIncomingFlow()
{
    FmoQsoState st = makeState();
    FmoQsoContext ctx = makeCtx();
    FmoQsoAction acts[8];

    // 收到 CALL：立即回 CALLANS,RING（回显来信 msgId "42"）并振铃。
    size_t n = onMsg(&st, &ctx, "BG9JYT-14", "CALL,Q796,U3187,S2579,测试台",
                     "42", 2000, acts, 8);
    assert(n == 1u && acts[0].type == FMO_QSO_ACT_SEND);
    assert(strcmp(acts[0].to, "BG9JYT-14") == 0);
    assert(strcmp(acts[0].payload, "CALLANS,RING") == 0);
    assert(strcmp(acts[0].msg_id, "42") == 0);
    assert(st.phase == FMO_QSO_PHASE_IN_RING);
    assert(st.peer_uid == 796u);
    assert(st.deadline == 2000 + FMO_QSO_IN_RING_TIMEOUT_S);

    // 接听：CALLANS,ACCEPT（回显 "42"）+ LOG + ESTABLISHED。
    n = FMO_QSO_CORE_Answer(&st, true, 2002, acts, 8);
    assert(n == 3u);
    assert(acts[0].type == FMO_QSO_ACT_SEND);
    assert(strcmp(acts[0].payload, "CALLANS,ACCEPT") == 0);
    assert(strcmp(acts[0].msg_id, "42") == 0);
    assert(acts[1].type == FMO_QSO_ACT_LOG && strcmp(acts[1].text, "已接听") == 0);
    assert(acts[2].type == FMO_QSO_ACT_ESTABLISHED && acts[2].uid == 796u);
    assert(st.phase == FMO_QSO_PHASE_ESTABLISHED && !st.outgoing);

    // 对方结束（CALLCANCEL）。
    n = onMsg(&st, &ctx, "BG9JYT", "CALLCANCEL,Q796,U3187", "43", 2010, acts,
              8);
    assert(n == 1u && acts[0].type == FMO_QSO_ACT_LOG);
    assert(strcmp(acts[0].text, "对方结束") == 0);
    assert(st.phase == FMO_QSO_PHASE_IDLE);

    // 再次来电 → 拒绝。
    n = onMsg(&st, &ctx, "BG9JYT", "CALL,Q796,U3187,S2579,测试台", "50", 2020,
              acts, 8);
    assert(n == 1u && st.phase == FMO_QSO_PHASE_IN_RING);
    n = FMO_QSO_CORE_Answer(&st, false, 2021, acts, 8);
    assert(n == 2u);
    assert(strcmp(acts[0].payload, "CALLANS,REJECT") == 0);
    assert(strcmp(acts[0].msg_id, "50") == 0);
    assert(acts[1].type == FMO_QSO_ACT_LOG && strcmp(acts[1].text, "已拒绝") == 0);
    assert(st.phase == FMO_QSO_PHASE_IDLE);
    printf("ok: incoming call flow\n");
}

void testQthqryAnswer()
{
    FmoQsoState st = makeState();
    FmoQsoContext ctx = makeCtx();
    FmoQsoAction acts[8];

    // 查的是我（U=本机 uid）→ 自动回 QTHANS，回显 msgId。
    size_t n = onMsg(&st, &ctx, "BG8LLD", "QTHQRY,Q2533,U3187", "17", 3000,
                     acts, 8);
    assert(n == 1u && acts[0].type == FMO_QSO_ACT_SEND);
    assert(strcmp(acts[0].to, "BG8LLD") == 0);
    assert(strcmp(acts[0].payload,
                  "QTHANS,F1,U3187,S2579,LA20260821010101Z,测试台") == 0);
    assert(strcmp(acts[0].msg_id, "17") == 0);
    // 无 U 字段（对方不知道我们的 uid）→ 同样应答（addressee 已匹配）。
    n = onMsg(&st, &ctx, "BG8LLD", "QTHQRY,Q2533", "18", 3001, acts, 8);
    assert(n == 1u);
    // 查的不是我（U 不符）→ 不答。
    n = onMsg(&st, &ctx, "BG8LLD", "QTHQRY,Q2533,U9999", "19", 3002, acts, 8);
    assert(n == 0u);
    // 未选定服务器 → 不答。
    FmoQsoContext no_srv = makeCtx(0u, "");
    n = onMsg(&st, &no_srv, "BG8LLD", "QTHQRY,Q2533,U3187", "20", 3003, acts,
              8);
    assert(n == 0u);
    printf("ok: qthqry auto-answer\n");
}

void testTimeouts()
{
    FmoQsoAction acts[8];
    char err[64];
    // QTHANS 10s 超时。
    {
        FmoQsoState st = makeState();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        n = FMO_QSO_CORE_Tick(&st, 1010, acts, 8);
        assert(n == 1u && acts[0].type == FMO_QSO_ACT_LOG);
        assert(strcmp(acts[0].text, "查询无应答") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    // RING 7s 超时（对方无应答）。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        (void)onMsg(&st, &ctx, "BG8LLD",
                    "QTHANS,F1,U2533,S2579,LA20260821010105Z,测试台", "1", 1001,
                    acts, 8);
        n = FMO_QSO_CORE_Tick(&st, 1001 + FMO_QSO_RING_TIMEOUT_S, acts, 8);
        assert(n == 1u && strcmp(acts[0].text, "对方无应答") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    // ACCEPT 60s 超时（对方未接听）。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        (void)onMsg(&st, &ctx, "BG8LLD",
                    "QTHANS,F1,U2533,S2579,LA20260821010105Z,测试台", "1", 1001,
                    acts, 8);
        (void)onMsg(&st, &ctx, "BG8LLD", "CALLANS,RING", "1", 1002, acts, 8);
        n = FMO_QSO_CORE_Tick(&st, 1002 + FMO_QSO_ACCEPT_TIMEOUT_S, acts, 8);
        assert(n == 1u && strcmp(acts[0].text, "对方未接听（超时）") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    // 被叫振铃 60s 超时（不发 TIMEOUT，本地收尾）。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        (void)onMsg(&st, &ctx, "BG9JYT", "CALL,Q796,U3187,S2579,测试台", "42",
                    2000, acts, 8);
        const size_t n =
            FMO_QSO_CORE_Tick(&st, 2000 + FMO_QSO_IN_RING_TIMEOUT_S, acts, 8);
        assert(n == 1u && acts[0].type == FMO_QSO_ACT_LOG);
        assert(strcmp(acts[0].text, "未接来电") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    printf("ok: timeout table\n");
}

void testCancelAndJumpFail()
{
    FmoQsoAction acts[8];
    char err[64];
    // 取消出站呼叫 → CALLCANCEL + LOG。
    {
        FmoQsoState st = makeState();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        n = FMO_QSO_CORE_Cancel(&st, 1001, acts, 8);
        assert(n == 2u);
        assert(acts[0].type == FMO_QSO_ACT_SEND);
        assert(strcmp(acts[0].payload, "CALLCANCEL,Q3187,U2533") == 0);
        assert(acts[1].type == FMO_QSO_ACT_LOG &&
               strcmp(acts[1].text, "已取消") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    // 振铃中对方取消。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        (void)onMsg(&st, &ctx, "BG9JYT", "CALL,Q796,U3187,S2579,测试台", "42",
                    2000, acts, 8);
        const size_t n =
            onMsg(&st, &ctx, "BG9JYT", "CALLCANCEL,Q796,U3187", "43", 2001,
                  acts, 8);
        assert(n == 1u && strcmp(acts[0].text, "对方取消") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    // 跳台失败（uid 查不到服务器）→ LOG + idle。
    {
        FmoQsoState st = makeState();
        FmoQsoContext ctx = makeCtx();
        size_t n = 0;
        assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n,
                                      err, sizeof(err)));
        (void)onMsg(&st, &ctx, "BG8LLD",
                    "QTHANS,F1,U2533,S2579,LA20260821010105Z,测试台", "1", 1001,
                    acts, 8);
        n = FMO_QSO_CORE_JumpFailed(&st, 1002, acts, 8);
        assert(n == 1u && acts[0].type == FMO_QSO_ACT_LOG);
        assert(strcmp(acts[0].text, "跳台失败") == 0);
        assert(st.phase == FMO_QSO_PHASE_IDLE);
    }
    printf("ok: cancel / jump-fail paths\n");
}

void testStartCallErrors()
{
    FmoQsoAction acts[8];
    size_t n = 0;
    char err[64];
    FmoQsoState st = makeState();
    assert(!FMO_QSO_CORE_StartCall(&st, "  ", 0u, 1000, acts, 8, &n, err,
                                   sizeof(err)));
    assert(strcmp(err, "请输入对方呼号") == 0);
    assert(!FMO_QSO_CORE_StartCall(&st, "bd4xgt-9", 0u, 1000, acts, 8, &n, err,
                                   sizeof(err)));
    assert(strcmp(err, "不能呼叫自己") == 0);
    assert(FMO_QSO_CORE_StartCall(&st, "BG8LLD", 2533u, 1000, acts, 8, &n, err,
                                  sizeof(err)));
    assert(!FMO_QSO_CORE_StartCall(&st, "BG9JYT", 0u, 1000, acts, 8, &n, err,
                                   sizeof(err)));
    assert(strcmp(err, "当前有进行中的 QSO，请先取消/结束") == 0);
    printf("ok: start-call validation\n");
}

} // namespace

int main()
{
    testBaseCall();
    testMaidenhead();
    testBuildLine();
    testParseLine();
    testPayloadBuilders();
    testParseFields();
    testRecordJson();
    testOutgoingFlow();
    testOutgoingUnknownUid();
    testOutgoingRejectAndBusy();
    testIncomingFlow();
    testQthqryAnswer();
    testTimeouts();
    testCancelAndJumpFail();
    testStartCallErrors();
    printf("all fmo_qso tests passed\n");
    return 0;
}
