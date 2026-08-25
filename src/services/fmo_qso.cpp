#include "services/fmo_qso.h"

#include "audio/audio_router.h"
#include "services/aprs_service.h"
#include "services/fmo_cert_store.h"
#include "services/fmo_qso_core.h"
#include "services/fmo_service.h"
#include "services/fmo_station_broadcast.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

constexpr const char *TAG = "FMO-QSO";
constexpr const char *kNvsNamespace = "fmo";
constexpr const char *kNvsLogKey = "qsolog";
constexpr const char *kNvsLogIdKey = "qsologid";
constexpr uint32_t kLogMagic = 0x4c535146u; // "FQSL"
constexpr uint16_t kLogVersion = 1u;
constexpr size_t kLogMax = 16u;
constexpr size_t kMaxActs = 8u;
constexpr size_t kDedupMax = 8u;
constexpr uint32_t kRingBeepPeriodS = 2u;

struct PersistedQsoLog {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t head;   // 下一条写入位置
    uint32_t total;  // 历史总条数（logId 基数）
    FmoQsoLogEntry entries[kLogMax];
    uint32_t crc32;
};

FmoQsoState s_state = {};
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
bool s_initialized = false;

PersistedQsoLog s_log = {};
bool s_log_loaded = false;
SemaphoreHandle_t s_log_mutex = nullptr;

// 原始报文去重（发现连接与 APRS 上行回显可能投递同一条信令）：
// 键 = from|to|verb|msgId。
char s_seen[kDedupMax][128] = {};
size_t s_seen_head = 0u;

uint32_t crcLog(const PersistedQsoLog *log)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(log),
                            offsetof(PersistedQsoLog, crc32));
}

void loadLog(void)
{
    if (s_log_loaded) return;
    s_log_loaded = true;
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = sizeof(s_log);
    PersistedQsoLog stored = {};
    const esp_err_t error = nvs_get_blob(nvs, kNvsLogKey, &stored, &size);
    nvs_close(nvs);
    if (error == ESP_OK && size == sizeof(stored) &&
        stored.magic == kLogMagic && stored.version == kLogVersion &&
        stored.count <= kLogMax && stored.head < kLogMax &&
        stored.crc32 == crcLog(&stored)) {
        s_log = stored;
        ESP_LOGI(TAG, "restored %u QSO log entries",
                 static_cast<unsigned>(s_log.count));
    } else {
        memset(&s_log, 0, sizeof(s_log));
        s_log.magic = kLogMagic;
        s_log.version = kLogVersion;
    }
}

bool saveLog(void)
{
    s_log.crc32 = crcLog(&s_log);
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return false;
    const bool ok = nvs_set_blob(nvs, kNvsLogKey, &s_log, sizeof(s_log)) ==
                        ESP_OK &&
                    nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

void logAppend(const int64_t ts, const uint8_t kind, const bool out,
               const char *peer, const uint32_t peer_uid, const char *result,
               const char *comment, const char *grid, const char *relay)
{
    FmoQsoLogEntry entry = {};
    entry.ts = ts;
    entry.kind = kind;
    entry.out = out ? 1u : 0u;
    snprintf(entry.peer, sizeof(entry.peer), "%s", peer != nullptr ? peer : "");
    entry.peer_uid = peer_uid;
    snprintf(entry.result, sizeof(entry.result), "%s",
             result != nullptr ? result : "");
    snprintf(entry.comment, sizeof(entry.comment), "%s",
             comment != nullptr ? comment : "");
    snprintf(entry.grid, sizeof(entry.grid), "%s", grid != nullptr ? grid : "");
    snprintf(entry.relay, sizeof(entry.relay), "%s",
             relay != nullptr ? relay : "");
    if (s_log_mutex != nullptr) xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    loadLog();
    s_log.entries[s_log.head] = entry;
    s_log.head = (s_log.head + 1u) % kLogMax;
    if (s_log.count < kLogMax) ++s_log.count;
    ++s_log.total;
    const bool saved = saveLog();
    if (s_log_mutex != nullptr) xSemaphoreGive(s_log_mutex);
    if (!saved) ESP_LOGW(TAG, "QSO log persist failed");
}

// established 记录的自增 logId（独立 NVS 计数器）。
uint32_t nextLogId(void)
{
    uint32_t id = 1u;
    nvs_handle_t nvs = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_get_u32(nvs, kNvsLogIdKey, &id);
        const uint32_t next = id + 1u;
        (void)nvs_set_u32(nvs, kNvsLogIdKey, next);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
        return id;
    }
    return static_cast<uint32_t>(time(nullptr));
}

void refreshIdentity(void)
{
    FmoIdentityStatus identity = {};
    const bool ready =
        FMO_CERT_GetStatus(&identity) == ESP_OK && identity.ready;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FMO_QSO_CORE_SetIdentity(&s_state, ready ? identity.callsign : "",
                             ready ? identity.uid : 0u);
    xSemaphoreGive(s_mutex);
}

bool myBaseCall(char *out, const size_t cap)
{
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) != ESP_OK || !identity.ready) {
        out[0] = '\0';
        return false;
    }
    FMO_QSO_CORE_BaseCall(identity.callsign, out, cap);
    return out[0] != '\0';
}

void currentContext(FmoQsoContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    FmoConfig config = {};
    FMO_GetConfig(&config);
    ctx->srv_uid = config.enabled ? config.server.uid : 0u;
    const char *name = config.server.name[0] != '\0'
                           ? config.server.name
                           : config.server.callsign;
    snprintf(ctx->srv_name, sizeof(ctx->srv_name), "%s", name);
    time_t now = time(nullptr);
    struct tm utc = {};
    gmtime_r(&now, &utc);
    strftime(ctx->la, sizeof(ctx->la), "%Y%m%d%H%M%SZ", &utc);
}

// established 单一挂点：向对方 FMO/QSO/UID/<uid> 发记录 JSON。
void publishQsoRecord(const char *peer, const uint32_t peer_uid)
{
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) != ESP_OK || !identity.ready) return;
    FmoBeaconConfig bcn = {};
    FMO_BEACON_GetConfig(&bcn);
    double lat = 0.0, lon = 0.0;
    (void)APRS_SERVICE_GetOwnPosition(&lat, &lon, nullptr);
    char grid[7];
    FMO_QSO_CORE_Maidenhead(lat, lon, grid);
    FmoConfig config = {};
    FMO_GetConfig(&config);
    const char *relay_name = config.server.name[0] != '\0'
                                 ? config.server.name
                                 : config.server.callsign;
    static char json[1024];
    const size_t json_size = FMO_QSO_CORE_BuildRecordJson(
        json, sizeof(json), nextLogId(),
        static_cast<uint64_t>(time(nullptr)),
        static_cast<uint64_t>(bcn.freq_x10000) * 100u, identity.callsign,
        grid, peer, "", bcn.qso_msg, "FMO", relay_name,
        config.server.callsign);
    if (json_size == 0u) {
        ESP_LOGW(TAG, "QSO record JSON build failed");
        return;
    }
    char topic[48];
    snprintf(topic, sizeof(topic), "FMO/QSO/UID/%lu",
             static_cast<unsigned long>(peer_uid));
    if (FMO_PublishMessage(topic, json, static_cast<int>(json_size))) {
        ESP_LOGI(TAG, "QSO record published to %s (%u bytes)", topic,
                 static_cast<unsigned>(json_size));
    } else {
        ESP_LOGW(TAG, "QSO record publish failed (MQTT link down?)");
    }
}

bool jumpToServer(const uint32_t srv_uid)
{
    const size_t count = FMO_ServerCount();
    for (size_t i = 0u; i < count; ++i) {
        FmoServer server = {};
        if (!FMO_GetServer(i, &server)) continue;
        if (server.uid == srv_uid) {
            ESP_LOGI(TAG, "jumping to server S%lu (%s %s:%u)",
                     static_cast<unsigned long>(srv_uid), server.name,
                     server.host, static_cast<unsigned>(server.port));
            return FMO_SelectServer(i, true);
        }
    }
    return false;
}

// 执行状态机排队的一组动作（在 s_mutex 之外调用）。
void drainActions(FmoQsoAction *acts, const size_t count)
{
    char my_call[FMO_QSO_CALLSIGN_MAX];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(my_call, sizeof(my_call), "%s", s_state.my_call);
    xSemaphoreGive(s_mutex);
    for (size_t i = 0u; i < count; ++i) {
        const FmoQsoAction &action = acts[i];
        switch (action.type) {
            case FMO_QSO_ACT_SEND: {
                char line[FMO_QSO_LINE_MAX];
                const size_t size = FMO_QSO_CORE_BuildLine(
                    line, sizeof(line), my_call, action.to,
                    action.payload, action.msg_id);
                if (size == 0u) {
                    ESP_LOGW(TAG, "APFMO0 line build failed");
                    break;
                }
                if (!APRS_SERVICE_SendRawLine(line)) {
                    ESP_LOGW(TAG, "APRS uplink busy, dropped: %s", line);
                } else {
                    ESP_LOGI(TAG, "QSO send: %s", line);
                }
                break;
            }
            case FMO_QSO_ACT_JUMP:
                if (!jumpToServer(action.uid)) {
                    ESP_LOGW(TAG, "server S%lu unknown, call aborted",
                             static_cast<unsigned long>(action.uid));
                    FmoQsoAction fail_acts[kMaxActs];
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    const size_t n = FMO_QSO_CORE_JumpFailed(
                        &s_state, static_cast<int64_t>(time(nullptr)),
                        fail_acts, kMaxActs);
                    xSemaphoreGive(s_mutex);
                    drainActions(fail_acts, n);
                    return; // 跳台失败：不再发送本批后续动作（CALL）
                }
                break;
            case FMO_QSO_ACT_ESTABLISHED:
                publishQsoRecord(action.to, action.uid);
                break;
            case FMO_QSO_ACT_LOG:
                logAppend(static_cast<int64_t>(time(nullptr)), 0u, action.out,
                          action.to, action.uid, action.text, nullptr,
                          nullptr, nullptr);
                break;
            default:
                break;
        }
    }
}

void ringBeep(void)
{
    // 880 Hz 双音振铃短促音（120 ms @8 kHz），走 FMO 下行的常驻扬声器路由。
    static int16_t pcm[960];
    static bool generated = false;
    if (!generated) {
        for (size_t i = 0u; i < 960u; ++i) {
            const double t = static_cast<double>(i) / 8000.0;
            const double envelope =
                i < 100u ? static_cast<double>(i) / 100.0
                         : (960u - i) < 100u
                               ? static_cast<double>(960u - i) / 100.0
                               : 1.0;
            pcm[i] = static_cast<int16_t>(sin(2.0 * M_PI * 880.0 * t) *
                                          12000.0 * envelope);
        }
        generated = true;
    }
    AudioRouter_PushFrame(AUDIO_SRC_FMO_DOWNLINK, 8000u, pcm, 960u);
}

void qsoTask(void *)
{
    bool beep_phase = false;
    for (;;) {
        refreshIdentity();
        FmoQsoAction acts[kMaxActs];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        const size_t n = FMO_QSO_CORE_Tick(
            &s_state, static_cast<int64_t>(time(nullptr)), acts, kMaxActs);
        const bool ringing = s_state.phase == FMO_QSO_PHASE_IN_RING;
        xSemaphoreGive(s_mutex);
        drainActions(acts, n);
        if (ringing) {
            if (!beep_phase) ringBeep();
            beep_phase = !beep_phase; // 每 2s 一响
        } else {
            beep_phase = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool dedupSeen(const char *key)
{
    for (size_t i = 0u; i < kDedupMax; ++i) {
        if (s_seen[i][0] != '\0' && strcmp(s_seen[i], key) == 0) return true;
    }
    snprintf(s_seen[s_seen_head], sizeof(s_seen[0]), "%s", key);
    s_seen_head = (s_seen_head + 1u) % kDedupMax;
    return false;
}

} // namespace

extern "C" bool FMO_QSO_Init(void)
{
    if (s_initialized) return true;
    FMO_QSO_CORE_Init(&s_state);
    s_mutex = xSemaphoreCreateMutex();
    s_log_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr || s_log_mutex == nullptr) {
        ESP_LOGE(TAG, "mutex creation failed");
        return false;
    }
    refreshIdentity();
    if (xTaskCreate(qsoTask, "fmo_qso", 6144, nullptr, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "worker task creation failed");
        return false;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "FMO QSO signaling ready");
    return true;
}

extern "C" void FMO_QSO_GetSnapshot(FmoQsoSnapshot *out)
{
    if (out == nullptr) return;
    memset(out, 0, sizeof(*out));
    if (s_mutex == nullptr) {
        snprintf(out->phase_name, sizeof(out->phase_name), "idle");
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->phase = static_cast<uint8_t>(s_state.phase);
    snprintf(out->phase_name, sizeof(out->phase_name), "%s",
             FMO_QSO_CORE_PhaseName(s_state.phase));
    snprintf(out->peer, sizeof(out->peer), "%s", s_state.peer);
    out->peer_uid = s_state.peer_uid;
    out->outgoing = s_state.outgoing;
    snprintf(out->detail, sizeof(out->detail), "%s", s_state.detail);
    xSemaphoreGive(s_mutex);
}

extern "C" bool FMO_QSO_StartCall(const char *peer, const uint32_t peer_uid,
                                  char *err, const size_t err_cap)
{
    if (err != nullptr && err_cap > 0u) err[0] = '\0';
    FmoIdentityStatus identity = {};
    if (FMO_CERT_GetStatus(&identity) != ESP_OK || !identity.ready) {
        if (err != nullptr) {
            snprintf(err, err_cap, "FMO 身份证书未就绪");
        }
        return false;
    }
    if (!APRS_SERVICE_IsNetVerified()) {
        if (err != nullptr) {
            snprintf(err, err_cap,
                     "APRS 上行未验证登录（先连接 APRS 且 passcode 正确）");
        }
        return false;
    }
    refreshIdentity();
    FmoQsoAction acts[kMaxActs];
    size_t n = 0u;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool ok = FMO_QSO_CORE_StartCall(
        &s_state, peer, peer_uid, static_cast<int64_t>(time(nullptr)), acts,
        kMaxActs, &n, err, err_cap);
    xSemaphoreGive(s_mutex);
    if (ok) drainActions(acts, n);
    return ok;
}

extern "C" bool FMO_QSO_Answer(const bool accept)
{
    FmoQsoAction acts[kMaxActs];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const size_t n = FMO_QSO_CORE_Answer(
        &s_state, accept, static_cast<int64_t>(time(nullptr)), acts, kMaxActs);
    xSemaphoreGive(s_mutex);
    drainActions(acts, n);
    return n > 0u;
}

extern "C" void FMO_QSO_Cancel(void)
{
    FmoQsoAction acts[kMaxActs];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const size_t n = FMO_QSO_CORE_Cancel(
        &s_state, static_cast<int64_t>(time(nullptr)), acts, kMaxActs);
    xSemaphoreGive(s_mutex);
    drainActions(acts, n);
}

extern "C" bool FMO_QSO_IncomingRing(void)
{
    if (s_mutex == nullptr) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool ringing = s_state.phase == FMO_QSO_PHASE_IN_RING;
    xSemaphoreGive(s_mutex);
    return ringing;
}

extern "C" void FMO_QSO_HandleAprsLine(const char *line)
{
    if (!s_initialized || line == nullptr) return;
    char from[20], to[20], payload[FMO_QSO_PAYLOAD_MAX],
        msg_id[FMO_QSO_MSG_ID_MAX];
    if (!FMO_QSO_CORE_ParseLine(line, from, sizeof(from), to, sizeof(to),
                                payload, sizeof(payload), msg_id,
                                sizeof(msg_id))) {
        return;
    }
    char mine[FMO_QSO_CALLSIGN_MAX];
    if (!myBaseCall(mine, sizeof(mine))) return;
    char to_base[FMO_QSO_CALLSIGN_MAX];
    FMO_QSO_CORE_BaseCall(to, to_base, sizeof(to_base));
    if (strcmp(to_base, mine) != 0) return; // 不是发给我的
    // 动词 = 载荷第一个逗号前；fields 原地切割供核心解析。
    char scratch[FMO_QSO_PAYLOAD_MAX];
    snprintf(scratch, sizeof(scratch), "%s", payload);
    char *comma = strchr(scratch, ',');
    char *fields = nullptr;
    if (comma != nullptr) {
        *comma = '\0';
        fields = comma + 1;
    } else {
        fields = scratch + strlen(scratch);
    }
    char key[128];
    snprintf(key, sizeof(key), "%s|%s|%s|%s", from, to, scratch, msg_id);
    if (dedupSeen(key)) return;
    FmoQsoContext ctx;
    currentContext(&ctx);
    FmoQsoAction acts[kMaxActs];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const size_t n = FMO_QSO_CORE_OnMessage(
        &s_state, &ctx, from, scratch, fields, msg_id,
        static_cast<int64_t>(time(nullptr)), acts, kMaxActs);
    xSemaphoreGive(s_mutex);
    drainActions(acts, n);
}

extern "C" void FMO_QSO_OnMqttRecord(const char *data, const int len)
{
    if (!s_initialized || data == nullptr || len <= 0) return;
    cJSON *root = cJSON_ParseWithLength(data, static_cast<size_t>(len));
    if (root == nullptr) {
        ESP_LOGW(TAG, "ignoring malformed QSO record (%d bytes)", len);
        return;
    }
    const cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    const cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "fromCallsign");
    const cJSON *grid = cJSON_GetObjectItemCaseSensitive(root, "fromGrid");
    const cJSON *comment = cJSON_GetObjectItemCaseSensitive(root, "toComment");
    const cJSON *relay = cJSON_GetObjectItemCaseSensitive(root, "relayName");
    const int64_t ts = cJSON_IsNumber(timestamp)
                           ? static_cast<int64_t>(timestamp->valuedouble)
                           : static_cast<int64_t>(time(nullptr));
    logAppend(ts, 1u, false, cJSON_IsString(from) ? from->valuestring : "",
              0u, "通联记录",
              cJSON_IsString(comment) ? comment->valuestring : "",
              cJSON_IsString(grid) ? grid->valuestring : "",
              cJSON_IsString(relay) ? relay->valuestring : "");
    ESP_LOGI(TAG, "QSO record from %s stored",
             cJSON_IsString(from) ? from->valuestring : "?");
    cJSON_Delete(root);
}

extern "C" size_t FMO_QSO_LogCount(void)
{
    if (s_log_mutex != nullptr) xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    loadLog();
    const size_t count = s_log.count;
    if (s_log_mutex != nullptr) xSemaphoreGive(s_log_mutex);
    return count;
}

extern "C" bool FMO_QSO_LogGet(const size_t index, FmoQsoLogEntry *out)
{
    if (out == nullptr) return false;
    if (s_log_mutex != nullptr) xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    loadLog();
    const bool valid = index < s_log.count;
    if (valid) {
        // index 0 = 最新（head 前一条）。
        *out = s_log.entries[(s_log.head + kLogMax - 1u - index) % kLogMax];
    }
    if (s_log_mutex != nullptr) xSemaphoreGive(s_log_mutex);
    return valid;
}
