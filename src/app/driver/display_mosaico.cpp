// ESP-Mosaico display UI: 480x480 QSPI AMOLED (CO5300) + CST9220 touch.
//
// LVGL port (no esp_lvgl_port, same pattern as display_s31.cpp): two PSRAM
// draw buffers in PARTIAL render mode, flush pushes dirty areas to the panel
// over QSPI. On top of that sits the square-portrait multi-page touch UI
// (Home / Music / Sensors / Settings) in the "AMOLED Dark" theme.

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include "display.h"
#include "display_mosaico_panel.h"
#include "external_radio.h"
#include "i2c1.h"
#include "mosaico_sensors.h"
#include "status_io.h"

#include "../../lib/nrl_audio_bridge.h"
#include "../../lib/nrl_net_compat.h"
#include "../../lib/nrl_version.h"
#include "../../lib/wifi_config_portal.h"
#include "../../services/music_player.h"
#include "../../services/signaling_service.h"
#include "../../services/time_sync_service.h"

#include <driver/gpio.h>
#include <esp_cache.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_lcd_touch_cst9220.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <nvs.h>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Generated CJK bitmap fonts (GB2312 level-1), compiled for this board from
// src/app/driver/fonts/. Weak references keep this file self-contained.
extern "C" {
extern const lv_font_t lv_font_cjk_16 __attribute__((weak));
extern const lv_font_t lv_font_cjk_20 __attribute__((weak));
}

// Defined in display_mosaico_panel.cpp; tracks Display_Init progress for the
// CDC-console status dump.
extern "C" volatile int g_mosaico_boot_stage;

namespace {

const char *kTag = "MOSAICO_DISP";

constexpr int kWidth = NRL_DISPLAY_WIDTH;
constexpr int kHeight = NRL_DISPLAY_HEIGHT;
// Draw-buffer height in lines (two buffers, PSRAM): 480x60 RGB565 x2 = 112 KB.
constexpr int kDrawBufLines = 60;

// ---- AMOLED Dark theme -----------------------------------------------------
// Pure-black background (AMOLED pixels emit directly: black is free and deep);
// vibrant accents tuned for the high-contrast panel.
constexpr uint32_t kColorBg       = 0x000000;  // pure black
constexpr uint32_t kColorCard     = 0x12151B;  // rounded card fill
constexpr uint32_t kColorBorder   = 0x232A35;
constexpr uint32_t kColorAccent   = 0x22D3EE;  // electric cyan (primary)
constexpr uint32_t kColorViolet   = 0xA78BFA;  // secondary
constexpr uint32_t kColorText     = 0xF2F6FA;
constexpr uint32_t kColorSub      = 0x8A97A8;
constexpr uint32_t kColorGood     = 0x4ADE80;
constexpr uint32_t kColorWarn     = 0xF5B453;
constexpr uint32_t kColorBad      = 0xF87171;
constexpr uint32_t kColorTabOnBg  = 0x0E2A33;  // active-tab cyan tint
constexpr uint32_t kColorPttTxBg  = 0x2A0F12;  // PTT transmitting fill
constexpr uint32_t kColorBtnPress = 0x1B2A33;

// ---- Layout grid (8 px grid, 16 px outer margins) --------------------------
constexpr int kMargin      = 16;
constexpr int kBarH        = 48;   // top status bar
constexpr int kDockH       = 64;   // bottom nav dock
constexpr int kContentY    = 56;
constexpr int kContentH    = 352;  // y 56..408
constexpr int kCardW       = (kWidth - 2 * kMargin - 8) / 2;  // 220

constexpr uint32_t kBarRefreshMs     = 500u;
constexpr uint32_t kSensorsRefreshMs = 250u;  // ~4 Hz live sensors
constexpr uint32_t kVolumeSaveDelayMs = 2000u;

esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
lv_display_t *s_disp = nullptr;
lv_indev_t *s_touch_indev = nullptr;
bool s_ready = false;
bool s_provisioning_mode = false;
int s_rotation = 0;  // applied panel rotation: 0/90/180/270 (MADCTL)

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

uint32_t lvglTick()
{
    return millis();
}

void lvglFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel =
        static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    // fullWidthArea (below) forces every dirty area to span the whole 480 px
    // row: a full-width strip is 960 bytes = 15x64, so LVGL's
    // LV_DRAW_BUF_STRIDE_ALIGN(64) row padding never kicks in and px_map is
    // always tightly packed. Narrow-area flushes were the source of every
    // artifact seen on this panel (sheared rows, bright dots, violet lines).
    if (w != kWidth) {
        ESP_LOGW(kTag, "unexpected narrow flush %ldx%ld @(%ld,%ld)",
                 static_cast<long>(w), static_cast<long>(h),
                 static_cast<long>(area->x1), static_cast<long>(area->y1));
    }
    // LVGL's software draw writes the PSRAM buffer through the CPU cache;
    // the byte swap below then dirties it again. Clean+invalidate so the swap
    // reads exactly what was rendered.
    esp_cache_msync(px_map, static_cast<uint32_t>(w) * 2u * static_cast<uint32_t>(h),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                        ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    // LVGL RGB565 is little-endian in memory; the CO5300 wants big-endian.
    lv_draw_sw_rgb565_swap(px_map, w * h);
    // The QSPI DMA reads PSRAM directly (psram_dma_direct): clean the swapped
    // data out of the cache or the panel shows stale-memory pixels.
    esp_cache_msync(px_map, static_cast<uint32_t>(w) * 2u * static_cast<uint32_t>(h),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1,
                              area->y2 + 1, px_map);
    // Block until the SPI DMA actually finished streaming this buffer before
    // telling LVGL it may reuse it.
    MosaicoPanel_WaitFlushDone(200);
    lv_display_flush_ready(disp);
}

// Force every invalidated area to a full-width strip. The CO5300 QSPI path is
// only reliable for full-row-width transfers here (narrow windows showed
// stride/shear/cache artifacts no matter how they were packed), and a
// 480x~60 strip re-render costs only a few ms.
void fullWidthArea(lv_event_t *e)
{
    lv_area_t *a = static_cast<lv_area_t *>(lv_event_get_param(e));
    if (a == nullptr) {
        return;
    }
    a->x1 = 0;
    a->x2 = kWidth - 1;
}

bool initLvgl()
{
    lv_init();
    g_mosaico_boot_stage = 71;

    s_disp = lv_display_create(kWidth, kHeight);
    if (s_disp == nullptr) {
        ESP_LOGE(kTag, "display create failed");
        return false;
    }
    g_mosaico_boot_stage = 72;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_flush_cb(s_disp, lvglFlush);

    const size_t buf_bytes = static_cast<size_t>(kWidth) * kDrawBufLines * 2u;
    // LVGL asserts LV_DRAW_BUF_ALIGN(=64)-aligned buffers; plain
    // heap_caps_malloc only guarantees 8, which trips LV_ASSERT_FORMAT_MSG in
    // lv_display_set_buffers (whose default handler is `while(1)` -- an
    // infinite hang, seen on hardware as a black screen with a live app).
    void *buf0 = heap_caps_aligned_alloc(64, buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *buf1 = heap_caps_aligned_alloc(64, buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf0 == nullptr || buf1 == nullptr) {
        ESP_LOGE(kTag, "draw buffer alloc failed");
        return false;
    }
    g_mosaico_boot_stage = 73;
    lv_display_set_buffers(s_disp, buf0, buf1, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(s_disp, fullWidthArea, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_tick_set_cb(lvglTick);
    g_mosaico_boot_stage = 74;
    return true;
}

// Touch press start point (logical coords), recorded on the press edge and
// kept after release so the gesture handler can tell an edge swipe-in from
// an in-page scroll.
int s_touch_start_x = -1;
int s_touch_start_y = -1;
bool s_touch_down = false;

void touchRead(lv_indev_t *, lv_indev_data_t *data)
{
    if (s_touch == nullptr || data == nullptr) {
        return;
    }
    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t count = 0;
    esp_lcd_touch_read_data(s_touch);
    if (esp_lcd_touch_get_data(s_touch, points, &count, 1) == ESP_OK && count > 0) {
        // The touch controller does not follow the panel's MADCTL rotation;
        // remap native coordinates into the rotated logical frame. With
        // swap_xy+mirror_x (rot 90) the panel shows logical (x,y) at physical
        // (W-1-y, x), so the inverse map is x=ty, y=W-1-tx; rot 270 is the
        // mirror image. (The two landscape cases must not be swapped --
        // 0/180 are self-symmetric and unaffected.)
        const int tx = static_cast<int>(points[0].x);
        const int ty = static_cast<int>(points[0].y);
        int x = tx;
        int y = ty;
        switch (s_rotation) {
            case 90:  x = ty;               y = kWidth - 1 - tx; break;
            case 180: x = kWidth - 1 - tx;  y = kHeight - 1 - ty; break;
            case 270: x = kHeight - 1 - ty; y = tx; break;
            default: break;
        }
        data->point.x = static_cast<int16_t>(x);
        data->point.y = static_cast<int16_t>(y);
        data->state = LV_INDEV_STATE_PRESSED;
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start_x = x;
            s_touch_start_y = y;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_touch_down = false;
    }
}

bool initTouch()
{
    i2c_master_bus_handle_t bus = nullptr;
    if (!I2C_MasterGetBus(&bus)) {
        ESP_LOGW(kTag, "touch I2C unavailable");
        return false;
    }

    // NOTE: the CST9220 reset shares GPIO42 with LCD_RST and was already
    // pulsed by the panel reset; the panel init sequence (>100 ms) doubles
    // as the touch controller's post-reset wait.
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST9220_ADDRESS;
    io_cfg.scl_speed_hz = 400000;
    // Same fields as ESP_LCD_TOUCH_IO_I2C_CST9220_CONFIG(), assigned one by one
    // (the macro's C designated initializers don't compile under C++ here).
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset = 0;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.disable_control_phase = 1;
    // Left at 0 a failed transfer waits forever and hangs Display_Init (the
    // official BSP sets BSP_LCD_TOUCH_I2C_TIMEOUT_MS=100 for the same reason).
    io_cfg.transaction_timeout_ms = 100;
    esp_lcd_panel_io_handle_t touch_io = nullptr;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &touch_io) != ESP_OK) {
        ESP_LOGW(kTag, "touch IO create failed");
        return false;
    }

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max = kWidth;
    touch_cfg.y_max = kHeight;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC;
    // Poll the controller on every read instead of gating on the INT pin:
    // the CST9220's INT is a per-report pulse, so INT-gated reads mostly get
    // skipped and LVGL sees the touch point frozen at the initial contact --
    // which kills all drag detection (page swipe gestures, sliders, scroll).
    touch_cfg.int_gpio_num = GPIO_NUM_NC;
    touch_cfg.levels.reset = 0;
    touch_cfg.levels.interrupt = 0;
    if (esp_lcd_touch_new_i2c_cst9220(touch_io, &touch_cfg, &s_touch) != ESP_OK) {
        ESP_LOGW(kTag, "CST9220 create failed");
        return false;
    }

    s_touch_indev = lv_indev_create();
    if (s_touch_indev == nullptr) {
        return false;
    }
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, s_disp);
    lv_indev_set_read_cb(s_touch_indev, touchRead);
    ESP_LOGI(kTag, "CST9220 touch ready");
    return true;
}

// ---- UI language (i18n) -----------------------------------------------------
// Compact equivalent of the Korvo tr(): English source strings, Chinese lookup
// when 中文 is active, passthrough for anything untranslated. Persisted in NVS
// ("ui"/"lang"); switching rebuilds the UI.

int s_lang = 0; // 0 = English, 1 = 中文

struct TrEntry {
    const char *en;
    const char *zh;
};

const TrEntry kTr[] = {
    // Tabs
    {"Home", "主页"},
    {"Music", "音乐"},
    {"Sensors", "传感器"},
    {"Settings", "设置"},
    // Status / common
    {"Linked", "已连接"},
    {"Offline", "离线"},
    {"Charging", "充电中"},
    {"AP mode", "热点模式"},
    {"Close", "关闭"},
    // Home cards
    {"SERVER", "服务器"},
    {"DEVICE IP", "设备 IP"},
    {"WIFI", "无线网络"},
    {"BATTERY", "电池"},
    {"HEADING", "航向"},
    {"No gauge", "无电量计"},
    // PTT bar
    {"HOLD TO TALK", "按住发射"},
    {"TRANSMITTING", "发射中"},
    // Music page
    {"NET RADIO", "网络电台"},
    {"Play", "播放"},
    {"Stop", "停止"},
    {"Playing", "播放中"},
    {"Stopped", "已停止"},
    {"No station", "无电台"},
    {"Set a station URL in the web portal first.", "请先在 Web 配置页设置电台地址。"},
    {"Tuning in...", "正在连接电台..."},
    {"Play failed.", "播放失败。"},
    {"Volume %d%%", "音量 %d%%"},
    {"Local library requires NAND storage (coming soon)",
     "本地曲库需要 NAND 存储（即将支持）"},
    // Sensors page
    {"ACCEL (g)", "加速度 (g)"},
    {"GYRO (dps)", "陀螺仪 (dps)"},
    {"MAGNETOMETER (uT)", "磁力计 (uT)"},
    {"Interference", "磁场干扰"},
    {"sensor absent", "传感器缺席"},
    // Settings page
    {"BRIGHTNESS", "亮度"},
    {"MIC VOLUME", "麦克风音量"},
    {"SPEAKER VOLUME", "扬声器音量"},
    {"Language", "语言"},
    {"Auto-rotate", "自动旋转"},
    {"Rotation", "屏幕方向"},
    {"Vibration", "震动反馈"},
    {"WiFi Setup", "WiFi 设置"},
    {"Hotspot Info", "热点信息"},
    {"ABOUT", "关于"},
    {"Board", "板卡"},
    {"Firmware", "固件"},
    // Provisioning overlay / screen
    {"WiFi Provisioning", "WiFi 配网"},
    {"1. Connect phone/PC to hotspot:", "1. 手机/电脑连接热点:"},
    {"2. Open in a browser:", "2. 浏览器打开:"},
    {"Or use WeChat mini program「NRL互联」via Bluetooth.",
     "或通过微信小程序「NRL互联」蓝牙配网。"},
};

const char *tr(const char *text)
{
    if (s_lang == 0 || text == nullptr) {
        return text;
    }
    for (size_t i = 0; i < sizeof(kTr) / sizeof(kTr[0]); ++i) {
        if (strcmp(kTr[i].en, text) == 0) {
            return kTr[i].zh;
        }
    }
    return text;
}

const char *weekdayName(int wday)
{
    static const char *kEn[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *kZh[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (wday < 0 || wday > 6) {
        wday = 0;
    }
    return (s_lang == 1) ? kZh[wday] : kEn[wday];
}

// 8-point compass label from a 0-360 heading.
const char *compassPoint(float deg)
{
    static const char *kEn[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    static const char *kZh[8] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
    while (deg < 0.0f) {
        deg += 360.0f;
    }
    const int idx = (static_cast<int>(deg + 22.5f) / 45) & 7;
    return (s_lang == 1) ? kZh[idx] : kEn[idx];
}

void loadUiLang()
{
    nvs_handle_t nvs;
    if (nvs_open("ui", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t lang = 0;
    if (nvs_get_u8(nvs, "lang", &lang) == ESP_OK && lang <= 1u) {
        s_lang = lang;
    }
    nvs_close(nvs);
}

void setUiLang(const int lang)
{
    s_lang = (lang != 0) ? 1 : 0;
    nvs_handle_t nvs;
    if (nvs_open("ui", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "lang", static_cast<uint8_t>(s_lang));
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// ---- Brightness (CO5300 0x51, persisted in NVS "mosaico"/"bright") ---------

uint8_t s_brightness = 255u;

void applyBrightness()
{
    (void)MosaicoPanel_SetBrightness(s_brightness);
}

void loadBrightness()
{
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(nvs, "bright", &value) == ESP_OK) {
            s_brightness = value;
        }
        nvs_close(nvs);
    }
    applyBrightness();
}

void saveBrightness()
{
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "bright", s_brightness);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// ---- Auto-rotate (BMI270 gravity -> CO5300 MADCTL hardware rotation) --------
// The panel is square, so LVGL geometry never changes; the panel's MADCTL
// does the rotation and touch coordinates are remapped in touchRead().
// Persisted in NVS "mosaico"/"autorot" (default on).

bool sensorSnapshot(MosaicoSensorSnapshot *out);  // defined below

bool s_auto_rotate = true;
int s_rot_candidate = 0;
uint32_t s_rot_candidate_ms = 0;
uint32_t s_last_rotate_check_ms = 0;

constexpr uint32_t kRotateCheckMs = 300u;
constexpr uint32_t kRotateHoldMs = 500u;   // candidate must persist this long
constexpr float kFlatG2 = 0.64f;           // az^2 above this: lying flat, keep

void applyRotation(int rot)
{
    if (s_panel == nullptr || rot == s_rotation) {
        return;
    }
    esp_lcd_panel_swap_xy(s_panel, rot == 90 || rot == 270);
    esp_lcd_panel_mirror(s_panel, rot == 90 || rot == 180, rot == 180 || rot == 270);
    s_rotation = rot;
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(kTag, "rotation %d", rot);
}

void loadAutoRotate()
{
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value = 1;
        if (nvs_get_u8(nvs, "autorot", &value) == ESP_OK) {
            s_auto_rotate = value != 0u;
        }
        nvs_close(nvs);
    }
}

void saveAutoRotate()
{
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "autorot", s_auto_rotate ? 1u : 0u);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void setAutoRotate(bool enabled)
{
    s_auto_rotate = enabled;
    saveAutoRotate();
    if (!enabled) {
        applyRotation(0);  // predictable upright UI when the feature is off
    }
}

// ---- Manual rotation (NVS "mosaico"/"rotation", cycles 0/90/180/270) ---------
// The Settings button picks the orientation directly; when auto-rotate is on
// the IMU may override it later, when off this is the only way to rotate.

void saveRotation()
{
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "rotation", static_cast<uint8_t>(s_rotation / 90));
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void loadRotation()
{
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(nvs, "rotation", &value) == ESP_OK && value <= 3u) {
            applyRotation(static_cast<int>(value) * 90);
        }
        nvs_close(nvs);
    }
}

// ---- Haptic feedback (button vibration, NVS "mosaico"/"haptic", default on) --

bool s_haptic = true;

void loadHaptic()
{
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t value = 1;
        if (nvs_get_u8(nvs, "haptic", &value) == ESP_OK) {
            s_haptic = value != 0u;
        }
        nvs_close(nvs);
    }
    STATUS_IO_SetHapticEnabled(s_haptic);
}

void setHaptic(bool enabled)
{
    s_haptic = enabled;
    STATUS_IO_SetHapticEnabled(enabled);
    nvs_handle_t nvs;
    if (nvs_open("mosaico", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "haptic", enabled ? 1u : 0u);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (enabled) {
        STATUS_IO_Vibrate(30);  // feel the switch turning back on
    }
}

// NOTE: accel axis signs assume the BMI270's PCB orientation; if rotation
// lands 90/270-swapped or 180-flipped on real hardware, fix the mapping here.
void pollAutoRotate(uint32_t now)
{
    if (!s_auto_rotate) {
        return;
    }
    if (s_last_rotate_check_ms != 0u && (now - s_last_rotate_check_ms) < kRotateCheckMs) {
        return;
    }
    s_last_rotate_check_ms = now;

    MosaicoSensorSnapshot snap = {};
    if (!sensorSnapshot(&snap) || !snap.imu_valid) {
        return;
    }
    const float ax = snap.accel_x_g;
    const float ay = snap.accel_y_g;
    const float az = snap.accel_z_g;
    if (az * az > kFlatG2) {
        return;  // lying flat on a table: keep the current orientation
    }
    int candidate;
    if (fabsf(ay) >= fabsf(ax)) {
        candidate = (ay > 0.0f) ? 0 : 180;
    } else {
        candidate = (ax > 0.0f) ? 90 : 270;
    }
    if (candidate == s_rotation) {
        s_rot_candidate = candidate;
        return;
    }
    if (candidate != s_rot_candidate) {
        s_rot_candidate = candidate;
        s_rot_candidate_ms = now;
        return;
    }
    if ((now - s_rot_candidate_ms) >= kRotateHoldMs) {
        applyRotation(candidate);
    }
}

// ---- Fonts (Montserrat primary + optional CJK fallback) ---------------------

lv_font_t s_font_ui_14;
lv_font_t s_font_ui_16;
lv_font_t s_font_ui_20;
lv_font_t s_font_ui_28;

void initFonts()
{
    s_font_ui_14 = lv_font_montserrat_14;
    s_font_ui_16 = lv_font_montserrat_16;
    s_font_ui_20 = lv_font_montserrat_20;
    s_font_ui_28 = lv_font_montserrat_28;
    if (&lv_font_cjk_16 != nullptr) {
        s_font_ui_14.fallback = &lv_font_cjk_16;
        s_font_ui_16.fallback = &lv_font_cjk_16;
    }
    if (&lv_font_cjk_20 != nullptr) {
        s_font_ui_20.fallback = &lv_font_cjk_20;
        // No 28px CJK bitmap; fall back to the 20px glyphs for titles.
        s_font_ui_28.fallback = &lv_font_cjk_20;
    }
}

// ---- UI state ---------------------------------------------------------------

enum class Page : int {
    Home = 0,
    Music,
    Sensors,
    Settings,
    Count,
};

Page s_page = Page::Home;
bool s_time_sync_started = false;
uint32_t s_last_bar_ms = 0u;
uint32_t s_last_page_ms = 0u;
bool s_volume_dirty = false;
uint32_t s_volume_change_ms = 0u;

// Persistent chrome.
lv_obj_t *s_content = nullptr;
lv_obj_t *s_tab_btns[static_cast<int>(Page::Count)] = {};
lv_obj_t *s_tab_labels[static_cast<int>(Page::Count)] = {};

// Status-bar labels.
lv_obj_t *s_lbl_clock = nullptr;
lv_obj_t *s_lbl_callsign_top = nullptr;
lv_obj_t *s_lbl_wifi = nullptr;
lv_obj_t *s_lbl_link = nullptr;
lv_obj_t *s_lbl_vol = nullptr;
lv_obj_t *s_lbl_batt = nullptr;

// Change-detection caches (keep Display_Poll cheap: no redraw when unchanged).
char s_shown_clock[16] = {};
char s_shown_callsign[16] = {};
char s_shown_wifi[16] = {};
char s_shown_vol[12] = {};
char s_shown_batt[16] = {};
char s_shown_home_clock[16] = {};

// Home page.
lv_obj_t *s_home_clock = nullptr;
lv_obj_t *s_home_date = nullptr;
lv_obj_t *s_home_callsign = nullptr;
lv_obj_t *s_home_rx_codec = nullptr;
lv_obj_t *s_home_ptt_hint = nullptr;
lv_obj_t *s_home_net_server = nullptr;
lv_obj_t *s_home_net_ip = nullptr;
bool s_home_rx_shown = false;
char s_shown_home_sig[48] = {};

// PTT surface (the Home hero card itself is the press-and-hold area).
lv_obj_t *s_ptt_btn = nullptr;
bool s_ptt_tx_visual = false;

// Music page.
lv_obj_t *s_music_track = nullptr;
lv_obj_t *s_music_state = nullptr;
lv_obj_t *s_music_url = nullptr;
lv_obj_t *s_music_play_label = nullptr;
lv_obj_t *s_music_vol = nullptr;

// Sensors page.
lv_obj_t *s_sens_accel = nullptr;
lv_obj_t *s_sens_gyro = nullptr;
lv_obj_t *s_sens_heading = nullptr;
lv_obj_t *s_sens_heading_sub = nullptr;
lv_obj_t *s_sens_batt = nullptr;
lv_obj_t *s_sens_mag = nullptr;
lv_obj_t *s_sens_mag_warn = nullptr;

// Settings page.
lv_obj_t *s_settings_bright_label = nullptr;
lv_obj_t *s_settings_mic_label = nullptr;
lv_obj_t *s_settings_spk_label = nullptr;
lv_obj_t *s_settings_rot_btn_label = nullptr;
lv_obj_t *s_settings_lang_btn_label = nullptr;

// Provisioning screen / overlay.
lv_obj_t *s_prov_ssid = nullptr;
lv_obj_t *s_prov_ip = nullptr;
lv_obj_t *s_overlay = nullptr;

// ---- Widget helpers ---------------------------------------------------------

lv_obj_t *makeCard(lv_obj_t *parent, int x, int y, int w, int h, int pad = 16)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Let left/right swipes on cards reach the content container.
    lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return card;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    uint32_t color)
{
    // Route Montserrat 14/16/20/28 through their CJK-fallback twins so
    // translated text renders; pure-ASCII output is unaffected.
    if (font == &lv_font_montserrat_14) {
        font = &s_font_ui_14;
    } else if (font == &lv_font_montserrat_16) {
        font = &s_font_ui_16;
    } else if (font == &lv_font_montserrat_20) {
        font = &s_font_ui_20;
    } else if (font == &lv_font_montserrat_28) {
        font = &s_font_ui_28;
    }
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

// Set a label only when the text changed (cheap Poll: no needless redraws).
// Returns true when the label was updated.
bool setLabel(lv_obj_t *label, char *cache, size_t cache_size, const char *text)
{
    if (label == nullptr) {
        return false;
    }
    if (cache != nullptr && strncmp(cache, text, cache_size) == 0) {
        return false;
    }
    lv_label_set_text(label, text);
    if (cache != nullptr) {
        size_t n = strlen(text);
        if (n >= cache_size) {
            n = cache_size - 1;
        }
        memcpy(cache, text, n);
        cache[n] = '\0';
    }
    return true;
}

lv_obj_t *makeButton(lv_obj_t *parent, int x, int y, int w, int h,
                     const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    // LVGL objects default to scrollable=1; a scrollable ancestor suppresses
    // all gesture detection, so strip it from every widget we create.
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtnPress), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    if (cb != nullptr) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *txt = makeLabel(btn, text, &lv_font_montserrat_20, kColorText);
    lv_obj_center(txt);
    return btn;
}

lv_obj_t *cardCaption(lv_obj_t *card, const char *text)
{
    lv_obj_t *caption = makeLabel(card, tr(text), &lv_font_montserrat_14, kColorSub);
    lv_obj_set_pos(caption, 0, 0);
    return caption;
}

// ---- Service queries --------------------------------------------------------

void formatCallsign(char *out, size_t out_size)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr || cfg->callsign[0] == '\0') {
        snprintf(out, out_size, "----------");
        return;
    }
    snprintf(out, out_size, "%s-%u", cfg->callsign,
             static_cast<unsigned>(cfg->callsign_ssid));
}

// ---- Remote caller (incoming NRL voice) --------------------------------------

// True while remote voice is playing; out gets "CALL-SSID".
bool remoteCaller(char *out, size_t out_size)
{
    char voice_call[12] = {};
    unsigned voice_ssid = 0;
    if (!NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid) ||
        voice_call[0] == '\0') {
        return false;
    }
    snprintf(out, out_size, "%s-%u", voice_call, voice_ssid);
    return true;
}

const char *rxCodecName()
{
    return (NRLAudioBridge_GetRxCodec() == 1u) ? "OPUS" : "G.711";
}

// Decoded-signaling line (DMR ID + last MDC/CTCSS/DTMF result), empty when the
// decoders produced nothing.
void signalingInfo(char *out, size_t out_size)
{
    char sig[32] = {};
    SIGNALING_GetLastResult(sig, sizeof(sig));
    const uint32_t dmr_id = NRLAudioBridge_GetRemoteDmrId();
    if (dmr_id != 0u) {
        snprintf(out, out_size, "DMRID %lu%s%s",
                 static_cast<unsigned long>(dmr_id),
                 sig[0] != '\0' ? " · " : "", sig);
    } else {
        snprintf(out, out_size, "%s", sig);
    }
}

void formatClock(char *out, size_t out_size, struct tm *out_tm)
{
    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    if (out_tm != nullptr) {
        *out_tm = tm_now;
    }
    if (tm_now.tm_year + 1900 >= 2024) {
        snprintf(out, out_size, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    } else {
        snprintf(out, out_size, "--:--");
    }
}

bool sensorSnapshot(MosaicoSensorSnapshot *out)
{
    return MOSAICO_SENSORS_GetSnapshot(out);
}

int batteryMvRaw()
{
    MosaicoSensorSnapshot snap = {};
    if (!sensorSnapshot(&snap) || !snap.gauge_present) {
        return 0;
    }
    return static_cast<int>(snap.battery_mv);
}

// ---- Status bar -------------------------------------------------------------

void switchTab(int index);
// Shared volume stepper (music page buttons + edge swipe-in gesture).
void adjustVolumePct(int delta);

// Swipe handling. Gestures always land on the SCREEN: every LVGL child gets
// gesture_bubble=1 by default, so LV_EVENT_GESTURE bubbles all the way up to
// scr no matter where the finger started. One handler covers both the
// horizontal page swipe and the vertical edge swipe-in volume control.
void screenGestureEvent(lv_event_t *)
{
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        const int current = static_cast<int>(s_page);
        if (dir == LV_DIR_LEFT) {
            switchTab((current + 1) % static_cast<int>(Page::Count));
        } else {
            switchTab((current + static_cast<int>(Page::Count) - 1) %
                      static_cast<int>(Page::Count));
        }
        return;
    }
    if (dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) {
        return;
    }
    // Vertical swipe that STARTED in the status bar or the dock: into the
    // screen = volume up, back out = volume down.
    const bool from_top = s_touch_start_y >= 0 && s_touch_start_y < kContentY;
    const bool from_bottom = s_touch_start_y >= kHeight - kDockH;
    if (!from_top && !from_bottom) {
        return;
    }
    const bool inward = (from_top && dir == LV_DIR_BOTTOM) ||
                        (from_bottom && dir == LV_DIR_TOP);
    adjustVolumePct(inward ? 5 : -5);
    STATUS_IO_Vibrate(15);
}

void adjustVolumePct(int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int pct = (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255 + delta;
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    const int volume = (pct * 255 + 50) / 100;
    if (volume != static_cast<int>(cfg->line_out_volume)) {
        EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
        s_volume_dirty = true;
        s_volume_change_ms = millis();
    }
}

void buildStatusBar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, kWidth, kBarH);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Fixed, non-overlapping slots (480 px bar):
    //   clock 34..104 | callsign 112..252 | wifi 256..312 | link ~320..336
    //   | vol 344..388 | batt 396..452
    s_lbl_clock = makeLabel(bar, "--:--", &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_lbl_clock, 70);
    lv_obj_set_style_text_align(s_lbl_clock, LV_TEXT_ALIGN_LEFT, 0);
    // Rounded-corner panel: keep corner content out of the clipped edge.
    lv_obj_align(s_lbl_clock, LV_ALIGN_LEFT_MID, 34, 0);

    s_lbl_callsign_top = makeLabel(bar, "----------", &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_width(s_lbl_callsign_top, 140);
    lv_obj_set_style_text_align(s_lbl_callsign_top, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_lbl_callsign_top, LV_LABEL_LONG_DOT);
    lv_obj_align(s_lbl_callsign_top, LV_ALIGN_LEFT_MID, 112, 0);

    s_lbl_wifi = makeLabel(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_wifi, 56);
    lv_obj_set_style_text_align(s_lbl_wifi, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_RIGHT_MID, -168, 0);

    s_lbl_link = makeLabel(bar, "\xE2\x97\x8F", &lv_font_montserrat_16, kColorSub); // ●
    lv_obj_align(s_lbl_link, LV_ALIGN_RIGHT_MID, -144, 0);

    // Speaker volume readout (icon + %); the music page and the Settings
    // speaker slider both change it.
    s_lbl_vol = makeLabel(bar, "", &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_vol, 44);
    lv_obj_set_style_text_align(s_lbl_vol, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_lbl_vol, LV_ALIGN_RIGHT_MID, -92, 0);

    s_lbl_batt = makeLabel(bar, "--", &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_batt, 56);
    lv_obj_set_style_text_align(s_lbl_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_lbl_batt, LV_ALIGN_RIGHT_MID, -28, 0);
}

void refreshStatusBar()
{
    if (s_lbl_clock == nullptr) {
        return;
    }
    if (!s_time_sync_started && nrlWifiStaConnected()) {
        (void)TIME_SYNC_StartIfNeeded();
        s_time_sync_started = true;
    }

    char text[24];
    formatClock(text, sizeof(text), nullptr);
    setLabel(s_lbl_clock, s_shown_clock, sizeof(s_shown_clock), text);

    char callsign[16];
    formatCallsign(callsign, sizeof(callsign));
    setLabel(s_lbl_callsign_top, s_shown_callsign, sizeof(s_shown_callsign), callsign);

    // WiFi glyph + RSSI dBm, colored by signal (amber in AP/config mode).
    uint32_t wifi_color;
    char wifi_text[12];
    if (nrlWifiStaConnected()) {
        wifi_ap_record_t ap = {};
        const int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        snprintf(wifi_text, sizeof(wifi_text), LV_SYMBOL_WIFI "%d", rssi);
        wifi_color = (rssi >= -65) ? kColorGood : ((rssi >= -78) ? kColorWarn : kColorBad);
    } else {
        snprintf(wifi_text, sizeof(wifi_text), LV_SYMBOL_WIFI "AP");
        wifi_color = kColorWarn;
    }
    if (setLabel(s_lbl_wifi, s_shown_wifi, sizeof(s_shown_wifi), wifi_text)) {
        lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(wifi_color), 0);
    }

    const bool linked = STATUS_IO_NrlServerLinked();
    lv_obj_set_style_text_color(s_lbl_link,
                                lv_color_hex(linked ? kColorGood : kColorSub), 0);

    // Speaker volume: mute glyph at 0, level glyph otherwise.
    const ExternalRadioConfig *vcfg = EXTERNAL_RADIO_GetConfig();
    const int vol_pct = (vcfg != nullptr)
                            ? (static_cast<int>(vcfg->line_out_volume) * 100 + 127) / 255
                            : 0;
    char vol_text[12];
    snprintf(vol_text, sizeof(vol_text), "%s%d",
             vol_pct == 0 ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX, vol_pct);
    setLabel(s_lbl_vol, s_shown_vol, sizeof(s_shown_vol), vol_text);

    MosaicoSensorSnapshot snap = {};
    char batt[16];
    if (sensorSnapshot(&snap) && snap.gauge_present) {
        snprintf(batt, sizeof(batt), "%s%u%%",
                 snap.battery_charging ? LV_SYMBOL_CHARGE : "",
                 static_cast<unsigned>(snap.battery_soc_percent));
    } else {
        snprintf(batt, sizeof(batt), "--");
    }
    setLabel(s_lbl_batt, s_shown_batt, sizeof(s_shown_batt), batt);
}

// ---- Bottom nav dock --------------------------------------------------------

// Floating pill dock, inset from the panel's rounded corners so the first and
// last tabs are never clipped by the curved edge.
constexpr int kDockInset = 24;
constexpr int kTabW = 90;
constexpr int kTabGap = 10;
constexpr int kTabCount = static_cast<int>(Page::Count);
constexpr int kTabX0 = (kWidth - 2 * kDockInset -
                        (kTabCount * kTabW + (kTabCount - 1) * kTabGap)) / 2;

void switchTab(int index);

void tabEvent(lv_event_t *event)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(event)));
    switchTab(index);
}

void buildDock(lv_obj_t *scr)
{
    lv_obj_t *dock = lv_obj_create(scr);
    lv_obj_set_pos(dock, kDockInset, kHeight - kDockH + 4);
    lv_obj_set_size(dock, kWidth - 2 * kDockInset, kDockH - 8);
    lv_obj_set_style_bg_color(dock, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_radius(dock, LV_RADIUS_CIRCLE, 0);  // pill
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    const char *icons[kTabCount] = {LV_SYMBOL_HOME, LV_SYMBOL_AUDIO,
                                    LV_SYMBOL_GPS, LV_SYMBOL_SETTINGS};
    const char *names[kTabCount] = {"Home", "Music", "Sensors", "Settings"};
    for (int i = 0; i < kTabCount; ++i) {
        lv_obj_t *btn = lv_button_create(dock);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(btn, kTabX0 + i * (kTabW + kTabGap), 6);
        lv_obj_set_size(btn, kTabW, 44);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, tabEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        char text[48];
        snprintf(text, sizeof(text), "%s\n%s", icons[i], tr(names[i]));
        lv_obj_t *label = makeLabel(btn, text, &lv_font_montserrat_14, kColorSub);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        s_tab_btns[i] = btn;
        s_tab_labels[i] = label;
    }
}

void updateTabHighlight()
{
    const int active = static_cast<int>(s_page);
    for (int i = 0; i < kTabCount; ++i) {
        if (s_tab_btns[i] == nullptr) {
            continue;
        }
        const bool on = (i == active);
        lv_obj_set_style_bg_color(s_tab_btns[i],
                                  lv_color_hex(on ? kColorTabOnBg : kColorBg), 0);
        lv_obj_set_style_bg_opa(s_tab_btns[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_tab_labels[i],
                                    lv_color_hex(on ? kColorAccent : kColorSub), 0);
    }
}

// ---- Page builders (into s_content, content coords: 0..479 x 0..351) --------

void buildHomePage();
void buildMusicPage();
void buildSensorsPage();
void buildSettingsPage();

void buildPage()
{
    if (s_content == nullptr) {
        return;
    }
    s_home_clock = nullptr;
    s_home_date = nullptr;
    s_home_callsign = nullptr;
    s_home_rx_codec = nullptr;
    s_home_ptt_hint = nullptr;
    s_home_net_server = nullptr;
    s_home_net_ip = nullptr;
    // The hero labels are recreated with placeholder text; their
    // change-detection caches must be cleared too, otherwise setLabel()
    // sees "unchanged" and the placeholder stays until the text next
    // changes (the clock could show --:-- for up to a minute).
    s_shown_home_clock[0] = '\0';
    s_shown_home_sig[0] = '\0';
    s_ptt_btn = nullptr;
    s_music_track = nullptr;
    s_music_state = nullptr;
    s_music_url = nullptr;
    s_music_play_label = nullptr;
    s_music_vol = nullptr;
    s_sens_accel = nullptr;
    s_sens_gyro = nullptr;
    s_sens_heading = nullptr;
    s_sens_heading_sub = nullptr;
    s_sens_batt = nullptr;
    s_sens_mag = nullptr;
    s_sens_mag_warn = nullptr;
    s_settings_bright_label = nullptr;
    s_settings_mic_label = nullptr;
    s_settings_spk_label = nullptr;
    s_settings_rot_btn_label = nullptr;
    s_settings_lang_btn_label = nullptr;
    lv_obj_clean(s_content);
    switch (s_page) {
        case Page::Home: buildHomePage(); break;
        case Page::Music: buildMusicPage(); break;
        case Page::Sensors: buildSensorsPage(); break;
        case Page::Settings: buildSettingsPage(); break;
        default: break;
    }
    s_last_page_ms = 0u; // force an immediate live refresh
}

void switchTab(int index)
{
    if (index < 0 || index >= static_cast<int>(Page::Count)) {
        return;
    }
    if (s_overlay != nullptr) {
        lv_obj_delete(s_overlay);
        s_overlay = nullptr;
    }
    const bool changed = (s_page != static_cast<Page>(index));
    s_page = static_cast<Page>(index);
    if (changed) {
        STATUS_IO_Vibrate(30);
        if (s_ptt_tx_visual) {
            // Leaving the Home page mid-transmission releases the soft key.
            STATUS_IO_SetSoftPtt(false);
            s_ptt_tx_visual = false;
        }
    }
    buildPage();
    updateTabHighlight();
}

// ---- Home page ----------------------------------------------------------------

// TX styling on the hero card (which doubles as the PTT surface): red border
// and a "TRANSMITTING" hint while keyed, subtle "HOLD TO TALK" hint when idle.
// Incoming-caller info shares the same card (clock row), so no separate RX
// styling is needed here.
void setHeroPtt(bool tx)
{
    s_ptt_tx_visual = tx;
    if (s_ptt_btn == nullptr) {
        return;
    }
    lv_obj_set_style_border_color(s_ptt_btn,
                                  lv_color_hex(tx ? kColorBad : kColorBorder), 0);
    lv_obj_set_style_border_width(s_ptt_btn, tx ? 3 : 1, 0);
    if (s_home_ptt_hint != nullptr) {
        lv_label_set_text(s_home_ptt_hint, tx ? tr("TRANSMITTING") : tr("HOLD TO TALK"));
        lv_obj_set_style_text_color(s_home_ptt_hint,
                                    lv_color_hex(tx ? kColorBad : kColorSub), 0);
    }
}

void pttEvent(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            STATUS_IO_SetSoftPtt(true);
            STATUS_IO_Vibrate(30);
            setHeroPtt(true);
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            STATUS_IO_SetSoftPtt(false);
            setHeroPtt(false);
            break;
        default:
            break;
    }
}

void buildHomePage()
{
    // The hero card IS the PTT button: press-and-hold anywhere on it to
    // transmit. Bigger card, bigger targets.
    lv_obj_t *hero = lv_button_create(s_content);
    lv_obj_set_pos(hero, kMargin, 8);
    lv_obj_set_size(hero, kWidth - 2 * kMargin, 268);
    lv_obj_set_style_bg_color(hero, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(kColorBtnPress), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(hero, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_radius(hero, 20, 0);
    lv_obj_set_style_pad_all(hero, 12, 0);
    lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    // Horizontal swipes still switch pages; vertical press keys up the radio.
    lv_obj_add_flag(hero, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(hero, pttEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(hero, pttEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(hero, pttEvent, LV_EVENT_PRESS_LOST, nullptr);
    s_ptt_btn = hero;

    s_home_clock = makeLabel(hero, "--:--", &lv_font_montserrat_48, kColorText);
    lv_obj_set_width(s_home_clock, kWidth - 2 * kMargin - 24);
    lv_obj_set_style_text_align(s_home_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_home_clock, LV_LABEL_LONG_DOT);
    lv_obj_align(s_home_clock, LV_ALIGN_CENTER, 0, -64);

    s_home_date = makeLabel(hero, "", &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_home_date, LV_ALIGN_CENTER, 0, -6);

    s_home_callsign = makeLabel(hero, "----------", &lv_font_montserrat_28, kColorAccent);
    lv_obj_align(s_home_callsign, LV_ALIGN_CENTER, 0, 36);

    // RX codec tag ("OPUS"/"G.711"), top-right of the hero; only visible
    // while a caller is on air.
    s_home_rx_codec = makeLabel(hero, "", &lv_font_montserrat_14, kColorAccent);
    lv_obj_align(s_home_rx_codec, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Bottom of the hero: PTT affordance / TX state.
    s_home_ptt_hint = makeLabel(hero, "", &lv_font_montserrat_16, kColorSub);
    lv_obj_set_style_text_align(s_home_ptt_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_home_ptt_hint, LV_ALIGN_CENTER, 0, 104);

    // Merged net card at the bottom: NRL server address | this device's IP.
    lv_obj_t *net = makeCard(s_content, kMargin, 284, kWidth - 2 * kMargin, 68, 10);
    const int half = (kWidth - 2 * kMargin - 20) / 2;  // two columns inside

    lv_obj_t *server_cap = makeLabel(net, tr("SERVER"), &lv_font_montserrat_14, kColorSub);
    lv_obj_set_pos(server_cap, 0, 0);
    s_home_net_server = makeLabel(net, "--", &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_home_net_server, half);
    lv_label_set_long_mode(s_home_net_server, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_home_net_server, 0, 22);

    lv_obj_t *ip_cap = makeLabel(net, tr("DEVICE IP"), &lv_font_montserrat_14, kColorSub);
    lv_obj_set_pos(ip_cap, half + 10, 0);
    s_home_net_ip = makeLabel(net, "--", &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_home_net_ip, half);
    lv_label_set_long_mode(s_home_net_ip, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_home_net_ip, half + 10, 22);

    // STATUS_IO_IsPttActive() is the *inbound* network-audio latch (drives the
    // AUDIO LED); the local-transmit state (physical/soft PTT keyed) is the
    // bridge's PttActive, same source the Korvo radio page uses.
    setHeroPtt(NRLAudioBridge_PttActive());
}

void refreshHomePage()
{
    if (s_home_clock == nullptr) {
        return;
    }
    char text[48];
    struct tm tm_now = {};
    formatClock(text, sizeof(text), &tm_now);  // fills tm_now for the date row

    // Incoming NRL voice takes over the hero: the clock becomes the caller's
    // "CALL-SSID", the codec tag appears at its top-right, and the local
    // callsign row carries the decoded signaling (DMR ID / MDC / CTCSS).
    char remote[16] = {};
    const bool rx = remoteCaller(remote, sizeof(remote));
    if (rx != s_home_rx_shown) {
        s_home_rx_shown = rx;
        s_shown_home_clock[0] = '\0';  // force a text refresh in the new role
        lv_obj_set_style_text_color(s_home_clock,
                                    lv_color_hex(rx ? kColorGood : kColorText), 0);
        lv_obj_set_style_text_color(s_home_callsign,
                                    lv_color_hex(rx ? kColorSub : kColorAccent), 0);
    }
    if (rx) {
        setLabel(s_home_clock, s_shown_home_clock, sizeof(s_shown_home_clock), remote);
        lv_label_set_text(s_home_rx_codec, rxCodecName());
        char info[sizeof(s_shown_home_sig)];
        signalingInfo(info, sizeof(info));
        setLabel(s_home_callsign, s_shown_home_sig, sizeof(s_shown_home_sig), info);
    } else {
        lv_label_set_text(s_home_rx_codec, "");
        setLabel(s_home_clock, s_shown_home_clock, sizeof(s_shown_home_clock), text);
        char callsign[16];
        formatCallsign(callsign, sizeof(callsign));
        setLabel(s_home_callsign, s_shown_home_sig, sizeof(s_shown_home_sig), callsign);
    }

    if (tm_now.tm_year + 1900 >= 2024) {
        snprintf(text, sizeof(text), "%04d-%02d-%02d %s", tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1, tm_now.tm_mday, weekdayName(tm_now.tm_wday));
    } else {
        snprintf(text, sizeof(text), "----");
    }
    lv_label_set_text(s_home_date, text);

    const bool linked = STATUS_IO_NrlServerLinked();
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    lv_label_set_text(s_home_net_server,
                      (cfg != nullptr && cfg->server_host[0] != '\0')
                          ? cfg->server_host
                          : "--");
    lv_obj_set_style_text_color(s_home_net_server,
                                lv_color_hex(linked ? kColorGood : kColorText), 0);

    char ip[20] = {};
    if (nrlWifiStaConnected()) {
        nrlIpToString(nrlWifiStaIp(), ip, sizeof(ip));
    } else {
        nrlIpToString(nrlWifiApIp(), ip, sizeof(ip));
    }
    lv_label_set_text(s_home_net_ip, ip[0] != '\0' ? ip : "--");

    // PTT surface (hero card): red border + hint while transmitting.
    const bool tx = NRLAudioBridge_PttActive();
    if (tx != s_ptt_tx_visual) {
        setHeroPtt(tx);
    }
}

// ---- Music page (net radio; local library needs NAND) ---------------------------

void refreshMusicPage();

void musicPlayEvent(lv_event_t *)
{
    if (MUSIC_IsPlaying()) {
        MUSIC_Stop();
    } else {
        char url[128] = {};
        MUSIC_GetRadioUrl(url, sizeof(url));
        if (url[0] != '\0') {
            (void)MUSIC_PlayFile(url);
        }
    }
    refreshMusicPage();
}

void musicVolumeEvent(lv_event_t *event)
{
    const int delta = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    adjustVolumePct(delta);
    refreshMusicPage();
}

void buildMusicPage()
{
    lv_obj_t *card = makeCard(s_content, kMargin, 8, kWidth - 2 * kMargin, 116, 12);
    cardCaption(card, "NET RADIO");
    s_music_track = makeLabel(card, tr("No station"), &lv_font_montserrat_20, kColorText);
    lv_obj_set_width(s_music_track, kWidth - 2 * kMargin - 24);
    lv_obj_set_pos(s_music_track, 0, 20);
    lv_label_set_long_mode(s_music_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    s_music_state = makeLabel(card, "", &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_music_state, 0, 52);
    s_music_url = makeLabel(card, "", &lv_font_montserrat_14, kColorSub);
    lv_obj_set_width(s_music_url, kWidth - 2 * kMargin - 24);
    lv_label_set_long_mode(s_music_url, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_music_url, 0, 76);

    lv_obj_t *play = makeButton(s_content, kMargin, 140, 216, 64, "", musicPlayEvent, nullptr);
    s_music_play_label = lv_obj_get_child(play, 0);
    makeButton(s_content, 240, 140, 104, 64, LV_SYMBOL_MINUS, musicVolumeEvent,
               reinterpret_cast<void *>(static_cast<intptr_t>(-5)));
    makeButton(s_content, 352, 140, 112, 64, LV_SYMBOL_PLUS, musicVolumeEvent,
               reinterpret_cast<void *>(static_cast<intptr_t>(5)));

    s_music_vol = makeLabel(s_content, "", &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_music_vol, kWidth - 2 * kMargin);
    lv_obj_set_style_text_align(s_music_vol, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_music_vol, kMargin, 216);

    // No SD/USB host on this board: local file playback is unavailable until
    // the NAND storage lands.
    lv_obj_t *note = makeCard(s_content, kMargin, 248, kWidth - 2 * kMargin, 56, 12);
    lv_obj_set_style_bg_color(note, lv_color_hex(0x0D1014), 0);
    lv_obj_t *note_text = makeLabel(note, tr("Local library requires NAND storage (coming soon)"),
                                    &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(note_text, kWidth - 2 * kMargin - 24);
    lv_obj_set_style_text_align(note_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(note_text);

    refreshMusicPage();
}

void refreshMusicPage()
{
    if (s_music_track == nullptr) {
        return;
    }
    const bool playing = MUSIC_IsPlaying();
    const char *path = MUSIC_CurrentPath();
    const MediaTrackInfo *info = MUSIC_GetTrackInfo();

    char text[160];
    if (playing && info != nullptr && info->title[0] != '\0') {
        snprintf(text, sizeof(text), "%s", info->title);
    } else if (playing && path != nullptr && path[0] != '\0') {
        snprintf(text, sizeof(text), "%s", path);
    } else {
        snprintf(text, sizeof(text), "%s", tr("No station"));
    }
    lv_label_set_text(s_music_track, text);

    snprintf(text, sizeof(text), "%s", tr(playing ? "Playing" : "Stopped"));
    lv_label_set_text(s_music_state, text);
    lv_obj_set_style_text_color(s_music_state,
                                lv_color_hex(playing ? kColorGood : kColorSub), 0);

    char url[128] = {};
    MUSIC_GetRadioUrl(url, sizeof(url));
    lv_label_set_text(s_music_url, url);

    snprintf(text, sizeof(text), "%s %s",
             playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY,
             tr(playing ? "Stop" : "Play"));
    lv_label_set_text(s_music_play_label, text);
    lv_obj_set_style_text_color(s_music_play_label,
                                lv_color_hex(playing ? kColorBad : kColorAccent), 0);

    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    const int pct = (cfg != nullptr)
                        ? (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255
                        : 0;
    snprintf(text, sizeof(text), tr("Volume %d%%"), pct);
    lv_label_set_text(s_music_vol, text);
}

// ---- Sensors page (live, ~4 Hz) -------------------------------------------------

void buildSensorsPage()
{
    // Scrollable page body: the magnetometer card sits below the 2x2 grid.
    lv_obj_t *page = lv_obj_create(s_content);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, kWidth, kContentH);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);

    const int row2 = 180;
    const int col2 = kMargin + kCardW + 8;

    lv_obj_t *accel = makeCard(page, kMargin, 8, kCardW, 164, 12);
    cardCaption(accel, "ACCEL (g)");
    s_sens_accel = makeLabel(accel, "--", &lv_font_montserrat_16, kColorText);
    lv_obj_set_pos(s_sens_accel, 0, 24);

    lv_obj_t *gyro = makeCard(page, col2, 8, kCardW, 164, 12);
    cardCaption(gyro, "GYRO (dps)");
    s_sens_gyro = makeLabel(gyro, "--", &lv_font_montserrat_16, kColorText);
    lv_obj_set_pos(s_sens_gyro, 0, 24);

    lv_obj_t *heading = makeCard(page, kMargin, row2, kCardW, 164, 12);
    cardCaption(heading, "HEADING");
    s_sens_heading = makeLabel(heading, "--", &lv_font_montserrat_28, kColorViolet);
    lv_obj_set_pos(s_sens_heading, 0, 28);
    s_sens_heading_sub = makeLabel(heading, "", &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(s_sens_heading_sub, 0, 76);

    lv_obj_t *batt = makeCard(page, col2, row2, kCardW, 164, 12);
    cardCaption(batt, "BATTERY");
    s_sens_batt = makeLabel(batt, "--", &lv_font_montserrat_16, kColorText);
    lv_obj_set_pos(s_sens_batt, 0, 24);

    // Dual BMM150 raw fields. The two sensors sit at different spots on the
    // board, so a large difference means local magnetic interference (speaker
    // magnet, metal) rather than the geomagnetic field.
    lv_obj_t *mag = makeCard(page, kMargin, 352, kWidth - 2 * kMargin, 128, 12);
    cardCaption(mag, "MAGNETOMETER (uT)");
    s_sens_mag = makeLabel(mag, "--", &lv_font_montserrat_16, kColorText);
    lv_obj_set_pos(s_sens_mag, 0, 24);
    s_sens_mag_warn = makeLabel(mag, "", &lv_font_montserrat_16, kColorWarn);
    lv_obj_set_pos(s_sens_mag_warn, 0, 92);
}

void refreshSensorsPage()
{
    if (s_sens_accel == nullptr) {
        return;
    }
    MosaicoSensorSnapshot snap = {};
    const bool ok = sensorSnapshot(&snap);
    char text[128];

    if (ok && snap.imu_valid) {
        snprintf(text, sizeof(text), "X %+.2f\nY %+.2f\nZ %+.2f",
                 static_cast<double>(snap.accel_x_g),
                 static_cast<double>(snap.accel_y_g),
                 static_cast<double>(snap.accel_z_g));
    } else {
        snprintf(text, sizeof(text), "%s", tr("sensor absent"));
    }
    lv_label_set_text(s_sens_accel, text);

    if (ok && snap.imu_valid) {
        snprintf(text, sizeof(text), "X %+.1f\nY %+.1f\nZ %+.1f",
                 static_cast<double>(snap.gyro_x_dps),
                 static_cast<double>(snap.gyro_y_dps),
                 static_cast<double>(snap.gyro_z_dps));
    } else {
        snprintf(text, sizeof(text), "%s", tr("sensor absent"));
    }
    lv_label_set_text(s_sens_gyro, text);

    if (ok && snap.mag2_valid) {
        snprintf(text, sizeof(text), "%.0f\xC2\xB0", static_cast<double>(snap.heading_deg));
        lv_label_set_text(s_sens_heading, text);
        snprintf(text, sizeof(text), "%s%s", compassPoint(snap.heading_deg),
                 snap.mag3_valid ? "  ·2" : "");
        lv_label_set_text(s_sens_heading_sub, text);
    } else {
        lv_label_set_text(s_sens_heading, "--");
        lv_label_set_text(s_sens_heading_sub, tr("sensor absent"));
    }

    if (ok && snap.gauge_present) {
        char charge[24] = {};
        if (snap.battery_charging) {
            snprintf(charge, sizeof(charge), " %s", tr("Charging"));
        }
        snprintf(text, sizeof(text), "%u mV\n%+d mA\n%u%%%s",
                 static_cast<unsigned>(snap.battery_mv),
                 static_cast<int>(snap.battery_current_ma),
                 static_cast<unsigned>(snap.battery_soc_percent), charge);
        lv_label_set_text(s_sens_batt, text);
    } else {
        lv_label_set_text(s_sens_batt, tr("No gauge"));
    }

    if (ok && (snap.mag2_valid || snap.mag3_valid)) {
        snprintf(text, sizeof(text), "#2 X %+.1f Y %+.1f Z %+.1f\n#3 X %+.1f Y %+.1f Z %+.1f",
                 static_cast<double>(snap.mag2_x_ut), static_cast<double>(snap.mag2_y_ut),
                 static_cast<double>(snap.mag2_z_ut),
                 static_cast<double>(snap.mag3_x_ut), static_cast<double>(snap.mag3_y_ut),
                 static_cast<double>(snap.mag3_z_ut));
        lv_label_set_text(s_sens_mag, text);
        // Vector difference between the two sensors -> local interference level.
        const float dx = snap.mag2_x_ut - snap.mag3_x_ut;
        const float dy = snap.mag2_y_ut - snap.mag3_y_ut;
        const float dz = snap.mag2_z_ut - snap.mag3_z_ut;
        const float diff = sqrtf(dx * dx + dy * dy + dz * dz);
        if (snap.mag2_valid && snap.mag3_valid && diff > 15.0f) {
            snprintf(text, sizeof(text), "%s %.0f uT", tr("Interference"),
                     static_cast<double>(diff));
            lv_label_set_text(s_sens_mag_warn, text);
        } else {
            lv_label_set_text(s_sens_mag_warn, "");
        }
    } else {
        lv_label_set_text(s_sens_mag, tr("sensor absent"));
        lv_label_set_text(s_sens_mag_warn, "");
    }
}

// ---- Settings page -----------------------------------------------------------

void brightnessEvent(lv_event_t *event)
{
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        s_brightness = static_cast<uint8_t>(lv_slider_get_value(slider));
        applyBrightness();
        if (s_settings_bright_label != nullptr) {
            char text[8];
            snprintf(text, sizeof(text), "%u", static_cast<unsigned>(s_brightness));
            lv_label_set_text(s_settings_bright_label, text);
        }
    } else if (code == LV_EVENT_RELEASED) {
        saveBrightness();
    }
}

void rebuildMainUi();

// Mic / speaker volume slider (0..100 % <-> 0..255 config). user_data flags
// the mic slider; applies live and persists on release.
void volumeSliderEvent(lv_event_t *event)
{
    const bool is_mic = static_cast<bool>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(event)));
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        const int pct = static_cast<int>(lv_slider_get_value(slider));
        const int volume = (pct * 255 + 50) / 100;
        if (is_mic) {
            EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(volume), false);
        } else {
            EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), false);
        }
        lv_obj_t *label = is_mic ? s_settings_mic_label : s_settings_spk_label;
        if (label != nullptr) {
            char text[8];
            snprintf(text, sizeof(text), "%d", pct);
            lv_label_set_text(label, text);
        }
    } else if (code == LV_EVENT_RELEASED) {
        (void)EXTERNAL_RADIO_SaveConfig();
    }
}

// Slider card identical in geometry to the brightness card; returns the
// value readout label through out_label.
void buildVolumeCard(lv_obj_t *page, int y, const char *caption, int initial_pct,
                     bool is_mic, lv_obj_t **out_label)
{
    lv_obj_t *card = makeCard(page, kMargin, y, kWidth - 2 * kMargin, 92, 12);
    cardCaption(card, caption);
    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 0, 40);
    lv_obj_set_size(slider, kWidth - 2 * kMargin - 24 - 88, 24);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorText), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 12);
    lv_slider_set_value(slider, initial_pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, volumeSliderEvent, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(is_mic)));
    lv_obj_add_event_cb(slider, volumeSliderEvent, LV_EVENT_RELEASED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(is_mic)));

    char text[8];
    snprintf(text, sizeof(text), "%d", initial_pct);
    *out_label = makeLabel(card, text, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_pos(*out_label, kWidth - 2 * kMargin - 24 - 76, 38);
    lv_obj_set_width(*out_label, 76);
    lv_obj_set_style_text_align(*out_label, LV_TEXT_ALIGN_RIGHT, 0);
}

void langEvent(lv_event_t *)
{
    setUiLang(s_lang == 0 ? 1 : 0);
    rebuildMainUi(); // switching the language rebuilds the active page
}

void closeOverlayEvent(lv_event_t *)
{
    if (s_overlay != nullptr) {
        lv_obj_delete(s_overlay);
        s_overlay = nullptr;
    }
}

void showProvisioningOverlay()
{
    if (s_overlay != nullptr) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    s_overlay = makeCard(scr, 24, 96, kWidth - 48, 288, 16);

    lv_obj_t *title = makeLabel(s_overlay, tr("WiFi Provisioning"),
                                &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_width(title, kWidth - 80);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *step1 = makeLabel(s_overlay, tr("1. Connect phone/PC to hotspot:"),
                                &lv_font_montserrat_16, kColorText);
    lv_obj_align(step1, LV_ALIGN_TOP_LEFT, 0, 44);

    char ssid[40] = {};
    WifiConfigPortal_GetApSsid(ssid, sizeof(ssid));
    lv_obj_t *ssid_label = makeLabel(s_overlay, ssid, &lv_font_montserrat_20, kColorGood);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 16, 70);

    lv_obj_t *step2 = makeLabel(s_overlay, tr("2. Open in a browser:"),
                                &lv_font_montserrat_16, kColorText);
    lv_obj_align(step2, LV_ALIGN_TOP_LEFT, 0, 108);

    char ip[24] = "192.168.4.1";
    const uint32_t ap_ip = nrlWifiApIp();
    if (ap_ip != 0u) {
        nrlIpToString(ap_ip, ip, sizeof(ip));
    }
    char url[48];
    snprintf(url, sizeof(url), "http://%s/", ip);
    lv_obj_t *ip_label = makeLabel(s_overlay, url, &lv_font_montserrat_20, kColorGood);
    lv_obj_align(ip_label, LV_ALIGN_TOP_LEFT, 16, 134);

    lv_obj_t *ble = makeLabel(s_overlay, tr("Or use WeChat mini program「NRL互联」via Bluetooth."),
                              &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(ble, kWidth - 80);
    lv_obj_align(ble, LV_ALIGN_TOP_LEFT, 0, 172);

    lv_obj_t *close = makeButton(s_overlay, (kWidth - 48 - 32 - 140) / 2, 214, 140, 52,
                                 tr("Close"), closeOverlayEvent, nullptr);
    (void)close;
}

void provisioningInfoEvent(lv_event_t *)
{
    showProvisioningOverlay();
}

void autoRotateEvent(lv_event_t *event)
{
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    setAutoRotate(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

void hapticEvent(lv_event_t *event)
{
    lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(event));
    setHaptic(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

void rotationEvent(lv_event_t *)
{
    const int rot = (s_rotation + 90) % 360;
    applyRotation(rot);
    saveRotation();
    if (s_settings_rot_btn_label != nullptr) {
        char text[8];
        snprintf(text, sizeof(text), "%d\xC2\xB0", rot);  // "90°" etc.
        lv_label_set_text(s_settings_rot_btn_label, text);
    }
}

void buildSettingsPage()
{
    // Scrollable page body: the cards below exceed the 352 px content height.
    lv_obj_t *page = lv_obj_create(s_content);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, kWidth, kContentH);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    // Vertical scroll stays here; horizontal swipes bubble up to switch tabs.
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Brightness.
    lv_obj_t *bright = makeCard(page, kMargin, 8, kWidth - 2 * kMargin, 92, 12);
    cardCaption(bright, "BRIGHTNESS");
    lv_obj_t *slider = lv_slider_create(bright);
    lv_obj_set_pos(slider, 0, 40);
    lv_obj_set_size(slider, kWidth - 2 * kMargin - 24 - 88, 24);
    lv_slider_set_range(slider, 0, 255);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorText), LV_PART_KNOB);
    // A fat knob and a generous hit area: a bare 16 px bar is nearly
    // impossible to grab with a fingertip.
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 12);
    lv_slider_set_value(slider, s_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightnessEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, brightnessEvent, LV_EVENT_RELEASED, nullptr);

    char text[64];
    snprintf(text, sizeof(text), "%u", static_cast<unsigned>(s_brightness));
    s_settings_bright_label = makeLabel(bright, text, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_pos(s_settings_bright_label, kWidth - 2 * kMargin - 24 - 76, 38);
    lv_obj_set_width(s_settings_bright_label, 76);
    lv_obj_set_style_text_align(s_settings_bright_label, LV_TEXT_ALIGN_RIGHT, 0);

    // Mic / speaker volume.
    const ExternalRadioConfig *acfg = EXTERNAL_RADIO_GetConfig();
    const int mic_pct = (acfg != nullptr)
                            ? (static_cast<int>(acfg->mic_volume) * 100 + 127) / 255
                            : 0;
    const int spk_pct = (acfg != nullptr)
                            ? (static_cast<int>(acfg->line_out_volume) * 100 + 127) / 255
                            : 0;
    buildVolumeCard(page, 108, tr("MIC VOLUME"), mic_pct, true, &s_settings_mic_label);
    buildVolumeCard(page, 208, tr("SPEAKER VOLUME"), spk_pct, false, &s_settings_spk_label);

    // Language.
    lv_obj_t *lang = makeCard(page, kMargin, 308, kWidth - 2 * kMargin, 64, 12);
    lv_obj_t *lang_label = makeLabel(lang, tr("Language"), &lv_font_montserrat_20, kColorText);
    lv_obj_align(lang_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *lang_btn = makeButton(lang, 0, 0, 140, 40, "", langEvent, nullptr);
    lv_obj_align(lang_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    s_settings_lang_btn_label = lv_obj_get_child(lang_btn, 0);
    lv_label_set_text(s_settings_lang_btn_label, s_lang == 0 ? "English" : "中文");

    // IMU auto-rotate toggle.
    lv_obj_t *rot = makeCard(page, kMargin, 380, kWidth - 2 * kMargin, 64, 12);
    lv_obj_t *rot_label = makeLabel(rot, tr("Auto-rotate"), &lv_font_montserrat_20, kColorText);
    lv_obj_align(rot_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *rot_sw = lv_switch_create(rot);
    lv_obj_set_size(rot_sw, 56, 32);
    lv_obj_align(rot_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(rot_sw, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    if (s_auto_rotate) {
        lv_obj_add_state(rot_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(rot_sw, autoRotateEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    // Manual rotation: cycle 0/90/180/270 (persisted; auto-rotate may
    // override later when it is enabled).
    lv_obj_t *mrot = makeCard(page, kMargin, 452, kWidth - 2 * kMargin, 64, 12);
    lv_obj_t *mrot_label = makeLabel(mrot, tr("Rotation"), &lv_font_montserrat_20, kColorText);
    lv_obj_align(mrot_label, LV_ALIGN_LEFT_MID, 0, 0);
    char rot_text[8];
    snprintf(rot_text, sizeof(rot_text), "%d\xC2\xB0", s_rotation);
    lv_obj_t *mrot_btn = makeButton(mrot, 0, 0, 100, 40, rot_text, rotationEvent, nullptr);
    lv_obj_align(mrot_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    s_settings_rot_btn_label = lv_obj_get_child(mrot_btn, 0);

    // Button vibration feedback toggle.
    lv_obj_t *hap = makeCard(page, kMargin, 524, kWidth - 2 * kMargin, 64, 12);
    lv_obj_t *hap_label = makeLabel(hap, tr("Vibration"), &lv_font_montserrat_20, kColorText);
    lv_obj_align(hap_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *hap_sw = lv_switch_create(hap);
    lv_obj_set_size(hap_sw, 56, 32);
    lv_obj_align(hap_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(hap_sw, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    if (s_haptic) {
        lv_obj_add_state(hap_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(hap_sw, hapticEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    // WiFi provisioning entry (SoftAP portal info).
    lv_obj_t *wifi = makeCard(page, kMargin, 596, kWidth - 2 * kMargin, 64, 12);
    lv_obj_t *wifi_label = makeLabel(wifi, tr("WiFi Setup"), &lv_font_montserrat_20, kColorText);
    lv_obj_align(wifi_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *wifi_btn = makeButton(wifi, 0, 0, 160, 40, tr("Hotspot Info"),
                                    provisioningInfoEvent, nullptr);
    lv_obj_align(wifi_btn, LV_ALIGN_RIGHT_MID, 0, 0);

    // About.
    lv_obj_t *about = makeCard(page, kMargin, 668, kWidth - 2 * kMargin, 92, 12);
    cardCaption(about, "ABOUT");
    snprintf(text, sizeof(text), "%s %s", tr("Firmware"), "v" NRL_FIRMWARE_VERSION);
    lv_obj_t *fw = makeLabel(about, text, &lv_font_montserrat_16, kColorText);
    lv_obj_set_pos(fw, 0, 22);
    snprintf(text, sizeof(text), "%s ESP-Mosaico  ·  IDF %s", tr("Board"),
             esp_get_idf_version());
    lv_obj_t *board = makeLabel(about, text, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_pos(board, 0, 48);
}

// ---- Provisioning screen (services not yet started) ----------------------------

void buildProvisioning()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);  // see buildMainUi
    s_content = nullptr;
    s_lbl_clock = nullptr;
    s_overlay = nullptr;
    for (int i = 0; i < kTabCount; ++i) {
        s_tab_btns[i] = nullptr;
        s_tab_labels[i] = nullptr;
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);

    lv_obj_t *title = makeLabel(scr, "WiFi Setup / 设备配网", &s_font_ui_28,
                                kColorAccent);
    lv_obj_set_width(title, kWidth - 32);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, kMargin, 40);

    lv_obj_t *box = makeCard(scr, 24, 110, kWidth - 48, 260, 16);

    lv_obj_t *wait = makeLabel(box, "正在等待网络设置 / Waiting for WiFi setup",
                               &lv_font_montserrat_16, kColorWarn);
    lv_obj_set_width(wait, kWidth - 80);
    lv_obj_set_style_text_align(wait, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(wait, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *step1 = makeLabel(box, "1. 连接设备热点 / Connect to hotspot:",
                                &lv_font_montserrat_16, kColorText);
    lv_obj_align(step1, LV_ALIGN_TOP_LEFT, 0, 48);

    s_prov_ssid = makeLabel(box, "NRL-ESP32-XXXXXX", &lv_font_montserrat_20, kColorGood);
    lv_obj_align(s_prov_ssid, LV_ALIGN_TOP_LEFT, 16, 76);

    lv_obj_t *step2 = makeLabel(box, "2. 浏览器打开 / Open in a browser:",
                                &lv_font_montserrat_16, kColorText);
    lv_obj_align(step2, LV_ALIGN_TOP_LEFT, 0, 116);

    s_prov_ip = makeLabel(box, "http://192.168.4.1/", &lv_font_montserrat_20, kColorGood);
    lv_obj_align(s_prov_ip, LV_ALIGN_TOP_LEFT, 16, 144);

    lv_obj_t *ble = makeLabel(box, "或通过微信小程序「NRL互联」蓝牙配网 / BLE via WeChat「NRL互联」",
                              &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(ble, kWidth - 80);
    lv_obj_align(ble, LV_ALIGN_TOP_LEFT, 0, 192);
}

void refreshProvisioning()
{
    if (s_prov_ssid == nullptr || s_prov_ip == nullptr) {
        return;
    }
    char ssid[40] = {};
    WifiConfigPortal_GetApSsid(ssid, sizeof(ssid));
    lv_label_set_text(s_prov_ssid, ssid);
    char ip[24] = "192.168.4.1";
    const uint32_t ap_ip = nrlWifiApIp();
    if (ap_ip != 0u) {
        nrlIpToString(ap_ip, ip, sizeof(ip));
    }
    char url[48];
    snprintf(url, sizeof(url), "http://%s/", ip);
    lv_label_set_text(s_prov_ip, url);
}

// ---- Screen assembly -----------------------------------------------------------

void buildMainUi()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    // Screens default to scrollable=1, and indev_gesture() bails out whenever
    // a scrollable ancestor exists -- that single default silently disabled
    // every swipe gesture on this UI.
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // All swipe gestures bubble up to the screen (gesture_bubble defaults to
    // 1 on every child), so one handler here covers page swipes anywhere and
    // edge swipe-ins from the bar/dock.
    lv_obj_add_event_cb(scr, screenGestureEvent, LV_EVENT_GESTURE, nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    s_overlay = nullptr;
    s_prov_ssid = nullptr;
    s_prov_ip = nullptr;

    buildStatusBar(scr);
    buildDock(scr);

    s_content = lv_obj_create(scr);
    lv_obj_set_pos(s_content, 0, kContentY);
    lv_obj_set_size(s_content, kWidth, kContentH);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    // Clickable so bare areas can be the press target; the resulting swipe
    // gesture still bubbles up to the screen handler (gesture_bubble).
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_CLICKABLE);

    buildPage();
    updateTabHighlight();
    refreshStatusBar();
}

void rebuildMainUi()
{
    memset(s_shown_clock, 0, sizeof(s_shown_clock));
    memset(s_shown_callsign, 0, sizeof(s_shown_callsign));
    memset(s_shown_wifi, 0, sizeof(s_shown_wifi));
    memset(s_shown_vol, 0, sizeof(s_shown_vol));
    memset(s_shown_batt, 0, sizeof(s_shown_batt));
    memset(s_shown_home_clock, 0, sizeof(s_shown_home_clock));
    buildMainUi();
}

// Live refresh dispatch: page-specific updates only while the page is active.
void refreshActivePage()
{
    switch (s_page) {
        case Page::Home: refreshHomePage(); break;
        case Page::Music: refreshMusicPage(); break;
        case Page::Sensors: refreshSensorsPage(); break;
        default: break;
    }
}

} // namespace

extern "C" void Display_Init(void)
{
    if (s_ready) {
        return;
    }
    s_panel = MosaicoPanel_Init();
    if (s_panel == nullptr) {
        ESP_LOGE(kTag, "panel init failed");
        return;
    }
    ESP_LOGI(kTag, "stage: lvgl");
    if (!initLvgl()) {
        return;
    }
    g_mosaico_boot_stage = 10;
    initFonts();
    loadUiLang();   // restore saved language before the first page is built
    loadBrightness();
    loadAutoRotate();
    loadRotation();
    loadHaptic();
    ESP_LOGI(kTag, "stage: touch");
    initTouch();
    g_mosaico_boot_stage = 11;
    ESP_LOGI(kTag, "stage: ui build");
    if (s_provisioning_mode) {
        buildProvisioning();
        refreshProvisioning();
    } else {
        buildMainUi();
    }
    g_mosaico_boot_stage = 12;
    ESP_LOGI(kTag, "stage: first render");
    lv_refr_now(nullptr);
    g_mosaico_boot_stage = 13;
    ESP_LOGI(kTag, "stage: first render done");
    s_ready = true;
    ESP_LOGI(kTag, "ready: %dx%d QSPI AMOLED", kWidth, kHeight);
}

extern "C" bool Display_IsReady(void)
{
    return s_ready;
}

// Printed by mosaico_usb_console.cpp whenever a host opens the CDC port, so
// the display/touch state is visible without a power-cycle.
extern "C" void MosaicoDisp_DumpStatus(void)
{
    ESP_LOGI(kTag, "status: ready=%d prov=%d panel=%s touch=%s rotation=%d bright=%u page=%d stage=%d",
             s_ready, s_provisioning_mode,
             s_panel != nullptr ? "ok" : "none",
             s_touch != nullptr ? "ok" : "none",
             s_rotation, static_cast<unsigned>(s_brightness),
             static_cast<int>(s_page), static_cast<int>(g_mosaico_boot_stage));

    // Hardware revision from eFuse USER_DATA (same scheme as the official BSP):
    // v1.2 moves I2C to GPIO56/3 and swaps LCD SCL/RST, so this decides pins.
    uint16_t hwver = 0;
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &hwver, 16) == ESP_OK) {
        ESP_LOGI(kTag, "hw version from eFuse: v%u.%u (raw=0x%04X)",
                 static_cast<unsigned>(hwver >> 8), static_cast<unsigned>(hwver & 0xFF),
                 static_cast<unsigned>(hwver));
    }
    // Live I2C bus health probe: codec 0x19, touch 0x5A, IMU 0x69, mag 0x11/0x12,
    // gauge 0x55. A NAK everywhere means the bus itself is dead (stuck low).
    ESP_LOGI(kTag, "i2c probe: es8311(19)=%d cst9220(5A)=%d bmi270(69)=%d bmm(11)=%d bmm(12)=%d bq(55)=%d",
             I2C_MasterProbe(0x19, 50), I2C_MasterProbe(0x5A, 50),
             I2C_MasterProbe(0x69, 50), I2C_MasterProbe(0x11, 50),
             I2C_MasterProbe(0x12, 50), I2C_MasterProbe(0x55, 50));
    // Live level of the GPIO7 function/PTT button (0 = pressed).
    ESP_LOGI(kTag, "btn gpio%d level=%d", NRL_PIN_BTN_PTT,
             gpio_get_level((gpio_num_t)NRL_PIN_BTN_PTT));
    // Local transmit gate (physical/soft PTT) vs. the on-screen TX indication.
    ESP_LOGI(kTag, "ptt: tx_gate=%d bridge_ptt=%d inbound_audio=%d linked=%d",
             STATUS_IO_IsSqlActive(), NRLAudioBridge_PttActive(),
             STATUS_IO_IsPttActive(), STATUS_IO_NrlServerLinked());
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    // vTaskList shows which task is blocked where (state + stack watermark) --
    // the main task hangs somewhere in Display_Init and we need to see it.
    char *list = static_cast<char *>(heap_caps_malloc(2048, MALLOC_CAP_INTERNAL));
    if (list != nullptr) {
        vTaskList(list);
        ESP_LOGI(kTag, "tasks:\n%s", list);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        // Runtime counters: comparing two dumps tells whether core 0 is alive
        // (IDLE0 counter advancing) or interrupt-locked.
        vTaskGetRunTimeStats(list);
        ESP_LOGI(kTag, "runtime:\n%s", list);
#endif
        heap_caps_free(list);
    }
#endif
}

extern "C" void Display_SetProvisioningMode(bool enabled)
{
    if (s_provisioning_mode == enabled) {
        return;
    }
    s_provisioning_mode = enabled;
    s_last_bar_ms = 0u;
    s_last_page_ms = 0u;
    if (!s_ready) {
        return;
    }
    if (enabled) {
        buildProvisioning();
        refreshProvisioning();
    } else {
        buildMainUi();
    }
    lv_refr_now(nullptr);
}

extern "C" void Display_Poll(void)
{
    if (!s_ready) {
        return;
    }
    const uint32_t now = millis();
    if (s_provisioning_mode) {
        if (s_last_bar_ms == 0u || (now - s_last_bar_ms) >= kBarRefreshMs) {
            s_last_bar_ms = now;
            refreshProvisioning();
        }
        lv_timer_handler();
        return;
    }
    if (s_last_bar_ms == 0u || (now - s_last_bar_ms) >= kBarRefreshMs) {
        s_last_bar_ms = now;
        refreshStatusBar();
    }
    pollAutoRotate(now);
    // Sensors run at ~4 Hz; other pages share the 500 ms status cadence.
    const uint32_t page_interval =
        (s_page == Page::Sensors) ? kSensorsRefreshMs : kBarRefreshMs;
    if (s_last_page_ms == 0u || (now - s_last_page_ms) >= page_interval) {
        s_last_page_ms = now;
        refreshActivePage();
    }
    if (s_volume_dirty && (now - s_volume_change_ms) >= kVolumeSaveDelayMs) {
        s_volume_dirty = !EXTERNAL_RADIO_SaveConfig();
    }
    lv_timer_handler();
}

extern "C" void Display_MenuOpen(void)
{
}

extern "C" bool Display_MenuIsActive(void)
{
    return false;
}

extern "C" void Display_MenuNavigate(int direction)
{
    (void)direction;
}

extern "C" void Display_MenuConfirm(void)
{
}

extern "C" void Display_HardwareKeyPress(enum DisplayHardwareKey key)
{
    (void)key;
}

extern "C" bool Display_CwIsActive(void)
{
    return false;
}

extern "C" void Display_CwExit(void)
{
}

extern "C" int Display_GetBatteryRawMv(void)
{
    return batteryMvRaw();
}

extern "C" int Display_GetBatteryCalibratedMv(void)
{
    const int raw = batteryMvRaw();
    if (raw <= 0) {
        return 0;
    }
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    const unsigned scale = (cfg != nullptr && cfg->battery_cal_milli != 0u)
                               ? cfg->battery_cal_milli
                               : 1000u;
    return static_cast<int>((static_cast<long>(raw) * static_cast<long>(scale) + 500L) / 1000L);
}

extern "C" long Display_FramebufferBenchMBps(void)
{
    return -1;
}

extern "C" bool Display_SetCjkFontEngine(int engine)
{
    (void)engine;
    return false;
}

extern "C" int Display_GetCjkFontEngine(void)
{
    return DISPLAY_CJK_FONT_BITMAP;
}

#endif // NRL_BOARD == NRL_BOARD_ESP_MOSAICO
