#include "services/speaker_info.h"

#include "services/aprs_service.h"
#include "services/fmo_qso_core.h"
#include "services/fmo_service.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace {

// 梅登黑德 4/6 位网格 → 方格中心经纬度（与 FMO_QSO_CORE_Maidenhead 互逆；
// 对齐原厂固件 Position::fromGrid：非法输入返回 false 而不是默认 (-180,-90)）。
bool gridToLatLon(const char *grid, double *lat, double *lon)
{
    if (grid == nullptr) return false;
    char g[7];
    size_t n = 0;
    while (grid[n] != '\0' && n < 7) {
        g[n] = static_cast<char>(toupper(static_cast<unsigned char>(grid[n])));
        ++n;
    }
    if (n != 4 && n != 6) return false;
    auto field = [](char c) -> double {
        return (c >= 'A' && c <= 'R') ? static_cast<double>(c - 'A') : -1.0;
    };
    auto sub = [](char c) -> double {
        // 子方字母范围 A..X（24x24），原厂 toupper 后同样按此范围
        return (c >= 'A' && c <= 'X') ? static_cast<double>(c - 'A') : -1.0;
    };
    auto dig = [](char c) -> double {
        return (c >= '0' && c <= '9') ? static_cast<double>(c - '0') : -1.0;
    };
    const double f0 = field(g[0]), f1 = field(g[1]);
    const double d2 = dig(g[2]), d3 = dig(g[3]);
    if (f0 < 0.0 || f1 < 0.0 || d2 < 0.0 || d3 < 0.0) return false;
    double lo = f0 * 20.0 + d2 * 2.0;
    double la = f1 * 10.0 + d3;
    if (n == 4) {
        // 4 位方格 2°x1°，取中心
        *lat = la - 90.0 + 0.5;
        *lon = lo - 180.0 + 1.0;
        return true;
    }
    const double s4 = sub(g[4]), s5 = sub(g[5]);
    if (s4 < 0.0 || s5 < 0.0) return false;
    // 6 位子方 5'x2.5'，取中心
    *lat = la - 90.0 + s5 * 2.5 / 60.0 + 1.25 / 60.0;
    *lon = lo - 180.0 + s4 * 5.0 / 60.0 + 2.5 / 60.0;
    return true;
}

// 大圆距离（米）与初始方位角（度）。与原厂固件同参数：f64、R=6371000。
void geoDistanceBearing(double lat1, double lon1, double lat2, double lon2,
                        double *dist_m, double *bearing_deg)
{
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double p1 = lat1 * kDeg2Rad, p2 = lat2 * kDeg2Rad;
    const double dp = (lat2 - lat1) * kDeg2Rad, dl = (lon2 - lon1) * kDeg2Rad;
    const double a = pow(sin(dp / 2.0), 2.0) +
                     cos(p1) * cos(p2) * pow(sin(dl / 2.0), 2.0);
    *dist_m = 6371000.0 * 2.0 * asin(sqrt(a));
    const double y = sin(dl) * cos(p2);
    const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double brg = atan2(y, x) / kDeg2Rad;
    if (brg < 0.0) brg += 360.0;
    *bearing_deg = brg;
}

// 16 方位罗盘（与原厂字符串表一致）
const char *compass16(double deg)
{
    static const char *kDirs[16] = {"N",   "NNE", "NE", "ENE", "E",  "ESE",
                                    "SE",  "SSE", "S",  "SSW", "SW", "WSW",
                                    "W",   "WNW", "NW", "NNW"};
    int idx = static_cast<int>((deg + 11.25) / 22.5) % 16;
    if (idx < 0) idx += 16;
    return kDirs[idx];
}

// 从 APRS 信标注释提取 FMO BEACON 字段：FREQ/RIG/ANT/HEIGHT（逗号分隔令牌）。
// 注释形如 "FMO-V4,BEACON,CERT:…,FREQ:438.5000,HEIGHT:50,RIG:…,ANT:…,SIG:…"。
void parseBeaconComment(const char *comment, SpeakerInfo *out)
{
    if (comment == nullptr || strstr(comment, "FREQ:") == nullptr) return;
    char buf[APRS_COMMENT_MAX_BYTES + 1u];
    snprintf(buf, sizeof(buf), "%s", comment);
    char *save = nullptr;
    for (char *tok = strtok_r(buf, ",", &save); tok != nullptr;
         tok = strtok_r(nullptr, ",", &save)) {
        if (strncmp(tok, "FREQ:", 5) == 0) {
            const float v = strtof(tok + 5, nullptr);
            if (v > 0.0f) out->freq_mhz = v;
        } else if (strncmp(tok, "HEIGHT:", 7) == 0) {
            const long v = strtol(tok + 7, nullptr, 10);
            if (v >= 0 && v < 9000) out->height_m = static_cast<int16_t>(v);
        } else if (strncmp(tok, "RIG:", 4) == 0) {
            snprintf(out->rig, sizeof(out->rig), "%s", tok + 4);
        } else if (strncmp(tok, "ANT:", 4) == 0) {
            snprintf(out->ant, sizeof(out->ant), "%s", tok + 4);
        }
    }
}

// 呼号归一化：去 -SSID、大写、限 6 字符（FMO 语音帧头呼号就是 6B base）。
void baseCallsign(const char *in, char out[8])
{
    size_t n = 0;
    while (in[n] != '\0' && in[n] != '-' && n < 6) {
        out[n] = static_cast<char>(toupper(static_cast<unsigned char>(in[n])));
        ++n;
    }
    out[n] = '\0';
}

} // namespace

extern "C" bool SPEAKER_INFO_Lookup(const char *callsign, SpeakerInfo *out)
{
    if (out == nullptr) return false;
    memset(out, 0, sizeof(*out));
    out->height_m = -1;
    if (callsign == nullptr || callsign[0] == '\0') return false;

    char base[8];
    baseCallsign(callsign, base);
    if (base[0] == '\0') return false;

    // 1) APRS 电台表（精确坐标；注释含 FREQ/RIG/ANT/HEIGHT）
    AprsStationInfo st;
    if (APRS_SERVICE_FindStation(base, &st)) {
        if (!isnan(st.distance_km)) {
            FMO_QSO_CORE_Maidenhead(static_cast<double>(st.lat),
                                    static_cast<double>(st.lon), out->grid);
            out->distance_km = st.distance_km;
            out->bearing_deg = st.bearing_deg;
            out->has_geo = true;
            out->geo_from_grid = false;
        }
        parseBeaconComment(st.comment, out);
        return true;
    }

    // 2) FMO 成员网格花名册（QSO 会话 / NRL 桥转发，±10km）
    char grid[7];
    if (FMO_LookupMemberGrid(base, grid)) {
        double la = 0.0, lo = 0.0;
        if (gridToLatLon(grid, &la, &lo)) {
            snprintf(out->grid, sizeof(out->grid), "%s", grid);
            double own_la = 0.0, own_lo = 0.0, own_alt = 0.0;
            APRS_SERVICE_GetOwnPosition(&own_la, &own_lo, &own_alt);
            if (own_la != 0.0 || own_lo != 0.0) {
                double dist_m = 0.0, brg = 0.0;
                geoDistanceBearing(own_la, own_lo, la, lo, &dist_m, &brg);
                out->distance_km = static_cast<float>(dist_m / 1000.0);
                out->bearing_deg = static_cast<uint16_t>(brg + 0.5) % 360u;
            } else {
                out->distance_km = NAN;
            }
            out->has_geo = true;
            out->geo_from_grid = true;
        }
        return out->has_geo;
    }
    return false;
}

extern "C" void SPEAKER_INFO_FormatGeo(const SpeakerInfo *info, char *out,
                                       size_t out_size)
{
    if (out == nullptr || out_size == 0) return;
    out[0] = '\0';
    if (info == nullptr || !info->has_geo) return;
    if (isnan(info->distance_km)) {
        snprintf(out, out_size, "%s", info->grid);
        return;
    }
    // 与原厂一致：(米+500)/1000 取整公里
    const unsigned km =
        static_cast<unsigned>(info->distance_km + 0.5f);
    snprintf(out, out_size, "%s | %s%ukm | %s%u", info->grid,
             info->geo_from_grid ? "~" : "", km,
             compass16(static_cast<double>(info->bearing_deg)),
             static_cast<unsigned>(info->bearing_deg));
}

extern "C" void SPEAKER_INFO_FormatRig(const SpeakerInfo *info, char *out,
                                       size_t out_size)
{
    if (out == nullptr || out_size == 0) return;
    out[0] = '\0';
    if (info == nullptr) return;
    size_t used = 0;
    auto append = [&](const char *piece) {
        if (piece[0] == '\0') return;
        const size_t room = out_size - used;
        if (room < 4) return;
        const int w = snprintf(out + used, room, "%s%s",
                               used > 0 ? " | " : "", piece);
        if (w > 0) used += static_cast<size_t>(w);
    };
    char freq[24] = {};
    if (info->freq_mhz > 0.0f) {
        snprintf(freq, sizeof(freq), "%.4fMHz",
                 static_cast<double>(info->freq_mhz));
    }
    char height[16] = {};
    if (info->height_m >= 0) {
        snprintf(height, sizeof(height), "%dm",
                 static_cast<int>(info->height_m));
    }
    append(freq);
    append(info->rig);
    append(info->ant);
    append(height);
}
