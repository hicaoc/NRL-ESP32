#include "services/fmo_qso_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

void copyText(char *out, const size_t cap, const char *in)
{
    if (cap == 0u) return;
    if (in == nullptr) in = "";
    size_t i = 0u;
    while (i + 1u < cap && in[i] != '\0') {
        out[i] = in[i];
        ++i;
    }
    out[i] = '\0';
}

bool parseUint(const char *text, uint32_t *value)
{
    if (text == nullptr || text[0] == '\0') return false;
    uint32_t v = 0u;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (v > (UINT32_MAX - digit) / 10u) return false;
        v = v * 10u + digit;
    }
    *value = v;
    return true;
}

void setDetail(FmoQsoState *st, const char *detail)
{
    copyText(st->detail, sizeof(st->detail), detail);
}

// 追加一个动作；缓冲满时静默丢弃（固件层给足容量）。
void push(FmoQsoAction *acts, const size_t max_acts, size_t *count,
          const FmoQsoAction &action)
{
    if (*count >= max_acts) return;
    acts[(*count)++] = action;
}

void pushSend(FmoQsoAction *acts, const size_t max_acts, size_t *count,
              const char *to, const char *payload, const char *msg_id)
{
    FmoQsoAction action = {};
    action.type = FMO_QSO_ACT_SEND;
    copyText(action.to, sizeof(action.to), to);
    copyText(action.payload, sizeof(action.payload), payload);
    copyText(action.msg_id, sizeof(action.msg_id), msg_id);
    push(acts, max_acts, count, action);
}

void pushLog(FmoQsoAction *acts, const size_t max_acts, size_t *count,
             const char *peer, const uint32_t peer_uid, const bool out,
             const char *result)
{
    FmoQsoAction action = {};
    action.type = FMO_QSO_ACT_LOG;
    copyText(action.to, sizeof(action.to), peer);
    action.uid = peer_uid;
    action.out = out;
    copyText(action.text, sizeof(action.text), result);
    push(acts, max_acts, count, action);
}

// 本机自增 msgId（原厂固件重发 QTHQRY 时同样递增：实捕 {2 {3 {4）
void nextMsgId(FmoQsoState *st, char out[FMO_QSO_MSG_ID_MAX])
{
    snprintf(out, FMO_QSO_MSG_ID_MAX, "%lu",
             static_cast<unsigned long>(st->seq++));
}

// 应答回显来信 msgId；来信没有 msgId 时回退到本机序列。
void echoMsgId(FmoQsoState *st, const char *incoming,
               char out[FMO_QSO_MSG_ID_MAX])
{
    if (incoming != nullptr && incoming[0] != '\0') {
        copyText(out, FMO_QSO_MSG_ID_MAX, incoming);
    } else {
        nextMsgId(st, out);
    }
}

bool baseEqual(const char *a, const char *b)
{
    char ba[FMO_QSO_CALLSIGN_MAX], bb[FMO_QSO_CALLSIGN_MAX];
    FMO_QSO_CORE_BaseCall(a, ba, sizeof(ba));
    FMO_QSO_CORE_BaseCall(b, bb, sizeof(bb));
    return ba[0] != '\0' && strcmp(ba, bb) == 0;
}

} // namespace

extern "C" void FMO_QSO_CORE_Init(FmoQsoState *st)
{
    if (st == nullptr) return;
    memset(st, 0, sizeof(*st));
    st->phase = FMO_QSO_PHASE_IDLE;
    st->seq = 1u;
}

extern "C" void FMO_QSO_CORE_SetIdentity(FmoQsoState *st,
                                         const char *callsign,
                                         const uint32_t uid)
{
    if (st == nullptr) return;
    copyText(st->my_call, sizeof(st->my_call), callsign);
    st->my_uid = uid;
}

extern "C" void FMO_QSO_CORE_BaseCall(const char *in, char *out,
                                      const size_t cap)
{
    if (cap == 0u) return;
    size_t i = 0u;
    if (in != nullptr) {
        while (i + 1u < cap && in[i] != '\0' && in[i] != '-') {
            char c = in[i];
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            out[i] = c;
            ++i;
        }
    }
    out[i] = '\0';
}

extern "C" void FMO_QSO_CORE_Maidenhead(const double lat, const double lon,
                                        char out[7])
{
    if (!isfinite(lat) || !isfinite(lon)) {
        out[0] = '\0';
        return;
    }
    // 经度归一到 [0,360)，纬度收敛到 [0,180)；各留一点余量防浮点上溢。
    double lo = fmod(lon + 180.0, 360.0);
    if (lo < 0.0) lo += 360.0;
    if (lo > 359.999999) lo = 359.999999;
    double la = lat;
    if (la < -90.0) la = -90.0;
    if (la > 90.0) la = 90.0;
    la += 90.0;
    if (la > 179.999999) la = 179.999999;
    out[0] = static_cast<char>('A' + static_cast<int>(lo / 20.0));
    out[1] = static_cast<char>('A' + static_cast<int>(la / 10.0));
    out[2] = static_cast<char>('0' + static_cast<int>(fmod(lo, 20.0) / 2.0));
    out[3] = static_cast<char>('0' + static_cast<int>(fmod(la, 10.0)));
    out[4] = static_cast<char>('a' + static_cast<int>(fmod(lo, 2.0) / 2.0 * 24.0));
    out[5] = static_cast<char>('a' + static_cast<int>(fmod(la, 1.0) * 24.0));
    out[6] = '\0';
}

extern "C" size_t FMO_QSO_CORE_BuildLine(char *out, const size_t cap,
                                         const char *from_call,
                                         const char *to_call,
                                         const char *payload,
                                         const char *msg_id)
{
    if (out == nullptr || cap == 0u || from_call == nullptr ||
        to_call == nullptr || payload == nullptr) {
        return 0u;
    }
    char from[FMO_QSO_CALLSIGN_MAX], to[FMO_QSO_CALLSIGN_MAX];
    copyText(from, sizeof(from), from_call);
    copyText(to, sizeof(to), to_call);
    for (size_t i = 0u; from[i] != '\0'; ++i) {
        if (from[i] >= 'a' && from[i] <= 'z') {
            from[i] = static_cast<char>(from[i] - 'a' + 'A');
        }
    }
    for (size_t i = 0u; to[i] != '\0'; ++i) {
        if (to[i] >= 'a' && to[i] <= 'z') {
            to[i] = static_cast<char>(to[i] - 'a' + 'A');
        }
    }
    const int written = snprintf(out, cap, "%s>APFMO0,TCPIP*::%-9.9s:%s{%s",
                                 from, to, payload,
                                 msg_id != nullptr ? msg_id : "");
    if (written < 0 || static_cast<size_t>(written) >= cap) return 0u;
    return static_cast<size_t>(written);
}

extern "C" bool FMO_QSO_CORE_ParseLine(const char *line, char *from,
                                       const size_t from_cap, char *to,
                                       const size_t to_cap, char *payload,
                                       const size_t payload_cap, char *msg_id,
                                       const size_t msg_id_cap)
{
    if (line == nullptr || from == nullptr || to == nullptr ||
        payload == nullptr || msg_id == nullptr) {
        return false;
    }
    msg_id[0] = '\0';
    const char *gt = strchr(line, '>');
    const char *colon = strchr(line, ':');
    if (gt == nullptr || colon == nullptr || gt >= colon) return false;
    // 信息字段以 ':' 开头（APRS 消息），随后 9 字符 addressee + ':'。
    if (colon[1] != ':' || colon[11] != ':') return false;
    const size_t from_len = static_cast<size_t>(gt - line);
    if (from_len == 0u || from_len >= from_cap) return false;
    memcpy(from, line, from_len);
    from[from_len] = '\0';
    // addressee: 9 字符，空格补齐；去掉尾部空格。
    size_t to_len = 9u;
    while (to_len > 0u && colon[2 + to_len - 1u] == ' ') --to_len;
    if (to_len == 0u || to_len >= to_cap) return false;
    memcpy(to, colon + 2, to_len);
    to[to_len] = '\0';
    const char *body = colon + 12;
    const char *brace = strrchr(body, '{');
    const size_t body_len =
        brace != nullptr ? static_cast<size_t>(brace - body) : strlen(body);
    if (body_len >= payload_cap) return false;
    memcpy(payload, body, body_len);
    payload[body_len] = '\0';
    if (brace != nullptr) {
        copyText(msg_id, msg_id_cap, brace + 1);
    }
    return true;
}

extern "C" size_t FMO_QSO_CORE_BuildQthqry(char *out, const size_t cap,
                                           const uint32_t my_uid,
                                           const uint32_t peer_uid)
{
    int written;
    if (peer_uid != 0u) {
        written = snprintf(out, cap, "QTHQRY,Q%lu,U%lu",
                           static_cast<unsigned long>(my_uid),
                           static_cast<unsigned long>(peer_uid));
    } else {
        written = snprintf(out, cap, "QTHQRY,Q%lu",
                           static_cast<unsigned long>(my_uid));
    }
    if (written < 0 || static_cast<size_t>(written) >= cap) return 0u;
    return static_cast<size_t>(written);
}

extern "C" size_t FMO_QSO_CORE_BuildQthans(char *out, const size_t cap,
                                           const uint32_t my_uid,
                                           const uint32_t srv_uid,
                                           const char *la,
                                           const char *srv_name_utf8)
{
    const int written =
        snprintf(out, cap, "QTHANS,F1,U%lu,S%lu,LA%s,%s",
                 static_cast<unsigned long>(my_uid),
                 static_cast<unsigned long>(srv_uid),
                 la != nullptr ? la : "",
                 srv_name_utf8 != nullptr ? srv_name_utf8 : "");
    if (written < 0 || static_cast<size_t>(written) >= cap) return 0u;
    return static_cast<size_t>(written);
}

extern "C" size_t FMO_QSO_CORE_BuildCall(char *out, const size_t cap,
                                         const uint32_t my_uid,
                                         const uint32_t peer_uid,
                                         const uint32_t srv_uid,
                                         const char *srv_name_utf8)
{
    const int written =
        snprintf(out, cap, "CALL,Q%lu,U%lu,S%lu,%s",
                 static_cast<unsigned long>(my_uid),
                 static_cast<unsigned long>(peer_uid),
                 static_cast<unsigned long>(srv_uid),
                 srv_name_utf8 != nullptr ? srv_name_utf8 : "");
    if (written < 0 || static_cast<size_t>(written) >= cap) return 0u;
    return static_cast<size_t>(written);
}

extern "C" size_t FMO_QSO_CORE_BuildCallans(char *out, const size_t cap,
                                            const char *answer)
{
    const int written = snprintf(out, cap, "CALLANS,%s",
                                 answer != nullptr ? answer : "");
    if (written < 0 || static_cast<size_t>(written) >= cap) return 0u;
    return static_cast<size_t>(written);
}

extern "C" size_t FMO_QSO_CORE_BuildCallcancel(char *out, const size_t cap,
                                               const uint32_t my_uid,
                                               const uint32_t peer_uid)
{
    const int written = snprintf(out, cap, "CALLCANCEL,Q%lu,U%lu",
                                 static_cast<unsigned long>(my_uid),
                                 static_cast<unsigned long>(peer_uid));
    if (written < 0 || static_cast<size_t>(written) >= cap) return 0u;
    return static_cast<size_t>(written);
}

extern "C" void FMO_QSO_CORE_ParseFields(char *fields, bool *has_q,
                                         uint32_t *q, bool *has_u, uint32_t *u,
                                         bool *has_s, uint32_t *s, char *name,
                                         const size_t name_cap)
{
    *has_q = *has_u = *has_s = false;
    if (name != nullptr && name_cap > 0u) name[0] = '\0';
    if (fields == nullptr) return;
    char *cursor = fields;
    while (cursor != nullptr) {
        char *token = cursor;
        char *comma = strchr(cursor, ',');
        if (comma != nullptr) {
            *comma = '\0';
            cursor = comma + 1;
        } else {
            cursor = nullptr;
        }
        // Q/U/S 标记：首字母 + 其余全数字。
        const size_t len = strlen(token);
        bool matched = false;
        if (len >= 2u) {
            bool digits = true;
            for (size_t i = 1u; i < len; ++i) {
                if (token[i] < '0' || token[i] > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                uint32_t value = 0u;
                switch (token[0]) {
                    case 'Q':
                        if (parseUint(token + 1, &value)) {
                            *q = value;
                            *has_q = true;
                            matched = true;
                        }
                        break;
                    case 'U':
                        if (parseUint(token + 1, &value)) {
                            *u = value;
                            *has_u = true;
                            matched = true;
                        }
                        break;
                    case 'S':
                        if (parseUint(token + 1, &value)) {
                            *s = value;
                            *has_s = true;
                            matched = true;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        if (!matched && strncmp(token, "LA", 2u) != 0 && token[0] != 'F' &&
            token[0] != '\0' && name != nullptr && name[0] == '\0') {
            copyText(name, name_cap, token);
        }
    }
}

namespace {

size_t appendEscaped(char *out, const size_t cap, size_t used, const char *text)
{
    if (text == nullptr) text = "";
    for (const char *p = text; *p != '\0' && used + 1u < cap; ++p) {
        const char c = *p;
        const char *escape = nullptr;
        switch (c) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }
        if (escape != nullptr) {
            if (used + 2u >= cap) break;
            out[used++] = escape[0];
            out[used++] = escape[1];
        } else {
            out[used++] = c;
        }
    }
    return used;
}

size_t appendRaw(char *out, const size_t cap, size_t used, const char *text)
{
    for (const char *p = text; *p != '\0' && used + 1u < cap; ++p) {
        out[used++] = *p;
    }
    return used;
}

size_t appendField(char *out, const size_t cap, size_t used, const char *key,
                   const char *value, const bool last)
{
    used = appendRaw(out, cap, used, "\"");
    used = appendRaw(out, cap, used, key);
    used = appendRaw(out, cap, used, "\":\"");
    used = appendEscaped(out, cap, used, value);
    used = appendRaw(out, cap, used, last ? "\"" : "\",");
    return used;
}

} // namespace

extern "C" size_t FMO_QSO_CORE_BuildRecordJson(
    char *out, const size_t cap, const uint32_t log_id,
    const uint64_t timestamp, const uint64_t freq_hz, const char *from_call,
    const char *from_grid, const char *to_call, const char *to_grid,
    const char *to_comment, const char *mode, const char *relay_name,
    const char *relay_admin)
{
    if (out == nullptr || cap == 0u) return 0u;
    char number[24];
    size_t used = appendRaw(out, cap, 0u, "{");
    snprintf(number, sizeof(number), "\"logId\":%lu,",
             static_cast<unsigned long>(log_id));
    used = appendRaw(out, cap, used, number);
    snprintf(number, sizeof(number), "\"timestamp\":%llu,",
             static_cast<unsigned long long>(timestamp));
    used = appendRaw(out, cap, used, number);
    snprintf(number, sizeof(number), "\"freqHz\":%llu,",
             static_cast<unsigned long long>(freq_hz));
    used = appendRaw(out, cap, used, number);
    used = appendField(out, cap, used, "fromCallsign", from_call, false);
    used = appendField(out, cap, used, "fromGrid", from_grid, false);
    used = appendField(out, cap, used, "toCallsign", to_call, false);
    used = appendField(out, cap, used, "toGrid", to_grid, false);
    used = appendField(out, cap, used, "toComment", to_comment, false);
    used = appendField(out, cap, used, "mode", mode, false);
    used = appendField(out, cap, used, "relayName", relay_name, false);
    used = appendField(out, cap, used, "relayAdmin", relay_admin, true);
    used = appendRaw(out, cap, used, "}");
    if (used + 1u >= cap) return 0u;
    out[used] = '\0';
    return used;
}

extern "C" bool FMO_QSO_CORE_StartCall(FmoQsoState *st, const char *peer,
                                       const uint32_t peer_uid,
                                       const int64_t now_s,
                                       FmoQsoAction *acts,
                                       const size_t max_acts,
                                       size_t *act_count, char *err,
                                       const size_t err_cap)
{
    *act_count = 0u;
    char trimmed[FMO_QSO_CALLSIGN_MAX];
    copyText(trimmed, sizeof(trimmed), peer);
    // 去首尾空格并大写（不剥 SSID：addressee 可以带 SSID）。
    size_t len = strlen(trimmed);
    while (len > 0u && trimmed[len - 1u] == ' ') trimmed[--len] = '\0';
    char *start = trimmed;
    while (*start == ' ') ++start;
    for (char *p = start; *p != '\0'; ++p) {
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 'a' + 'A');
    }
    if (*start == '\0') {
        copyText(err, err_cap, "请输入对方呼号");
        return false;
    }
    if (st->phase != FMO_QSO_PHASE_IDLE) {
        copyText(err, err_cap, "当前有进行中的 QSO，请先取消/结束");
        return false;
    }
    if (baseEqual(start, st->my_call)) {
        copyText(err, err_cap, "不能呼叫自己");
        return false;
    }
    copyText(st->peer, sizeof(st->peer), start);
    st->peer_uid = peer_uid;
    char payload[FMO_QSO_PAYLOAD_MAX];
    char msg_id[FMO_QSO_MSG_ID_MAX];
    FMO_QSO_CORE_BuildQthqry(payload, sizeof(payload), st->my_uid, peer_uid);
    nextMsgId(st, msg_id);
    pushSend(acts, max_acts, act_count, st->peer, payload, msg_id);
    st->phase = FMO_QSO_PHASE_OUT_QUERY;
    st->wait_accept = false;
    st->last_sent = now_s;
    st->deadline = now_s + FMO_QSO_QUERY_TIMEOUT_S;
    snprintf(st->detail, sizeof(st->detail), "正在查询 %s 所在服务器…",
             st->peer);
    return true;
}

extern "C" size_t FMO_QSO_CORE_Answer(FmoQsoState *st, const bool accept,
                                      const int64_t now_s, FmoQsoAction *acts,
                                      const size_t max_acts)
{
    size_t count = 0u;
    if (st->phase != FMO_QSO_PHASE_IN_RING) return 0u;
    char payload[FMO_QSO_PAYLOAD_MAX];
    char msg_id[FMO_QSO_MSG_ID_MAX];
    echoMsgId(st, st->reply_msg_id, msg_id);
    if (accept) {
        FMO_QSO_CORE_BuildCallans(payload, sizeof(payload), "ACCEPT");
        pushSend(acts, max_acts, &count, st->peer, payload, msg_id);
        pushLog(acts, max_acts, &count, st->peer, st->peer_uid, false, "已接听");
        FmoQsoAction established = {};
        established.type = FMO_QSO_ACT_ESTABLISHED;
        copyText(established.to, sizeof(established.to), st->peer);
        established.uid = st->peer_uid;
        push(acts, max_acts, &count, established);
        st->phase = FMO_QSO_PHASE_ESTABLISHED;
        st->outgoing = false;
        st->deadline = now_s;
        snprintf(st->detail, sizeof(st->detail), "与 %s 的 QSO 已建立",
                 st->peer);
    } else {
        FMO_QSO_CORE_BuildCallans(payload, sizeof(payload), "REJECT");
        pushSend(acts, max_acts, &count, st->peer, payload, msg_id);
        pushLog(acts, max_acts, &count, st->peer, st->peer_uid, false, "已拒绝");
        st->phase = FMO_QSO_PHASE_IDLE;
        snprintf(st->detail, sizeof(st->detail), "已拒绝 %s 的呼叫", st->peer);
    }
    return count;
}

extern "C" size_t FMO_QSO_CORE_Cancel(FmoQsoState *st, const int64_t now_s,
                                      FmoQsoAction *acts,
                                      const size_t max_acts)
{
    size_t count = 0u;
    char payload[FMO_QSO_PAYLOAD_MAX];
    char msg_id[FMO_QSO_MSG_ID_MAX];
    switch (st->phase) {
        case FMO_QSO_PHASE_OUT_QUERY:
        case FMO_QSO_PHASE_OUT_CALL:
            FMO_QSO_CORE_BuildCallcancel(payload, sizeof(payload), st->my_uid,
                                         st->peer_uid);
            nextMsgId(st, msg_id);
            pushSend(acts, max_acts, &count, st->peer, payload, msg_id);
            pushLog(acts, max_acts, &count, st->peer, st->peer_uid, true,
                    "已取消");
            st->phase = FMO_QSO_PHASE_IDLE;
            snprintf(st->detail, sizeof(st->detail), "已取消对 %s 的呼叫",
                     st->peer);
            break;
        case FMO_QSO_PHASE_ESTABLISHED:
            pushLog(acts, max_acts, &count, st->peer, st->peer_uid,
                    st->outgoing, "已结束");
            st->phase = FMO_QSO_PHASE_IDLE;
            snprintf(st->detail, sizeof(st->detail), "与 %s 的 QSO 已结束",
                     st->peer);
            break;
        case FMO_QSO_PHASE_IN_RING:
            FMO_QSO_CORE_BuildCallans(payload, sizeof(payload), "REJECT");
            echoMsgId(st, st->reply_msg_id, msg_id);
            pushSend(acts, max_acts, &count, st->peer, payload, msg_id);
            pushLog(acts, max_acts, &count, st->peer, st->peer_uid, false,
                    "已拒绝");
            st->phase = FMO_QSO_PHASE_IDLE;
            snprintf(st->detail, sizeof(st->detail), "已拒绝 %s 的呼叫",
                     st->peer);
            break;
        default:
            break;
    }
    (void)now_s;
    return count;
}

namespace {

// 收到 QTHQRY：自动应答本机当前服务器（固件行为：无需人工）。
size_t onQthqry(FmoQsoState *st, const FmoQsoContext *ctx, const char *from,
                char *fields, const char *msg_id, FmoQsoAction *acts,
                const size_t max_acts)
{
    size_t count = 0u;
    bool has_q = false, has_u = false, has_s = false;
    uint32_t q = 0u, u = 0u, s = 0u;
    char name[FMO_QSO_SRV_NAME_MAX + 1u];
    FMO_QSO_CORE_ParseFields(fields, &has_q, &q, &has_u, &u, &has_s, &s, name,
                             sizeof(name));
    (void)has_q;
    (void)q;
    (void)has_s;
    (void)s;
    // U 是查询目标 uid：不是查我就不答（呼号带 SSID 时 to 匹配可能误中）。
    if (has_u && st->my_uid != 0u && u != st->my_uid) return 0u;
    if (ctx == nullptr || ctx->srv_uid == 0u) {
        setDetail(st, "收到服务器查询，但本机未选定服务器，未应答");
        return 0u;
    }
    char payload[FMO_QSO_PAYLOAD_MAX];
    char reply[FMO_QSO_MSG_ID_MAX];
    FMO_QSO_CORE_BuildQthans(payload, sizeof(payload), st->my_uid,
                             ctx->srv_uid, ctx->la, ctx->srv_name);
    echoMsgId(st, msg_id, reply);
    pushSend(acts, max_acts, &count, from, payload, reply);
    snprintf(st->detail, sizeof(st->detail), "已应答 %s 的服务器查询", from);
    return count;
}

// 收到 QTHANS（我是主叫）：跳到对方服务器并发 CALL。
size_t onQthans(FmoQsoState *st, const char *from, char *fields,
                const int64_t now_s, FmoQsoAction *acts, const size_t max_acts)
{
    size_t count = 0u;
    if (st->phase != FMO_QSO_PHASE_OUT_QUERY || !baseEqual(from, st->peer)) {
        return 0u;
    }
    bool has_q = false, has_u = false, has_s = false;
    uint32_t q = 0u, u = 0u, s = 0u;
    char name[FMO_QSO_SRV_NAME_MAX + 1u];
    FMO_QSO_CORE_ParseFields(fields, &has_q, &q, &has_u, &u, &has_s, &s, name,
                             sizeof(name));
    (void)has_q;
    (void)q;
    if (!has_s) {
        setDetail(st, "对方的 QTHANS 缺少服务器编号");
        return 0u;
    }
    if (has_u) st->peer_uid = u; // 主叫从这里学到对方 uid（发布记录要用）
    st->srv_uid = s;
    FmoQsoAction jump = {};
    jump.type = FMO_QSO_ACT_JUMP;
    jump.uid = s;
    push(acts, max_acts, &count, jump);
    char payload[FMO_QSO_PAYLOAD_MAX];
    char msg_id[FMO_QSO_MSG_ID_MAX];
    FMO_QSO_CORE_BuildCall(payload, sizeof(payload), st->my_uid, st->peer_uid,
                           s, name);
    nextMsgId(st, msg_id);
    pushSend(acts, max_acts, &count, st->peer, payload, msg_id);
    st->phase = FMO_QSO_PHASE_OUT_CALL;
    st->wait_accept = false;
    st->deadline = now_s + FMO_QSO_RING_TIMEOUT_S;
    snprintf(st->detail, sizeof(st->detail),
             "%s 在服务器 S%lu，跳台并呼叫…", st->peer,
             static_cast<unsigned long>(s));
    return count;
}

// 收到 CALL（我是被叫）：回 RING + 振铃，等人工接听/拒绝。
size_t onCall(FmoQsoState *st, const char *from, char *fields,
              const char *msg_id, const int64_t now_s, FmoQsoAction *acts,
              const size_t max_acts)
{
    size_t count = 0u;
    bool has_q = false, has_u = false, has_s = false;
    uint32_t q = 0u, u = 0u, s = 0u;
    char name[FMO_QSO_SRV_NAME_MAX + 1u];
    FMO_QSO_CORE_ParseFields(fields, &has_q, &q, &has_u, &u, &has_s, &s, name,
                             sizeof(name));
    (void)has_u;
    (void)u;
    char payload[FMO_QSO_PAYLOAD_MAX];
    char reply[FMO_QSO_MSG_ID_MAX];
    echoMsgId(st, msg_id, reply);
    if (st->phase != FMO_QSO_PHASE_IDLE) {
        FMO_QSO_CORE_BuildCallans(payload, sizeof(payload), "BUSY");
        pushSend(acts, max_acts, &count, from, payload, reply);
        setDetail(st, "忙时收到呼叫，已回 BUSY");
        return count;
    }
    FMO_QSO_CORE_BuildCallans(payload, sizeof(payload), "RING");
    pushSend(acts, max_acts, &count, from, payload, reply);
    copyText(st->peer, sizeof(st->peer), from);
    // CALL 里 Q=主叫 uid（=对方，发布通联记录的目标）、U=被叫 uid（=本机）。
    //（nrl-pulse 参考实现此处误取 U；按协议 "Q=本机 uid, U=对方 uid" 取 Q。）
    st->peer_uid = has_q ? q : 0u;
    st->srv_uid = has_s ? s : 0u;
    copyText(st->reply_msg_id, sizeof(st->reply_msg_id),
             msg_id != nullptr ? msg_id : "");
    st->phase = FMO_QSO_PHASE_IN_RING;
    st->outgoing = false;
    st->deadline = now_s + FMO_QSO_IN_RING_TIMEOUT_S;
    snprintf(st->detail, sizeof(st->detail), "%s 呼入", st->peer);
    return count;
}

// 收到 CALLANS（我是主叫）。
size_t onCallans(FmoQsoState *st, const char *from, char *fields,
                 const int64_t now_s, FmoQsoAction *acts, const size_t max_acts)
{
    size_t count = 0u;
    if (st->phase != FMO_QSO_PHASE_OUT_CALL || !baseEqual(from, st->peer)) {
        return 0u;
    }
    // 应答动词 = 第一个字段。
    const char *answer = fields;
    char *comma = fields != nullptr ? strchr(fields, ',') : nullptr;
    if (comma != nullptr) *comma = '\0';
    if (answer == nullptr) answer = "";
    if (strcmp(answer, "RING") == 0) {
        if (!st->wait_accept) {
            st->wait_accept = true;
            st->deadline = now_s + FMO_QSO_ACCEPT_TIMEOUT_S;
            setDetail(st, "对方振铃中…");
        }
        return 0u;
    }
    if (strcmp(answer, "ACCEPT") == 0) {
        pushLog(acts, max_acts, &count, st->peer, st->peer_uid, true, "接通");
        FmoQsoAction established = {};
        established.type = FMO_QSO_ACT_ESTABLISHED;
        copyText(established.to, sizeof(established.to), st->peer);
        established.uid = st->peer_uid;
        push(acts, max_acts, &count, established);
        st->phase = FMO_QSO_PHASE_ESTABLISHED;
        st->outgoing = true;
        st->deadline = now_s;
        snprintf(st->detail, sizeof(st->detail), "%s 已接听，QSO 建立",
                 st->peer);
        return count;
    }
    const char *text = "对方未应答";
    if (strcmp(answer, "REJECT") == 0) text = "对方拒绝";
    else if (strcmp(answer, "BUSY") == 0) text = "对方忙";
    else if (strcmp(answer, "DND") == 0) text = "对方免打扰";
    else if (strcmp(answer, "NOTFRIEND") == 0) text = "对方未加好友";
    else if (strcmp(answer, "NOSERVER") == 0) text = "对方无服务器";
    else if (strcmp(answer, "TIMEOUT") == 0) text = "对方超时";
    pushLog(acts, max_acts, &count, st->peer, st->peer_uid, true, text);
    st->phase = FMO_QSO_PHASE_IDLE;
    snprintf(st->detail, sizeof(st->detail), "呼叫 %s 失败：%s", st->peer,
             text);
    return count;
}

// 收到 CALLCANCEL（对方取消/结束）。
size_t onCallcancel(FmoQsoState *st, const char *from, FmoQsoAction *acts,
                    const size_t max_acts)
{
    size_t count = 0u;
    if (st->phase == FMO_QSO_PHASE_IN_RING && baseEqual(from, st->peer)) {
        pushLog(acts, max_acts, &count, st->peer, st->peer_uid, false,
                "对方取消");
        st->phase = FMO_QSO_PHASE_IDLE;
        snprintf(st->detail, sizeof(st->detail), "%s 取消了呼叫", st->peer);
    } else if (st->phase == FMO_QSO_PHASE_ESTABLISHED &&
               baseEqual(from, st->peer)) {
        pushLog(acts, max_acts, &count, st->peer, st->peer_uid, st->outgoing,
                "对方结束");
        st->phase = FMO_QSO_PHASE_IDLE;
        snprintf(st->detail, sizeof(st->detail), "%s 结束了 QSO", st->peer);
    }
    return count;
}

} // namespace

extern "C" size_t FMO_QSO_CORE_OnMessage(FmoQsoState *st,
                                         const FmoQsoContext *ctx,
                                         const char *from, const char *verb,
                                         char *fields, const char *msg_id,
                                         const int64_t now_s,
                                         FmoQsoAction *acts,
                                         const size_t max_acts)
{
    if (st == nullptr || from == nullptr || verb == nullptr) return 0u;
    if (strcmp(verb, "QTHQRY") == 0) {
        return onQthqry(st, ctx, from, fields, msg_id, acts, max_acts);
    }
    if (strcmp(verb, "QTHANS") == 0) {
        return onQthans(st, from, fields, now_s, acts, max_acts);
    }
    if (strcmp(verb, "CALL") == 0) {
        return onCall(st, from, fields, msg_id, now_s, acts, max_acts);
    }
    if (strcmp(verb, "CALLANS") == 0) {
        return onCallans(st, from, fields, now_s, acts, max_acts);
    }
    if (strcmp(verb, "CALLCANCEL") == 0) {
        return onCallcancel(st, from, acts, max_acts);
    }
    return 0u;
}

extern "C" size_t FMO_QSO_CORE_Tick(FmoQsoState *st, const int64_t now_s,
                                    FmoQsoAction *acts, const size_t max_acts)
{
    size_t count = 0u;
    if (st == nullptr) return 0u;
    char payload[FMO_QSO_PAYLOAD_MAX];
    char msg_id[FMO_QSO_MSG_ID_MAX];
    switch (st->phase) {
        case FMO_QSO_PHASE_OUT_QUERY:
            if (now_s >= st->deadline) {
                pushLog(acts, max_acts, &count, st->peer, st->peer_uid, true,
                        "查询无应答");
                st->phase = FMO_QSO_PHASE_IDLE;
                snprintf(st->detail, sizeof(st->detail),
                         "%s 未应答服务器查询", st->peer);
            } else if (now_s - st->last_sent >= FMO_QSO_QUERY_RETRY_S) {
                // 重发 QTHQRY（msgId 递增，与原厂固件一致）。
                FMO_QSO_CORE_BuildQthqry(payload, sizeof(payload), st->my_uid,
                                         st->peer_uid);
                nextMsgId(st, msg_id);
                pushSend(acts, max_acts, &count, st->peer, payload, msg_id);
                st->last_sent = now_s;
            }
            break;
        case FMO_QSO_PHASE_OUT_CALL:
            if (now_s >= st->deadline) {
                const char *text =
                    st->wait_accept ? "对方未接听（超时）" : "对方无应答";
                pushLog(acts, max_acts, &count, st->peer, st->peer_uid, true,
                        text);
                st->phase = FMO_QSO_PHASE_IDLE;
                snprintf(st->detail, sizeof(st->detail), "呼叫 %s 失败：%s",
                         st->peer, text);
            }
            break;
        case FMO_QSO_PHASE_IN_RING:
            if (now_s >= st->deadline) {
                pushLog(acts, max_acts, &count, st->peer, st->peer_uid, false,
                        "未接来电");
                st->phase = FMO_QSO_PHASE_IDLE;
                snprintf(st->detail, sizeof(st->detail), "未接来电：%s",
                         st->peer);
            }
            break;
        default:
            break;
    }
    return count;
}

extern "C" size_t FMO_QSO_CORE_JumpFailed(FmoQsoState *st, const int64_t now_s,
                                          FmoQsoAction *acts,
                                          const size_t max_acts)
{
    size_t count = 0u;
    (void)now_s;
    if (st == nullptr || st->phase != FMO_QSO_PHASE_OUT_CALL) return 0u;
    pushLog(acts, max_acts, &count, st->peer, st->peer_uid, true, "跳台失败");
    st->phase = FMO_QSO_PHASE_IDLE;
    snprintf(st->detail, sizeof(st->detail),
             "跳台失败：服务器 S%lu 不在已知列表",
             static_cast<unsigned long>(st->srv_uid));
    return count;
}

extern "C" const char *FMO_QSO_CORE_PhaseName(const FmoQsoPhase phase)
{
    switch (phase) {
        case FMO_QSO_PHASE_OUT_QUERY: return "querying";
        case FMO_QSO_PHASE_OUT_CALL: return "calling";
        case FMO_QSO_PHASE_IN_RING: return "incoming";
        case FMO_QSO_PHASE_ESTABLISHED: return "established";
        default: return "idle";
    }
}
