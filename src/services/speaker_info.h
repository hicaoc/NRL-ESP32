#pragma once

// 说话人附加信息（对齐 nrl-pulse / 原厂固件）：
// 在语音接收时为主屏呼号区提供「网格 | 距离 | 方位」与「频率 | 机型 | 高度」。
//
// 位置来源优先级：
//   1) APRS 电台表（本机 m/100 范围内的信标，精确坐标；注释里带 FREQ/RIG/ANT/HEIGHT）
//   2) FMO 成员网格花名册（FMO/QSO/UID/# 成员 JSON，含 NRL 桥 [json] 跨服务器转发；
//      网格方格中心推算，±10km 级，距离前加 ~ 标注）
//
// 距离/方位算法与原厂固件同参数：大圆 f64、R=6371000m、(米+500)/1000 取整公里、
// 16 方位罗盘 N/NNE/.../NNW。

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_geo;        // 有位置可显示
    bool geo_from_grid;  // true = 成员网格推算（±10km，距离前加 ~）
    char grid[7];        // 梅登黑德 6 位（无位置时为空串）
    float distance_km;   // NAN = 本机位置未知
    uint16_t bearing_deg; // 初始方位角 0..359（0=北）
    float freq_mhz;      // <=0 = 无（信标 FREQ 字段）
    int16_t height_m;    // <0 = 无（信标 HEIGHT 字段）
    char rig[25];        // 机型（信标 RIG 原始字节，可能为 GBK 编码）
    char ant[25];        // 天线/QTH（信标 ANT 原始字节）
} SpeakerInfo;

// 按呼号查说话人信息（忽略大小写与 -SSID）。无任何可用信息时返回 false。
bool SPEAKER_INFO_Lookup(const char *callsign, SpeakerInfo *out);

// 位置行："OM92jd | 889km | SSE165"；网格源距离 "~889km"；仅有网格无本机
// 位置时输出网格本身；无位置输出空串。
void SPEAKER_INFO_FormatGeo(const SpeakerInfo *info, char *out, size_t out_size);

// 电台行："438.5000MHz | UV-K1 | 50m"；没有任何字段时输出空串。
void SPEAKER_INFO_FormatRig(const SpeakerInfo *info, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
