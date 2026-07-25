// On-device LCD UI for the 格子派 board.
//
// Hardware: ST7789 240x240 SPI panel, identical wiring to the 小智 (xiaozhi)
// 格子派 board (see src/app/driver/board_pins.h). The panel is an IDF esp_lcd
// ST7789 device; rendering uses LVGL. LVGL is vendored as a local component
// (components/lvgl) and driven directly here -- no esp_lvgl_port -- so the
// build never has to download anything from the component registry.
//
// Layout (clean, dark "tech" theme):
//   +------------------------------------------+
//   | -72dB            75%             4.05V   |  <- status bar
//   |------------------------------------------|
//   |                RECEIVING                 |  <- caption (TX/RX state)
//   |               B G 7 X Y Z                 |  <- callsign (large)
//   |                 SSID 5                    |  <- callsign SSID (smaller)
//   |                10:24:37                   |  <- current time
//   |                                           |
//   |               192.168.1.50               |  <- address bar
//   +------------------------------------------+
//
// The callsign area shows the remote caller while a voice stream is being
// received, and this device's own callsign/SSID otherwise. The caption tracks
// the live state: STANDBY / RECEIVING / TRANSMITTING / FULL DUPLEX. Heartbeat
// packets never count as "receiving".
//
// The address bar normally shows the station IP; while transmitting OR
// receiving voice it shows the NRL server host instead (red for TX, cyan for
// RX). When WiFi cannot join a router and the device falls back to its config
// AP, the bar shows the AP hotspot address (192.168.4.1) in amber.

#include "display.h"

#include "board_pins.h"
#include "i2c1.h"
#if NRL_BOARD == NRL_BOARD_BI4UMD
#include "display_bi4umd.h"
#include "touch_bi4umd.h"
#endif

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD_IS_GEZIPAI_FAMILY

#include "../../lib/nrl_audio_bridge.h"
#include "../../lib/ble_config.h"
#include "../../lib/nrl_psram.h"
#include "../../lib/wifi_config_portal.h"
#include "../../services/aprs_service.h"
#include "../../services/espnow_link.h"
#include "../../services/display_notice.h"
#include "../../services/music_player.h"
#include "../../services/music_playlist.h"
#include "../../services/ota_service.h"
#include "../../services/radio_favorites.h"
#include "../../services/storage_service.h"
#include "../../services/signaling_service.h"
#include "../../lib/nrl_version.h"
#include "external_radio.h"
#include "fonts/lv_font_cjk.h"
#include "status_io.h"

#include "../../lib/nrl_net_compat.h"

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "LCD";
#include <esp_adc/adc_oneshot.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#if NRL_DISPLAY_BUS_RGB
// Touch is only wired on the RGB-panel path (all esp_lcd_touch* usage below is
// under NRL_DISPLAY_BUS_RGB). Keeping the include out of the ST7789 path avoids
// pulling the esp_lcd_touch component on gezipai, which has no touch.
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_gt1151.h>
#endif
#include <esp_lcd_panel_vendor.h>

#include <lvgl.h>

namespace {

constexpr int kWidth = NRL_DISPLAY_WIDTH;
constexpr int kHeight = NRL_DISPLAY_HEIGHT;

#ifndef NRL_DISPLAY_BUS_ST7789
#define NRL_DISPLAY_BUS_ST7789 0
#endif
#ifndef NRL_DISPLAY_BUS_RGB
#define NRL_DISPLAY_BUS_RGB 0
#endif

// LVGL partial render buffer height, in scan lines.
constexpr int kBufLines = 60;

// ---- Tech-style palette (0xRRGGBB) ----
constexpr uint32_t kColorBg       = 0x070B11;  // near-black screen background
constexpr uint32_t kColorBar      = 0x0E1622;  // status/IP bar fill
constexpr uint32_t kColorAccent   = 0x22D3EE;  // cyan accent line
constexpr uint32_t kColorCallIdle = 0xE6EDF3;  // callsign while idle
constexpr uint32_t kColorCallLive = 0x22D3EE;  // callsign while receiving
constexpr uint32_t kColorSub      = 0x6F8BA0;  // muted blue-gray (SSID, bars)
constexpr uint32_t kColorTime     = 0xBFE9F5;  // clock
constexpr uint32_t kColorCaption  = 0x46627A;  // dim caption above callsign
constexpr uint32_t kColorIp       = 0x46D6E6;  // IP address (STA mode)
constexpr uint32_t kColorApWarn   = 0xF5B453;  // amber: config / AP mode
constexpr uint32_t kColorGood     = 0x4ADE80;  // strong signal
constexpr uint32_t kColorWeak     = 0xF87171;  // weak signal / low battery
constexpr uint32_t kColorTx       = 0xFF6B6B;  // transmitting (PTT held)
constexpr uint32_t kColorDuplex   = 0xA78BFA;  // transmitting + receiving

// APRS packets may contain Chinese comments. Keep the normal Latin UI font,
// but fall back to the bundled 16px GB2312 font for the ticker only.
lv_font_t s_font_aprs_16;
#if NRL_BOARD == NRL_BOARD_BI4UMD
lv_font_t s_font_music_20;
constexpr size_t kBi4umdMusicListMaxRows = 48u;
#endif

// Battery pack assumed to be a single Li-ion cell (3.0 V .. 4.2 V).
constexpr int kBatteryMinMv = 3000;
constexpr int kBatteryMaxMv = 4200;

constexpr uint32_t kRefreshIntervalMs = 500u;
constexpr uint32_t kBatteryIntervalMs = 10000u;
constexpr int kStatusBarHeight = 34;
constexpr int kContentY = 36;
constexpr int kBottomBarHeight = 34;
constexpr int kContentHeight = kHeight - kContentY - kBottomBarHeight;

bool s_ready = false;
bool s_provisioning_mode = false;
lv_obj_t *s_lbl_provision_ip = nullptr;
lv_obj_t *s_lbl_provision_ssid = nullptr;
lv_obj_t *s_lbl_provision_ble = nullptr;

esp_lcd_panel_io_handle_t s_panel_io = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;

lv_display_t *s_disp = nullptr;
uint8_t *s_draw_buf = nullptr;
#if NRL_DISPLAY_BUS_RGB
NRL_PSRAM_BSS uint8_t s_rgb_draw_buffer[kWidth * kBufLines * 2u];
esp_lcd_touch_handle_t s_touch = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
lv_indev_t *s_touch_indev = nullptr;
#elif NRL_BOARD == NRL_BOARD_BI4UMD
lv_indev_t *s_touch_indev = nullptr;
#endif

lv_obj_t *s_lbl_caption = nullptr;
lv_obj_t *s_lbl_callsign = nullptr;
lv_obj_t *s_lbl_ssid = nullptr;
lv_obj_t *s_lbl_time = nullptr;
lv_obj_t *s_lbl_wifi = nullptr;
lv_obj_t *s_lbl_vol = nullptr;
lv_obj_t *s_lbl_batt = nullptr;
lv_obj_t *s_lbl_ip = nullptr;
lv_obj_t *s_lbl_cpu = nullptr;
lv_obj_t *s_lbl_gps = nullptr;
lv_obj_t *s_lbl_hint = nullptr;
lv_obj_t *s_lbl_ota = nullptr;
lv_obj_t *s_bar_ota = nullptr;
lv_obj_t *s_content = nullptr;
#if NRL_BOARD == NRL_BOARD_BI4UMD
lv_obj_t *s_lbl_signaling = nullptr;
enum class Bi4umdPage : uint8_t { Radio, Music, MusicList, Settings };
Bi4umdPage s_bi4umd_page = Bi4umdPage::Radio;
lv_obj_t *s_lbl_music_title = nullptr;
lv_obj_t *s_lbl_music_artist = nullptr;
lv_obj_t *s_lbl_music_state = nullptr;
lv_obj_t *s_lbl_music_format = nullptr;
lv_obj_t *s_lbl_music_source = nullptr;
lv_obj_t *s_list_music = nullptr;
lv_obj_t *s_btn_music_play_label = nullptr;
lv_obj_t *s_btn_music_repeat_label = nullptr;
lv_obj_t *s_lbl_settings_mic = nullptr;
lv_obj_t *s_lbl_settings_volume = nullptr;
char s_shown_music_path[256] = {};
bool s_shown_music_playing = false;
size_t s_music_tap_index = SIZE_MAX;
uint32_t s_music_tap_ms = 0u;
lv_obj_t *s_music_tap_row = nullptr;
bool s_bi4umd_aprs_from_settings = false;
#endif

adc_oneshot_unit_handle_t s_adc = nullptr;
adc_cali_handle_t s_adc_cali = nullptr;
bool s_adc_ready = false;

uint32_t s_last_refresh_ms = 0u;
uint32_t s_last_battery_ms = 0u;
int s_battery_mv = 0;
bool s_time_sync_started = false;

enum class MenuPage : uint8_t {
    Main,
    Language,
    Ota,
    About,
    Aprs,
    AprsSettings,
    AprsList,
    AprsGps,
    Signaling,
    Ctcss,
    Mdc,
    Dtmf,
};

// Written by STATUS_IO_Poll() and consumed only by Display_Poll(). Keeping
// LVGL out of the button/audio task avoids cross-task widget access.
volatile bool s_menu_active = false;
volatile bool s_menu_open_requested = false;
volatile int s_menu_nav_pending = 0;
volatile unsigned s_menu_confirm_pending = 0u;
MenuPage s_menu_page = MenuPage::Main;
bool s_menu_chinese = true;
size_t s_menu_index = 0u;
bool s_menu_ota_requested = false;
uint32_t s_menu_ota_check_baseline_ms = 0u;
uint32_t s_menu_ota_refresh_ms = 0u;
uint32_t s_menu_aprs_refresh_ms = 0u;
uint32_t s_menu_aprs_revision = 0u;
char s_menu_ota_state[224] = {};
char s_menu_message[64] = {};
uint32_t s_menu_message_until_ms = 0u;
// The full OTA manifest is about 3.7 KB. Keeping it as a local variable in
// processMenuInput() consumed most of nrl_main_loop's 6 KB stack even when the
// main menu (not the OTA page) was being opened, corrupting its return address.
NrlOtaStatus *s_ota_ui_status = nullptr;

const char *menuText(const char *english, const char *chinese)
{
    return s_menu_chinese ? chinese : english;
}

void localizeDisplayNotice(char *out, size_t out_size, const char *notice)
{
    if (out == nullptr || out_size == 0u) return;
    if (notice == nullptr) notice = "";
    if (!s_menu_chinese) {
        snprintf(out, out_size, "%s", notice);
        return;
    }

    struct NoticeTranslation {
        const char *english;
        const char *chinese;
    };
    static constexpr NoticeTranslation kOtaNotices[] = {
        {"OTA CHECKING...", "OTA检查中..."},
        {"FIRMWARE IS UP TO DATE", "固件已是最新版本"},
        {"OTA CHECK FAILED", "OTA检查失败"},
        {"OTA UPDATE FAILED", "OTA升级失败"},
        {"OTA UPDATING...", "OTA升级中..."},
        {"OTA UPLOADING...", "OTA上传中..."},
        {"OTA COMPLETE - REBOOTING", "OTA完成，正在重启"},
    };
    for (const auto &translation : kOtaNotices) {
        if (strcmp(notice, translation.english) == 0) {
            snprintf(out, out_size, "%s", translation.chinese);
            return;
        }
    }
    constexpr char kUpdatingPrefix[] = "OTA UPDATING ";
    if (strncmp(notice, kUpdatingPrefix, sizeof(kUpdatingPrefix) - 1u) == 0) {
        snprintf(out, out_size, "OTA升级中 %s", notice + sizeof(kUpdatingPrefix) - 1u);
        return;
    }
    constexpr char kNewFirmwarePrefix[] = "NEW FIRMWARE ";
    if (strncmp(notice, kNewFirmwarePrefix, sizeof(kNewFirmwarePrefix) - 1u) == 0) {
        snprintf(out, out_size, "发现新固件 %s", notice + sizeof(kNewFirmwarePrefix) - 1u);
        return;
    }
    snprintf(out, out_size, "%s", notice);
}

const lv_font_t *menuFont(const lv_font_t *english_font)
{
    return s_menu_chinese ? &s_font_aprs_16 : english_font;
}

void loadMenuLanguage()
{
    nvs_handle_t nvs;
    uint8_t language = 1u;
    if (nvs_open("display", NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "language", &language);
        nvs_close(nvs);
    }
    s_menu_chinese = language != 0u;
}

void saveMenuLanguage()
{
    nvs_handle_t nvs;
    if (nvs_open("display", NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_set_u8(nvs, "language", s_menu_chinese ? 1u : 0u);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// Cached on-screen text, so labels are only rewritten when a value changes.
char s_shown_callsign[16] = {};
char s_shown_ssid[160] = {};
char s_shown_time[16] = {};
char s_shown_wifi[28] = {};
char s_shown_vol[16] = {};
char s_shown_batt[20] = {};
char s_shown_ip[96] = {};
char s_shown_cpu[12] = {};
char s_shown_gps[16] = {};
char s_shown_ota[160] = {}; // sized for a scrolling APRS monitor line
#if NRL_BOARD == NRL_BOARD_BI4UMD
char s_shown_signaling[160] = {};
#endif
int s_shown_state = -1;  // caption: -1 unset, 0 standby, 1 last heard, 2 rx, 3 tx
bool s_shown_media = false;
char s_cached_radio_path[256] = {};
char s_cached_radio_name[RADIO_FAV_NAME_SIZE] = {};
uint32_t s_radio_name_refresh_ms = 0u;

void refreshVolume();
lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color);
void buildUi();
void buildProvisioningUi();
void buildHomeContent();
void buildMenuUi();
size_t menuItemCount();
#if NRL_BOARD == NRL_BOARD_BI4UMD
void menuTouchPressed(lv_event_t *event);
void menuTouchReleased(lv_event_t *event);
#endif
#if NRL_BOARD == NRL_BOARD_BI4UMD
void buildBi4umdMusicContent();
void buildBi4umdMusicListContent();
void buildBi4umdSettingsContent();
void refreshBi4umdMusic();
void rebuildBi4umdMusicList();
#endif

NrlOtaStatus *otaUiSnapshot()
{
    if (s_ota_ui_status == nullptr) {
        s_ota_ui_status = static_cast<NrlOtaStatus *>(
            heap_caps_calloc(1, sizeof(NrlOtaStatus), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_ota_ui_status == nullptr) {
            s_ota_ui_status = static_cast<NrlOtaStatus *>(
                heap_caps_calloc(1, sizeof(NrlOtaStatus), MALLOC_CAP_8BIT));
        }
    }
    if (s_ota_ui_status == nullptr) {
        return nullptr;
    }
    memset(s_ota_ui_status, 0, sizeof(*s_ota_ui_status));
    OtaService_GetStatus(s_ota_ui_status);
    return s_ota_ui_status;
}

//============================ Panel bring-up =================================

bool initPanel()
{
#if NRL_BOARD == NRL_BOARD_BI4UMD
    if (!BI4UMD_Display_Init(&s_panel_io)) {
        ESP_LOGI(TAG,"[LCD] BI4UMD ILI9341V init failed");
        return false;
    }
    return true;
#else
#if NRL_PIN_DISPLAY_BL >= 0
    // Backlight off until the first frame is drawn, to avoid a white flash.
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << NRL_PIN_DISPLAY_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_DISPLAY_BL), 0);
#endif

#if NRL_DISPLAY_BUS_RGB
    esp_lcd_rgb_panel_config_t panel_cfg = {};
    panel_cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_cfg.timings.pclk_hz = 26 * 1000 * 1000;
    panel_cfg.timings.h_res = kWidth;
    panel_cfg.timings.v_res = kHeight;
    panel_cfg.timings.hsync_pulse_width = 1;
    panel_cfg.timings.hsync_back_porch = 40;
    panel_cfg.timings.hsync_front_porch = 20;
    panel_cfg.timings.vsync_pulse_width = 1;
    panel_cfg.timings.vsync_back_porch = 10;
    panel_cfg.timings.vsync_front_porch = 5;
    panel_cfg.timings.flags.pclk_active_neg = true;
    panel_cfg.data_width = 16;
    panel_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    panel_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    panel_cfg.num_fbs = 2;
    panel_cfg.dma_burst_size = 128;
    panel_cfg.hsync_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_HSYNC);
    panel_cfg.vsync_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_VSYNC);
    panel_cfg.de_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_DE);
    panel_cfg.pclk_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_PCLK);
    panel_cfg.disp_gpio_num = static_cast<gpio_num_t>(NRL_PIN_LCD_DISP);
    panel_cfg.data_gpio_nums[0] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA0);
    panel_cfg.data_gpio_nums[1] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA1);
    panel_cfg.data_gpio_nums[2] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA2);
    panel_cfg.data_gpio_nums[3] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA3);
    panel_cfg.data_gpio_nums[4] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA4);
    panel_cfg.data_gpio_nums[5] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA5);
    panel_cfg.data_gpio_nums[6] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA6);
    panel_cfg.data_gpio_nums[7] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA7);
    panel_cfg.data_gpio_nums[8] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA8);
    panel_cfg.data_gpio_nums[9] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA9);
    panel_cfg.data_gpio_nums[10] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA10);
    panel_cfg.data_gpio_nums[11] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA11);
    panel_cfg.data_gpio_nums[12] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA12);
    panel_cfg.data_gpio_nums[13] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA13);
    panel_cfg.data_gpio_nums[14] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA14);
    panel_cfg.data_gpio_nums[15] = static_cast<gpio_num_t>(NRL_PIN_LCD_DATA15);
    panel_cfg.flags.fb_in_psram = true;
    if (esp_lcd_new_rgb_panel(&panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] RGB panel init failed");
        return false;
    }
#else
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = NRL_PIN_DISPLAY_SCLK;
    bus_cfg.mosi_io_num = NRL_PIN_DISPLAY_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = kWidth * kBufLines * static_cast<int>(sizeof(uint16_t));
    if (spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] SPI bus init failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_CS);
    io_cfg.dc_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_DC);
    io_cfg.spi_mode = 3;
    io_cfg.pclk_hz = 80 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &s_panel_io) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] panel IO init failed");
        return false;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(NRL_PIN_DISPLAY_RST);
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] ST7789 init failed");
        return false;
    }
#endif

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
#if NRL_DISPLAY_BUS_RGB
    esp_lcd_panel_invert_color(s_panel, false);
#else
    esp_lcd_panel_invert_color(s_panel, true);
#endif
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return true;
#endif
}

//============================ LVGL <-> esp_lcd ===============================

// LVGL time base: millis() is a 32-bit millisecond counter, exactly what LVGL
// wants from its tick callback.
uint32_t lvglTick()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// esp_lcd finished pushing a chunk over SPI -> let LVGL reuse the buffer.
// Runs in the SPI ISR; lv_display_flush_ready() only flips a flag, so this is
// safe here.
bool onColorTransDone(esp_lcd_panel_io_handle_t /*io*/,
                      esp_lcd_panel_io_event_data_t * /*edata*/,
                      void *user_ctx)
{
    lv_display_flush_ready(static_cast<lv_display_t *>(user_ctx));
    return false;
}

// LVGL hands us a rendered chunk; byte-swap it (ST7789-over-SPI is big-endian
// RGB565, LVGL renders little-endian) and DMA it to the panel.
void lvglFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
#if NRL_BOARD == NRL_BOARD_BI4UMD
    const int32_t count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    if (!BI4UMD_Display_Flush(s_panel_io, area->x1, area->y1,
                              area->x2, area->y2, px_map,
                              static_cast<size_t>(count))) {
        // A failed queued transfer has no completion callback. Release LVGL's
        // render buffer here so one SPI error cannot freeze all later frames.
        ESP_LOGE(TAG, "[LCD] BI4UMD flush failed (%ld pixels)",
                 static_cast<long>(count));
        lv_display_flush_ready(disp);
    }
#else
    esp_lcd_panel_handle_t panel =
        static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));

#if NRL_DISPLAY_BUS_ST7789
    const int32_t count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    uint16_t *pixels = reinterpret_cast<uint16_t *>(px_map);
    for (int32_t i = 0; i < count; ++i) {
        pixels[i] = static_cast<uint16_t>((pixels[i] >> 8) | (pixels[i] << 8));
    }
#endif

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
#if NRL_DISPLAY_BUS_RGB
    lv_display_flush_ready(disp);
#endif
#endif
}

bool initLvgl()
{
    lv_init();

    const size_t buf_bytes = static_cast<size_t>(kWidth) * kBufLines * 2u;
#if NRL_DISPLAY_BUS_RGB
    s_draw_buf = s_rgb_draw_buffer;
#else
    s_draw_buf = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA));
#endif
    if (s_draw_buf == nullptr) {
        ESP_LOGI(TAG,"[LCD] draw buffer alloc failed");
        return false;
    }

    s_disp = lv_display_create(kWidth, kHeight);
    if (s_disp == nullptr) {
        ESP_LOGI(TAG,"[LCD] lv_display_create failed");
        return false;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_flush_cb(s_disp, lvglFlush);
    lv_display_set_buffers(s_disp, s_draw_buf, nullptr, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_tick_set_cb(lvglTick);

#if NRL_DISPLAY_BUS_ST7789 || NRL_DISPLAY_BUS_ILI9341
    esp_lcd_panel_io_callbacks_t io_cbs = {};
    io_cbs.on_color_trans_done = onColorTransDone;
    esp_lcd_panel_io_register_event_callbacks(s_panel_io, &io_cbs, s_disp);
#endif
    return true;
}

#if NRL_BOARD == NRL_BOARD_BI4UMD
void bi4umdTouchRead(lv_indev_t *, lv_indev_data_t *data)
{
    uint16_t x = 0;
    uint16_t y = 0;
    if (data != nullptr && BI4UMD_Touch_Read(&x, &y)) {
        data->point.x = static_cast<int16_t>(x);
        data->point.y = static_cast<int16_t>(y);
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (data != nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool initBi4umdTouch()
{
    if (!BI4UMD_Touch_Init()) return false;
    s_touch_indev = lv_indev_create();
    if (s_touch_indev == nullptr) return false;
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, s_disp);
    lv_indev_set_read_cb(s_touch_indev, bi4umdTouchRead);
    return true;
}

void bi4umdPttEvent(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) STATUS_IO_SetSoftPtt(true);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) STATUS_IO_SetSoftPtt(false);
}

bool bi4umdIsRadioPath(const char *path)
{
    return path != nullptr &&
           (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0);
}

size_t bi4umdScanSdMusic()
{
    if (!STORAGE_SdMounted() && !STORAGE_SdMountRetry()) {
        return 0u;
    }

    size_t tracks = PLAYLIST_Scan();
    if (PLAYLIST_AtRoot()) {
        const size_t dirs = PLAYLIST_DirCount();
        for (size_t i = 0; i < dirs; ++i) {
            const char *path = PLAYLIST_GetDirPath(i);
            if (path != nullptr && strncmp(path, "/sdcard", 7) == 0) {
                if (PLAYLIST_EnterDir(i)) {
                    tracks = PLAYLIST_Count();
                }
                break;
            }
        }
    }
    return tracks;
}

void bi4umdShowRadioPage(lv_event_t *)
{
    STATUS_IO_SetSoftPtt(false);
    s_bi4umd_page = Bi4umdPage::Radio;
    buildHomeContent();
}

void bi4umdShowMusicPage(lv_event_t *)
{
    STATUS_IO_SetSoftPtt(false);
    s_bi4umd_page = Bi4umdPage::Music;
    buildBi4umdMusicContent();
    if (!MUSIC_IsPlaying() && PLAYLIST_Count() == 0u) {
        const size_t tracks = bi4umdScanSdMusic();
        char status[40] = {};
        snprintf(status, sizeof(status), STORAGE_SdMounted() ? "SD: %u tracks" : "SD mount failed",
                 static_cast<unsigned>(tracks));
        lv_label_set_text(s_lbl_music_source, status);
        rebuildBi4umdMusicList();
    }
}

void bi4umdShowMusicListPage(lv_event_t *)
{
    s_bi4umd_page = Bi4umdPage::MusicList;
    buildBi4umdMusicListContent();
}

void bi4umdShowSettingsPage(lv_event_t *)
{
    STATUS_IO_SetSoftPtt(false);
    s_bi4umd_page = Bi4umdPage::Settings;
    buildBi4umdSettingsContent();
}

void bi4umdOpenMainMenu(lv_event_t *)
{
    s_bi4umd_aprs_from_settings = false;
    s_bi4umd_page = Bi4umdPage::Radio;
    Display_MenuOpen();
}

void bi4umdOpenAprsPage(const MenuPage page)
{
    STATUS_IO_SetSoftPtt(false);
    s_bi4umd_page = Bi4umdPage::Radio;
    s_menu_active = true;
    s_menu_open_requested = false;
    s_menu_nav_pending = 0;
    s_menu_confirm_pending = 0u;
    s_menu_message[0] = '\0';
    s_menu_page = page;
    s_menu_index = 0u;
    s_menu_aprs_refresh_ms = 0u;
    s_menu_aprs_revision = APRS_SERVICE_GetStationRevision();
    s_bi4umd_aprs_from_settings = true;
    buildMenuUi();
}

void bi4umdOpenGpsPage(lv_event_t *)
{
    bi4umdOpenAprsPage(MenuPage::AprsGps);
}

void bi4umdOpenAprsListPage(lv_event_t *)
{
    bi4umdOpenAprsPage(MenuPage::AprsList);
}

void bi4umdMenuUp(lv_event_t *) { Display_MenuNavigate(1); }
void bi4umdMenuDown(lv_event_t *) { Display_MenuNavigate(-1); }
void bi4umdMenuConfirm(lv_event_t *) { Display_MenuConfirm(); }

void addBi4umdMenuButtons()
{
#if NRL_BOARD == NRL_BOARD_BI4UMD
    if (s_content == nullptr) return;
    auto menu_button = [](int x, const char *text, lv_event_cb_t callback) {
        lv_obj_t *button = lv_button_create(s_content);
        lv_obj_set_pos(button, x,
                       s_menu_page == MenuPage::AprsList || s_menu_page == MenuPage::AprsGps
                           ? 208 : 178);
        lv_obj_set_size(button, 64, 38);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x10212A), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x087A82), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(button, lv_color_hex(0x1C6B73), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *label = makeLabel(button, &s_font_aprs_16, kColorCallIdle);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    };
#if NRL_BOARD == NRL_BOARD_BI4UMD
    if (s_menu_page == MenuPage::AprsList || s_menu_page == MenuPage::AprsGps) {
        menu_button(166, menuText("BACK", "返回"), bi4umdMenuConfirm);
        return;
    }
    if (s_menu_page == MenuPage::About) {
        menu_button(88, menuText("BACK", "返回"), bi4umdMenuConfirm);
        return;
    }
#endif
    menu_button(10, menuText("UP", "上"), bi4umdMenuUp);
    menu_button(88, menuText("DOWN", "下"), bi4umdMenuDown);
    menu_button(166, menuText("OK", "确认"), bi4umdMenuConfirm);
#endif
}

void bi4umdMusicPrev(lv_event_t *)
{
    if (bi4umdIsRadioPath(MUSIC_CurrentPath())) (void)RADIO_FAV_Prev();
    else (void)PLAYLIST_Prev();
}

void bi4umdMusicNext(lv_event_t *)
{
    if (bi4umdIsRadioPath(MUSIC_CurrentPath())) (void)RADIO_FAV_Next();
    else (void)PLAYLIST_Next();
}

void bi4umdMusicToggle(lv_event_t *)
{
    if (MUSIC_IsPlaying()) {
        MUSIC_Stop();
        return;
    }

    char path[256] = {};
    snprintf(path, sizeof(path), "%s", MUSIC_CurrentPath());
    if (path[0] != '\0') {
        (void)MUSIC_PlayFile(path);
        return;
    }

    MUSIC_GetRadioUrl(path, sizeof(path));
    if (path[0] != '\0') (void)MUSIC_PlayFile(path);
    else (void)PLAYLIST_Next();
}

void bi4umdMusicRepeat(lv_event_t *)
{
    (void)PLAYLIST_ToggleRepeatMode();
    refreshBi4umdMusic();
}

void bi4umdMusicRefresh(lv_event_t *)
{
    lv_label_set_text(s_lbl_music_source, "Scanning SD...");
    lv_refr_now(nullptr);
    const size_t tracks = bi4umdScanSdMusic();
    char status[40] = {};
    snprintf(status, sizeof(status), STORAGE_SdMounted() ? "SD: %u tracks" : "SD mount failed",
             static_cast<unsigned>(tracks));
    lv_label_set_text(s_lbl_music_source, status);
    rebuildBi4umdMusicList();
}

void bi4umdMusicAdjustVolume(const int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) return;
    int volume = static_cast<int>(cfg->line_out_volume) + delta;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    if (volume != static_cast<int>(cfg->line_out_volume)) {
        EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), true);
        refreshVolume();
    }
}

void bi4umdMusicVolumeDown(lv_event_t *) { bi4umdMusicAdjustVolume(-16); }
void bi4umdMusicVolumeUp(lv_event_t *) { bi4umdMusicAdjustVolume(16); }

void refreshBi4umdSettingsValues()
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) return;
    char text[12] = {};
    if (s_lbl_settings_mic != nullptr) {
        snprintf(text, sizeof(text), "%u", static_cast<unsigned>(cfg->mic_volume));
        lv_label_set_text(s_lbl_settings_mic, text);
    }
    if (s_lbl_settings_volume != nullptr) {
        snprintf(text, sizeof(text), "%u", static_cast<unsigned>(cfg->line_out_volume));
        lv_label_set_text(s_lbl_settings_volume, text);
    }
}

void bi4umdSettingsAdjustMic(const int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) return;
    int value = static_cast<int>(cfg->mic_volume) + delta;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    if (value != static_cast<int>(cfg->mic_volume)) {
        EXTERNAL_RADIO_SetMicVolume(static_cast<uint8_t>(value), true);
        refreshBi4umdSettingsValues();
    }
}

void bi4umdSettingsMicDown(lv_event_t *) { bi4umdSettingsAdjustMic(-16); }
void bi4umdSettingsMicUp(lv_event_t *) { bi4umdSettingsAdjustMic(16); }
void bi4umdSettingsVolumeDown(lv_event_t *)
{
    bi4umdMusicAdjustVolume(-16);
    refreshBi4umdSettingsValues();
}
void bi4umdSettingsVolumeUp(lv_event_t *)
{
    bi4umdMusicAdjustVolume(16);
    refreshBi4umdSettingsValues();
}
#endif

#if NRL_DISPLAY_BUS_RGB
void touchRead(lv_indev_t * /*indev*/, lv_indev_data_t *data)
{
    if (s_touch == nullptr || data == nullptr) {
        return;
    }

    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t strength = 0;
    uint8_t count = 0;
    esp_lcd_touch_read_data(s_touch);
    const bool pressed = esp_lcd_touch_get_coordinates(s_touch, &x, &y, &strength, &count, 1);
    if (pressed && count > 0) {
        data->point.x = static_cast<int16_t>(x);
        data->point.y = static_cast<int16_t>(y);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool initTouch()
{
    i2c_master_bus_handle_t i2c_bus = nullptr;
    if (!I2C_MasterGetBus(&i2c_bus)) {
        ESP_LOGW(TAG, "[LCD] touch I2C unavailable");
        return false;
    }

    esp_lcd_touch_config_t touch_cfg = {};
    touch_cfg.x_max = kWidth;
    touch_cfg.y_max = kHeight;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC;
    touch_cfg.int_gpio_num = GPIO_NUM_NC;
    touch_cfg.levels.reset = 0;
    touch_cfg.levels.interrupt = 0;

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &s_touch_io) != ESP_OK) {
        ESP_LOGW(TAG, "[LCD] touch IO init failed");
        return false;
    }
    if (esp_lcd_touch_new_i2c_gt1151(s_touch_io, &touch_cfg, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "[LCD] GT1151 init failed");
        return false;
    }

    s_touch_indev = lv_indev_create();
    if (s_touch_indev == nullptr) {
        ESP_LOGW(TAG, "[LCD] touch LVGL indev init failed");
        return false;
    }
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, s_disp);
    lv_indev_set_read_cb(s_touch_indev, touchRead);
    ESP_LOGI(TAG, "[LCD] GT1151 touch ready");
    return true;
}

void adjustTouchVolume(const int delta)
{
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg == nullptr) {
        return;
    }
    int volume = static_cast<int>(cfg->line_out_volume) + delta;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 255) {
        volume = 255;
    }
    if (volume == static_cast<int>(cfg->line_out_volume)) {
        return;
    }
    EXTERNAL_RADIO_SetLineOutVolume(static_cast<uint8_t>(volume), true);
    refreshVolume();
}

void onTouchButton(lv_event_t *event)
{
    const intptr_t id = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (id < 0) {
        adjustTouchVolume(-16);
    } else if (id > 0) {
        adjustTouchVolume(16);
    } else {
        ESP_LOGI(TAG, "[LCD] config touch action");
    }
}
#endif

//============================ ADC battery sense ==============================

void initBatteryAdc()
{
#if defined(NRL_HAS_BATTERY_ADC) && NRL_HAS_BATTERY_ADC
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] battery ADC unit init failed");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_adc, NRL_BATTERY_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGI(TAG,"[LCD] battery ADC channel config failed");
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.chan = NRL_BATTERY_ADC_CHANNEL;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        s_adc_ready = true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_adc_ready) {
        adc_cali_line_fitting_config_t cali_cfg = {};
        cali_cfg.unit_id = ADC_UNIT_1;
        cali_cfg.atten = ADC_ATTEN_DB_12;
        cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
            s_adc_ready = true;
        }
    }
#endif
    if (!s_adc_ready) {
        ESP_LOGI(TAG,"[LCD] battery ADC not calibrated (eFuse missing) -- voltage hidden");
    }
#else
    ESP_LOGI(TAG,"[LCD] battery ADC not present on this board");
#endif
}

// Reads the raw (uncalibrated) battery voltage in millivolts, or 0 if
// unavailable. The sense pin sits behind a 1:2 divider, so the ADC reading is
// multiplied by 3 to match the 小智 格子派 board's measurement.
int readBatteryRawMv()
{
#if defined(NRL_HAS_BATTERY_ADC) && NRL_HAS_BATTERY_ADC
    if (!s_adc_ready || s_adc == nullptr || s_adc_cali == nullptr) {
        return 0;
    }

    long sum = 0;
    int samples = 0;
    for (int i = 0; i < 16; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, NRL_BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            ++samples;
        }
    }
    if (samples == 0) {
        return 0;
    }

    int voltage = 0;
    if (adc_cali_raw_to_voltage(s_adc_cali, static_cast<int>(sum / samples), &voltage) != ESP_OK) {
        return 0;
    }
    return voltage * 3;
#else
    return 0;
#endif
}

// Applies the persisted calibration multiplier (battery_cal_milli / 1000) to
// the raw voltage. Falls back to the raw value if the config cannot be read.
int applyBatteryCalibration(const int raw_mv)
{
    if (raw_mv <= 0) {
        return 0;
    }
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    const unsigned scale = (cfg != nullptr && cfg->battery_cal_milli != 0u)
                               ? cfg->battery_cal_milli
                               : 1000u;
    return static_cast<int>((static_cast<long>(raw_mv) * static_cast<long>(scale) + 500L) / 1000L);
}

int readBatteryMv()
{
    return applyBatteryCalibration(readBatteryRawMv());
}

//================================ UI build ===================================

lv_obj_t *makeBar(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kWidth, height);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBar), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    return label;
}

void resetCenterWidgets()
{
    s_lbl_caption = nullptr;
    s_lbl_callsign = nullptr;
    s_lbl_ssid = nullptr;
    s_lbl_time = nullptr;
    s_lbl_hint = nullptr;
    s_lbl_ota = nullptr;
    s_bar_ota = nullptr;
    s_shown_callsign[0] = '\0';
    s_shown_ssid[0] = '\0';
    s_shown_time[0] = '\0';
    s_shown_ota[0] = '\0';
    s_shown_state = -1;
    s_shown_media = false;
#if NRL_BOARD == NRL_BOARD_BI4UMD
    s_lbl_signaling = nullptr;
    s_shown_signaling[0] = '\0';
    s_lbl_music_title = nullptr;
    s_lbl_music_artist = nullptr;
    s_lbl_music_state = nullptr;
    s_lbl_music_format = nullptr;
    s_lbl_music_source = nullptr;
    s_list_music = nullptr;
    s_btn_music_play_label = nullptr;
    s_btn_music_repeat_label = nullptr;
    s_lbl_settings_mic = nullptr;
    s_lbl_settings_volume = nullptr;
#endif
}

void resetHomeWidgets()
{
    resetCenterWidgets();
    s_content = nullptr;
    s_lbl_wifi = nullptr;
    s_lbl_vol = nullptr;
    s_lbl_batt = nullptr;
    s_lbl_ip = nullptr;
    s_lbl_cpu = nullptr;
    s_lbl_gps = nullptr;
    s_lbl_provision_ip = nullptr;
    s_lbl_provision_ssid = nullptr;
    s_lbl_provision_ble = nullptr;
    s_shown_wifi[0] = '\0';
    s_shown_vol[0] = '\0';
    s_shown_batt[0] = '\0';
    s_shown_ip[0] = '\0';
    s_shown_cpu[0] = '\0';
    s_shown_gps[0] = '\0';
}

lv_obj_t *prepareScreen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    resetHomeWidgets();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

// Replace only the 240x170 centre area. The top status bar and bottom IP bar
// remain alive and keep updating while the physical-button menu is open.
lv_obj_t *prepareContent()
{
    if (s_content == nullptr) {
        s_content = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_content);
        lv_obj_set_pos(s_content, 0, kContentY);
        lv_obj_set_size(s_content, kWidth, kContentHeight);
        lv_obj_set_style_bg_color(s_content, lv_color_hex(kColorBg), 0);
        lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
#if NRL_BOARD == NRL_BOARD_BI4UMD
        lv_obj_add_flag(s_content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_content, menuTouchPressed, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(s_content, menuTouchReleased, LV_EVENT_RELEASED, nullptr);
#endif
    } else {
        lv_obj_clean(s_content);
    }
    resetCenterWidgets();
    return s_content;
}

#if NRL_BOARD == NRL_BOARD_BI4UMD
struct MenuRowInfo { int y; int item_index; };
constexpr size_t kMaxMenuRows = 12;
MenuRowInfo s_menu_rows[kMaxMenuRows];
size_t s_menu_row_count = 0;
int s_menu_touch_start_y = 0;
bool s_menu_touch_active = false;

void menuTouchPressed(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_menu_touch_start_y = p.y;
    s_menu_touch_active = true;
}

void menuTouchReleased(lv_event_t *)
{
    if (!s_menu_touch_active) return;
    s_menu_touch_active = false;
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int delta_y = p.y - s_menu_touch_start_y;
    if (delta_y > 24 || delta_y < -24) {
        const int distance = delta_y > 0 ? delta_y : -delta_y;
        const size_t item_count = menuItemCount();
        int steps = distance / 28;
        if (steps < 1) steps = 1;
        if (item_count > 1u && steps >= static_cast<int>(item_count)) {
            steps = static_cast<int>(item_count - 1u);
        }
        for (int i = 0; i < steps; ++i) {
            Display_MenuNavigate(delta_y > 0 ? 1 : -1);
        }
    } else {
        const int rel_y = p.y - kContentY;
        for (size_t i = 0; i < s_menu_row_count; ++i) {
            if (rel_y >= s_menu_rows[i].y && rel_y < s_menu_rows[i].y + 22) {
                s_menu_index = static_cast<size_t>(s_menu_rows[i].item_index);
                Display_MenuConfirm();
                break;
            }
        }
    }
}

void bi4umdScrollableMenuItemClicked(lv_event_t *event)
{
    s_menu_index = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    Display_MenuConfirm();
}

void buildBi4umdScrollableMenu(lv_obj_t *parent, const char *const *items,
                               const size_t item_count)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, kWidth, 170);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorAccent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    lv_obj_t *selected_row = nullptr;
    for (size_t i = 0; i < item_count; ++i) {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_pos(row, 4, 2 + static_cast<int>(i) * 28);
        lv_obj_set_size(row, kWidth - 12, 25);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(s_menu_index == i ? 0x17364A : kColorBg), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x087A82), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_outline_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, bi4umdScrollableMenuItemClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));

        lv_obj_t *label = makeLabel(row, &s_font_aprs_16,
                                    s_menu_index == i ? kColorCallIdle : kColorSub);
        lv_obj_set_width(label, kWidth - 28);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 5, 0);
        lv_label_set_text(label, items[i]);
        if (s_menu_index == i) selected_row = row;
    }
    lv_obj_update_layout(list);
    if (selected_row != nullptr) lv_obj_scroll_to_view(selected_row, LV_ANIM_OFF);
}
#endif

void menuRow(lv_obj_t *scr, int y, const char *text, bool selected,
             const lv_font_t *override_font = nullptr, int item_index = -1)
{
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_size(row, kWidth - 8, 22);
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x17364A : kColorBg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(selected ? kColorAccent : kColorBg), 0);
    lv_obj_set_style_border_width(row, selected ? 1 : 0, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
#if NRL_BOARD == NRL_BOARD_BI4UMD
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (item_index >= 0 && s_menu_row_count < kMaxMenuRows) {
        s_menu_rows[s_menu_row_count].y = y;
        s_menu_rows[s_menu_row_count].item_index = item_index;
        ++s_menu_row_count;
    }
#endif

    const lv_font_t *font = override_font != nullptr
                                ? override_font
                                : menuFont(&lv_font_montserrat_16);
    lv_obj_t *lbl = makeLabel(row, font,
                              selected ? kColorCallIdle : kColorSub);
    lv_obj_set_size(lbl, kWidth - 24,
                    lv_font_get_line_height(font));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 7, 0);
    char line[64] = {};
    snprintf(line, sizeof(line), "%s%s", selected ? "> " : "  ", text);
    lv_label_set_text(lbl, line);
}

void menuFooter(lv_obj_t *scr, const char *text, uint32_t color = kColorCaption)
{
    lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_16), color);
    lv_obj_set_width(lbl, kWidth - 8);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl, text);
}

void setMenuMessage(const char *text, uint32_t duration_ms = 3000u)
{
    snprintf(s_menu_message, sizeof(s_menu_message), "%s", text ? text : "");
    s_menu_message_until_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL) + duration_ms;
}

void menuStatusFooter(lv_obj_t *scr, const char *default_text)
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (s_menu_message[0] != '\0' && static_cast<int32_t>(s_menu_message_until_ms - now) > 0) {
        menuFooter(scr, s_menu_message, kColorApWarn);
    } else {
        menuFooter(scr, default_text);
    }
}

size_t menuWindowStart(const size_t item_count, const size_t visible_count)
{
    if (item_count <= visible_count || s_menu_index < visible_count) return 0u;
    size_t start = s_menu_index - visible_count + 1u;
    if (start + visible_count > item_count) start = item_count - visible_count;
    return start;
}

void menuScrollBar(lv_obj_t *scr, const size_t item_count,
                   const size_t visible_count, const size_t start)
{
    if (item_count <= visible_count) return;

    constexpr int kTrackHeight = 142;
    lv_obj_t *track = lv_obj_create(scr);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, kWidth - 4, 2);
    lv_obj_set_size(track, 3, kTrackHeight);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x21384A), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    const int thumb_height = static_cast<int>(kTrackHeight * visible_count / item_count);
    const size_t max_start = item_count - visible_count;
    const int thumb_y = static_cast<int>((kTrackHeight - thumb_height) * start / max_start);
    lv_obj_t *thumb = lv_obj_create(track);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_pos(thumb, 0, thumb_y);
    lv_obj_set_size(thumb, 3, thumb_height);
    lv_obj_set_style_bg_color(thumb, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb, 2, 0);
}

void buildMainMenu()
{
    lv_obj_t *scr = prepareContent();
    char ptt[32] = {};
    char nrl_codec[32] = {};
    char now_codec[32] = {};
    snprintf(ptt, sizeof(ptt), menuText("PTT: %s", "PTT模式: %s"),
             ESPNOW_LINK_GetPttMode() == 1u ? "ESP-NOW" : "NRL");
    snprintf(nrl_codec, sizeof(nrl_codec), menuText("NRL CODEC: %s", "NRL编码: %s"),
             NRLAudioBridge_GetVoiceCodec() == 1u ? "OPUS" : "G711");
    snprintf(now_codec, sizeof(now_codec), menuText("NOW CODEC: %s", "NOW编码: %s"),
             ESPNOW_LINK_GetTxCodec() == 1u ? "OPUS" : "G711");
    const char *items[] = {
        menuText("< BACK", "< 返回"),
        ptt,
        nrl_codec,
        now_codec,
        menuText("SIGNALING", "信令设置"),
        menuText("CHECK UPDATE", "检查更新"),
        "APRS",
        menuText("LANGUAGE", "语言"),
        menuText("ABOUT", "关于"),
    };
    constexpr size_t kItemCount = sizeof(items) / sizeof(items[0]);
#if NRL_BOARD == NRL_BOARD_BI4UMD
    buildBi4umdScrollableMenu(scr, items, kItemCount);
#else
#if NRL_BOARD == NRL_BOARD_GEZIPAI
    constexpr size_t kVisibleRows = 6u;
#else
    constexpr size_t kVisibleRows = 7u;
#endif
    const size_t start = menuWindowStart(kItemCount, kVisibleRows);
    const size_t end = (start + kVisibleRows < kItemCount) ? start + kVisibleRows : kItemCount;
    for (size_t i = start; i < end; ++i) {
        menuRow(scr, 1 + static_cast<int>(i - start) * 24, items[i], s_menu_index == i,
                nullptr, static_cast<int>(i));
    }
#if NRL_BOARD == NRL_BOARD_GEZIPAI || NRL_BOARD == NRL_BOARD_BI4UMD
    menuScrollBar(scr, kItemCount, kVisibleRows, start);
#endif
#endif
    menuStatusFooter(scr, menuText("VOL+/- SELECT   PTT OK", "音量+/- 选择  PTT确认"));
}

void buildLanguageMenu()
{
    lv_obj_t *scr = prepareContent();
    const char *items[] = {
        menuText("< BACK / LANGUAGE", "< 返回 / 语言"),
        s_menu_chinese ? "> 中文" : "  中文",
        !s_menu_chinese ? "> English" : "  English",
    };
    for (size_t i = 0; i < 3u; ++i) {
        menuRow(scr, 1 + static_cast<int>(i) * 28, items[i], s_menu_index == i,
                i == 1u ? &s_font_aprs_16 : nullptr, static_cast<int>(i));
    }
    menuStatusFooter(scr, menuText("PTT SELECT", "PTT选择"));
}

void buildAboutMenu()
{
    lv_obj_t *scr = prepareContent();
    menuRow(scr, 1, menuText("< BACK", "< 返回"), true, nullptr, 0);

    lv_obj_t *name = makeLabel(scr, &lv_font_montserrat_20, kColorAccent);
    lv_obj_set_width(name, kWidth);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 38);
    lv_label_set_text(name, NRL_FIRMWARE_NAME);

    char version_text[48] = {};
    snprintf(version_text, sizeof(version_text), menuText("VERSION %s", "版本 %s"), NRL_FIRMWARE_VERSION);
    lv_obj_t *version = makeLabel(scr, menuFont(&lv_font_montserrat_20), kColorCallIdle);
    lv_obj_set_width(version, kWidth);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(version, LV_ALIGN_TOP_MID, 0, 72);
    lv_label_set_text(version, version_text);

    lv_obj_t *board = makeLabel(scr, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(board, kWidth);
    lv_obj_set_style_text_align(board, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(board, LV_ALIGN_TOP_MID, 0, 108);
    lv_label_set_text(board, "GEZIPAI");
    menuFooter(scr, menuText("PTT BACK", "PTT返回"));
}

void buildOtaMenu()
{
    lv_obj_t *scr = prepareContent();
    const NrlOtaStatus *ota = otaUiSnapshot();
    if (ota == nullptr) {
        s_menu_index = 0u;
        menuRow(scr, 1, menuText("< BACK / OTA", "< 返回 / OTA"), true, nullptr, 0);
        lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_16), kColorWeak);
        lv_obj_set_width(lbl, kWidth - 20);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 64);
        lv_label_set_text(lbl, menuText("OTA STATUS UNAVAILABLE", "OTA状态不可用"));
        menuFooter(scr, menuText("PTT BACK", "PTT返回"));
        return;
    }
    if (s_menu_index > ota->release_count) s_menu_index = 0u;
#if NRL_BOARD == NRL_BOARD_BI4UMD
    const bool scroll_releases = !ota->checking && !s_menu_ota_requested &&
                                 !ota->updating && ota->release_count > 0u;
    if (!scroll_releases) {
        menuRow(scr, 1, menuText("< BACK / OTA", "< 返回 / OTA"),
                s_menu_index == 0u, nullptr, 0);
    }
#else
    menuRow(scr, 1, menuText("< BACK / OTA", "< 返回 / OTA"), s_menu_index == 0u, nullptr, 0);
#endif

    if (ota->checking || s_menu_ota_requested) {
        lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_20), kColorApWarn);
        lv_obj_set_width(lbl, kWidth);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 64);
        lv_label_set_text(lbl, ota->checking
            ? menuText("CHECKING...", "正在检查...")
            : menuText("CHECK REQUESTED...", "已请求检查..."));
    } else if (ota->updating) {
        lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_20), kColorTx);
        lv_obj_set_width(lbl, kWidth);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 54);
        char progress[32] = {};
        if (ota->update_size > 0u) {
            snprintf(progress, sizeof(progress), menuText("INSTALLING %u%%", "正在安装 %u%%"),
                     static_cast<unsigned>(ota->update_percent));
        } else {
            snprintf(progress, sizeof(progress), "%s", menuText("INSTALLING...", "正在安装..."));
        }
        lv_label_set_text(lbl, progress);
        if (ota->update_size > 0u) {
            lv_obj_t *bar = lv_bar_create(scr);
            lv_obj_set_pos(bar, 20, 94);
            lv_obj_set_size(bar, kWidth - 40, 10);
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, ota->update_percent, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTx), LV_PART_INDICATOR);
        }
    } else if (ota->release_count == 0u) {
        lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_16), kColorSub);
        lv_obj_set_width(lbl, kWidth - 20);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 58);
        lv_label_set_text(lbl, ota->last_error[0] ? ota->last_error
                                                  : menuText("NO VERSIONS FOUND", "未找到版本"));
    } else {
#if NRL_BOARD == NRL_BOARD_BI4UMD
        char version_lines[NRL_OTA_RELEASE_MAX][48] = {};
        const char *items[NRL_OTA_RELEASE_MAX + 1u] = {};
        items[0] = menuText("< BACK / OTA", "< 返回 / OTA");
        for (size_t i = 0; i < ota->release_count; ++i) {
            snprintf(version_lines[i], sizeof(version_lines[i]), "%.36s%s",
                     ota->releases[i].version,
                     strcmp(ota->releases[i].version, NRL_FIRMWARE_VERSION) == 0
                         ? menuText("  CURRENT", "  当前")
                         : "");
            items[i + 1u] = version_lines[i];
        }
        buildBi4umdScrollableMenu(scr, items, ota->release_count + 1u);
#else
        constexpr size_t kVisibleVersions = 4u;
        size_t selected_release = s_menu_index > 0u ? s_menu_index - 1u : 0u;
        size_t start = 0u;
        if (selected_release >= kVisibleVersions) start = selected_release - kVisibleVersions + 1u;
        if (start + kVisibleVersions > ota->release_count && ota->release_count > kVisibleVersions) {
            start = ota->release_count - kVisibleVersions;
        }
        const size_t end = (start + kVisibleVersions < ota->release_count)
                               ? start + kVisibleVersions
                               : ota->release_count;
        for (size_t i = start; i < end; ++i) {
            char version[48] = {};
            snprintf(version, sizeof(version), "%.36s%s",
                     ota->releases[i].version,
                     strcmp(ota->releases[i].version, NRL_FIRMWARE_VERSION) == 0
                         ? menuText("  CURRENT", "  当前")
                         : "");
            menuRow(scr, 29 + static_cast<int>(i - start) * 27, version,
                    s_menu_index == i + 1u, nullptr, static_cast<int>(i + 1u));
        }
#endif
    }

    if (ota->last_error[0] != '\0' && ota->release_count > 0u) {
        menuFooter(scr, ota->last_error, kColorWeak);
    } else if (s_menu_message[0] != '\0') {
        menuFooter(scr, s_menu_message, kColorApWarn);
    } else if (ota->release_count > 0u) {
        menuFooter(scr, menuText("SELECT VERSION   PTT INSTALL", "选择版本  PTT安装"));
    } else {
        menuFooter(scr, menuText("PTT BACK", "PTT返回"));
    }
}

void buildAprsMenu()
{
    lv_obj_t *scr = prepareContent();
    const char *items[] = {
        menuText("< BACK / APRS", "< 返回 / APRS"),
        menuText("APRS SETTINGS", "APRS设置"),
        menuText("STATION LIST", "电台列表"),
        menuText("GPS LIVE INFO", "GPS实时信息"),
        menuText("SEND BEACON NOW", "立即发送信标"),
    };
    for (size_t i = 0; i < 5u; ++i) {
        menuRow(scr, 1 + static_cast<int>(i) * 28, items[i], s_menu_index == i,
                nullptr, static_cast<int>(i));
    }

    AprsConfig cfg{};
    APRS_SERVICE_GetConfig(&cfg);
    char status[64];
    snprintf(status, sizeof(status), "%s  IS:%s  RX:%lu TX:%lu",
             cfg.enabled ? "ON" : "OFF",
             APRS_SERVICE_IsNetConnected() ? "UP" : "DOWN",
             static_cast<unsigned long>(APRS_SERVICE_GetRxCount()),
             static_cast<unsigned long>(APRS_SERVICE_GetTxCount()));
    menuStatusFooter(scr, status);
}

void buildAprsSettingsMenu()
{
    lv_obj_t *scr = prepareContent();
    AprsConfig cfg{};
    APRS_SERVICE_GetConfig(&cfg);
    constexpr size_t kItemCount = 9u;
#if NRL_BOARD != NRL_BOARD_BI4UMD
    constexpr size_t kVisibleRows = 5u;
    const size_t start = menuWindowStart(kItemCount, kVisibleRows);
    const size_t end = (start + kVisibleRows < kItemCount) ? start + kVisibleRows : kItemCount;
#endif
    const char *names[] = {
        menuText("MASTER", "总开关"), "APRS-IS", menuText("RF TX", "射频发送"),
        menuText("RF RX", "射频接收"), menuText("AUTO PERIOD", "自动周期"),
        menuText("FIXED POS", "固定位置")};
    const bool values[] = {cfg.enabled, cfg.net_enabled, cfg.rf_tx_enabled,
                           cfg.rf_rx_enabled, cfg.auto_interval,
                           cfg.fixed_beacon_without_gps};
#if NRL_BOARD == NRL_BOARD_BI4UMD
    char lines[kItemCount][48] = {};
    const char *items[kItemCount] = {};
    for (size_t item = 0; item < kItemCount; ++item) {
        if (item == 0u) {
            snprintf(lines[item], sizeof(lines[item]), "%s",
                     menuText("< BACK / APRS SET", "< 返回 / APRS设置"));
        } else if (item <= 6u) {
            snprintf(lines[item], sizeof(lines[item]), "%s: %s", names[item - 1u],
                     values[item - 1u] ? menuText("ON", "开") : menuText("OFF", "关"));
        } else if (item == 7u) {
            snprintf(lines[item], sizeof(lines[item]),
                     menuText("PERIOD: %us", "周期: %u秒"),
                     static_cast<unsigned>(cfg.beacon_interval_s));
        } else {
            snprintf(lines[item], sizeof(lines[item]), "SSID: %u",
                     static_cast<unsigned>(cfg.ssid));
        }
        items[item] = lines[item];
    }
    buildBi4umdScrollableMenu(scr, items, kItemCount);
#else
    for (size_t item = start; item < end; ++item) {
        char line[40];
        if (item == 0u) {
            snprintf(line, sizeof(line), "%s", menuText("< BACK / APRS SET", "< 返回 / APRS设置"));
        } else if (item <= 6u) {
            snprintf(line, sizeof(line), "%s: %s", names[item - 1u],
                     values[item - 1u] ? menuText("ON", "开") : menuText("OFF", "关"));
        } else if (item == 7u) {
            snprintf(line, sizeof(line), menuText("PERIOD: %us", "周期: %u秒"),
                     static_cast<unsigned>(cfg.beacon_interval_s));
        } else {
            snprintf(line, sizeof(line), "SSID: %u", static_cast<unsigned>(cfg.ssid));
        }
        menuRow(scr, 1 + static_cast<int>(item - start) * 28, line,
                s_menu_index == item, nullptr, static_cast<int>(item));
    }
#endif
    char footer[40];
    snprintf(footer, sizeof(footer), menuText("ITEM %u/%u  PTT TOGGLE/NEXT",
                                              "项目 %u/%u  PTT切换"),
             static_cast<unsigned>(s_menu_index + 1u),
             static_cast<unsigned>(kItemCount));
    menuStatusFooter(scr, footer);
}

// Dedicated APRS station page: BACK row plus the most recent stations heard,
// one compact line per station (callsign, distance, source, age).
void buildAprsListMenu()
{
    lv_obj_t *scr = prepareContent();
    menuRow(scr, 1, menuText("< BACK / STATIONS", "< 返回 / 电台列表"), true, nullptr, 0);

    AprsStationInfo stations[8];
    const size_t count = APRS_SERVICE_GetStations(stations, 8);
    if (!APRS_SERVICE_IsEnabled()) {
        lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_14), kColorSub);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 60);
        lv_label_set_text(lbl, menuText("APRS OFF (web/AT+APRS=ON)", "APRS未开启"));
    } else if (count == 0u) {
        lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_14), kColorSub);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 60);
        lv_label_set_text(lbl, menuText("NO STATIONS HEARD YET", "尚未收到电台"));
    }
    for (size_t i = 0; i < count; ++i) {
        const AprsStationInfo &s = stations[i];
        char dist[16] = "--";
        if (!isnan(s.distance_km)) {
            snprintf(dist, sizeof(dist), "%.1fkm", static_cast<double>(s.distance_km));
        }
        char age[12];
        if (s.age_s < 60u) snprintf(age, sizeof(age), "%lus", static_cast<unsigned long>(s.age_s));
        else snprintf(age, sizeof(age), "%lum", static_cast<unsigned long>(s.age_s / 60u));
        char line[64];
        snprintf(line, sizeof(line), "%s %s %s %s",
                 s.name, dist, s.via_rf ? "RF" : "IS", age);
        menuRow(scr, 25 + static_cast<int>(i) * 22, line, false);
    }
    menuFooter(scr, menuText("PTT BACK", "PTT返回"));
}

void gpsInfoLine(lv_obj_t *scr, const int y, const char *text, const uint32_t color)
{
    lv_obj_t *lbl = makeLabel(scr, menuFont(&lv_font_montserrat_14), color);
    lv_obj_set_width(lbl, kWidth - 10);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 5, y);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl, text);
}

void buildGpsInfoMenu()
{
    lv_obj_t *scr = prepareContent();
    menuRow(scr, 1, menuText("< BACK / GPS", "< 返回 / GPS"), true, nullptr, 0);

    AprsGpsInfo gps{};
    APRS_SERVICE_GetGpsInfo(&gps);

    char line[80];
    snprintf(line, sizeof(line), menuText("UART:%s  NMEA:%s", "串口:%s  NMEA:%s"),
             gps.uart_enabled ? menuText("ON", "开") : menuText("OFF", "关"),
             gps.connected ? menuText("OK", "正常") : "--");
    gpsInfoLine(scr, 25, line, gps.connected ? kColorGood : kColorApWarn);

    char sat_used[8] = "--";
    char sat_visible[8] = "--";
    if (gps.satellites >= 0) {
        snprintf(sat_used, sizeof(sat_used), "%d", static_cast<int>(gps.satellites));
    }
    if (gps.visible_satellites >= 0) {
        snprintf(sat_visible, sizeof(sat_visible), "%d",
                 static_cast<int>(gps.visible_satellites));
    }
    char gsv_age[16] = "--";
    if (gps.gsv_age_ms != UINT32_MAX) {
        snprintf(gsv_age, sizeof(gsv_age), "%.1fs",
                 static_cast<double>(gps.gsv_age_ms) / 1000.0);
    }
    snprintf(line, sizeof(line), menuText("FIX:%s Q:%u SAT:%s/%s", "定位:%s 质量:%u 卫星:%s/%s"),
             gps.has_fix ? menuText("YES", "是") : menuText("NO", "否"),
             static_cast<unsigned>(gps.fix_quality), sat_used, sat_visible);
    gpsInfoLine(scr, 43, line, gps.has_fix ? kColorGood : kColorApWarn);

    if (!isnan(gps.hdop)) {
        snprintf(line, sizeof(line), menuText("HDOP:%.1f  GSV:%s", "精度:%.1f  GSV:%s"),
                 static_cast<double>(gps.hdop), gsv_age);
    } else {
        snprintf(line, sizeof(line), menuText("HDOP:--  GSV:%s", "精度:--  GSV:%s"), gsv_age);
    }
    gpsInfoLine(scr, 61, line, kColorSub);

    if (gps.has_fix) {
        snprintf(line, sizeof(line), menuText("LAT: %.6f", "纬度: %.6f"), gps.latitude);
        gpsInfoLine(scr, 79, line, kColorCallIdle);
        snprintf(line, sizeof(line), menuText("LON: %.6f", "经度: %.6f"), gps.longitude);
        gpsInfoLine(scr, 97, line, kColorCallIdle);
    } else {
        gpsInfoLine(scr, 79, menuText("LAT: --", "纬度: --"), kColorWeak);
        gpsInfoLine(scr, 97, menuText("LON: --", "经度: --"), kColorWeak);
    }

    if (gps.has_fix && !isnan(gps.altitude_m)) {
        snprintf(line, sizeof(line), menuText("ALT:%.1fm SPD:%.1fkm/h", "海拔:%.1fm 速度:%.1fkm/h"),
                 gps.altitude_m, static_cast<double>(gps.speed_kmh));
    } else {
        snprintf(line, sizeof(line), "%s", menuText("ALT:-- SPD:--", "海拔:-- 速度:--"));
    }
    gpsInfoLine(scr, 115, line, kColorSub);

    char nmea_age[16] = "--";
    if (gps.age_ms != UINT32_MAX) {
        snprintf(nmea_age, sizeof(nmea_age), "%.1fs",
                 static_cast<double>(gps.age_ms) / 1000.0);
    }
    if (gps.course_valid) {
        snprintf(line, sizeof(line), menuText("CRS:%u AGE:%s SIG:%u", "航向:%u 更新:%s 信号:%u"),
                 static_cast<unsigned>(gps.course_deg), nmea_age,
                 static_cast<unsigned>(gps.satellite_detail_count));
    } else {
        snprintf(line, sizeof(line), menuText("CRS:-- AGE:%s SIG:%u", "航向:-- 更新:%s 信号:%u"),
                 nmea_age, static_cast<unsigned>(gps.satellite_detail_count));
    }
    gpsInfoLine(scr, 133, line, kColorSub);
    menuFooter(scr, menuText("PTT BACK", "PTT返回"));
}

bool signalingRouteEnabled(const SignalingConfig &cfg, bool mdc, size_t index)
{
    if (mdc) {
        if (index == 0u) return cfg.mdc_rx_mic;
        if (index == 1u) return cfg.mdc_rx_nrl;
        if (index == 2u) return cfg.mdc_tx_nrl;
        return cfg.mdc_tx_speaker;
    }
    if (index == 0u) return cfg.dtmf_rx_mic;
    if (index == 1u) return cfg.dtmf_rx_nrl;
    if (index == 2u) return cfg.dtmf_tx_nrl;
    return cfg.dtmf_tx_speaker;
}

void buildSignalingMenu()
{
    lv_obj_t *scr = prepareContent();
    const char *items[] = {
        menuText("< BACK / SIGNAL", "< 返回 / 信令"),
        menuText("MDC1200 SETTINGS", "MDC1200设置"),
        menuText("DTMF SETTINGS", "DTMF设置"),
        menuText("CTCSS RX SETTINGS", "CTCSS接收设置")};
    for (size_t i = 0; i < 4u; ++i) menuRow(scr, 1 + static_cast<int>(i) * 28, items[i], s_menu_index == i, nullptr, static_cast<int>(i));
    menuStatusFooter(scr, menuText("VOL+/- SELECT   PTT OK", "音量+/- 选择  PTT确认"));
}

void buildCtcssMenu()
{
    lv_obj_t *scr = prepareContent();
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);
    menuRow(scr, 1, menuText("< BACK / CTCSS", "< 返回 / CTCSS"), s_menu_index == 0u, nullptr, 0);
    char line[40];
    snprintf(line, sizeof(line), menuText("MIC RX: %s", "麦克风接收: %s"),
             cfg.ctcss_rx_mic ? menuText("ON", "开") : menuText("OFF", "关"));
    menuRow(scr, 35, line, s_menu_index == 1u, nullptr, 1);
    snprintf(line, sizeof(line), menuText("NRL RX: %s", "NRL接收: %s"),
             cfg.ctcss_rx_nrl ? menuText("ON", "开") : menuText("OFF", "关"));
    menuRow(scr, 69, line, s_menu_index == 2u, nullptr, 2);
    menuStatusFooter(scr, menuText("PTT TOGGLE", "PTT切换"));
}

void buildProtocolMenu(bool mdc)
{
    lv_obj_t *scr = prepareContent();
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);
    menuRow(scr, 1, mdc
        ? menuText("< BACK / MDC1200", "< 返回 / MDC1200")
        : menuText("< BACK / DTMF", "< 返回 / DTMF"), s_menu_index == 0u, nullptr, 0);
    const char *names[] = {
        menuText("MIC RX", "麦克风接收"), menuText("NRL RX", "NRL接收"),
        menuText("NRL TX", "NRL发送"), menuText("SPEAKER TX", "扬声器发送")};
    for (size_t i = 0; i < 4u; ++i) {
        char line[40];
        snprintf(line, sizeof(line), "%s: %s", names[i], signalingRouteEnabled(cfg, mdc, i)
            ? menuText("ON", "开") : menuText("OFF", "关"));
        menuRow(scr, 29 + static_cast<int>(i) * 28, line, s_menu_index == i + 1u,
                nullptr, static_cast<int>(i + 1u));
    }
    char footer[48];
    if (mdc) {
        snprintf(footer, sizeof(footer), menuText("ID:%04X  PTT TOGGLE", "ID:%04X  PTT切换"),
                 static_cast<unsigned>(cfg.mdc_unit_id));
    } else {
        snprintf(footer, sizeof(footer), menuText("ID:%.16s  PTT TOGGLE", "ID:%.16s  PTT切换"), cfg.dtmf_digits);
    }
    menuStatusFooter(scr, footer);
}

void buildMenuUi()
{
#if NRL_BOARD == NRL_BOARD_BI4UMD
    s_menu_row_count = 0;
#endif
    if (s_menu_page == MenuPage::Language) buildLanguageMenu();
    else if (s_menu_page == MenuPage::About) buildAboutMenu();
    else if (s_menu_page == MenuPage::Ota) buildOtaMenu();
    else if (s_menu_page == MenuPage::Aprs) buildAprsMenu();
    else if (s_menu_page == MenuPage::AprsSettings) buildAprsSettingsMenu();
    else if (s_menu_page == MenuPage::AprsList) buildAprsListMenu();
    else if (s_menu_page == MenuPage::AprsGps) buildGpsInfoMenu();
    else if (s_menu_page == MenuPage::Signaling) buildSignalingMenu();
    else if (s_menu_page == MenuPage::Ctcss) buildCtcssMenu();
    else if (s_menu_page == MenuPage::Mdc) buildProtocolMenu(true);
    else if (s_menu_page == MenuPage::Dtmf) buildProtocolMenu(false);
    else buildMainMenu();
#if NRL_BOARD == NRL_BOARD_BI4UMD
    addBi4umdMenuButtons();
#endif
}

#if NRL_DISPLAY_BUS_RGB
lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x1C2B3D), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t *makeTouchButton(lv_obj_t *parent, int x, int y, int w, int h,
                          const char *text, intptr_t id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x142033), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4E63), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x29445E), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_event_cb(btn, onTouchButton, LV_EVENT_CLICKED, reinterpret_cast<void *>(id));

    lv_obj_t *label = makeLabel(btn, &lv_font_montserrat_20, kColorCallIdle);
    lv_obj_center(label);
    lv_label_set_text(label, text);
    return btn;
}

void buildWideUi()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = makeBar(scr, 0, 52);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0A111B), 0);

    s_lbl_wifi = makeLabel(top, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, 22, 0);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI "  --");

    s_lbl_time = makeLabel(top, &lv_font_montserrat_28, kColorTime);
    lv_obj_center(s_lbl_time);
    lv_label_set_text(s_lbl_time, "--:--:--");

    s_lbl_vol = makeLabel(top, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_vol, LV_ALIGN_RIGHT_MID, -22, 0);
    lv_label_set_text(s_lbl_vol, LV_SYMBOL_VOLUME_MID " --");

    lv_obj_t *accent = lv_obj_create(scr);
    lv_obj_remove_style_all(accent);
    lv_obj_set_pos(accent, 0, 52);
    lv_obj_set_size(accent, kWidth, 2);
    lv_obj_set_style_bg_color(accent, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);

    lv_obj_t *left = makePanel(scr, 22, 76, 456, 260);
    s_lbl_caption = makeLabel(left, menuFont(&lv_font_montserrat_20), kColorCaption);
    lv_obj_align(s_lbl_caption, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_lbl_caption, menuText("STANDBY", "待机"));

    s_lbl_callsign = makeLabel(left, &lv_font_montserrat_48, kColorCallIdle);
    lv_obj_set_width(s_lbl_callsign, 428);
    lv_obj_set_style_text_align(s_lbl_callsign, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_TOP_LEFT, 0, 46);
    lv_label_set_text(s_lbl_callsign, "----");

    s_lbl_ssid = makeLabel(left, &lv_font_montserrat_28, kColorSub);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_TOP_LEFT, 2, 118);
    lv_label_set_text(s_lbl_ssid, "SSID -");

    s_lbl_hint = makeLabel(left, &lv_font_montserrat_16, kColorSub);
    lv_obj_set_width(s_lbl_hint, 428);
    lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(s_lbl_hint, "NRL voice bridge");

    lv_obj_t *right = makePanel(scr, 500, 76, 278, 260);
    lv_obj_t *net_title = makeLabel(right, &lv_font_montserrat_16, kColorCaption);
    lv_obj_align(net_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(net_title, "NETWORK");

    s_lbl_ip = makeLabel(right, &lv_font_montserrat_20, kColorIp);
    lv_obj_set_width(s_lbl_ip, 250);
    lv_obj_align(s_lbl_ip, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_ip, "---");

    lv_obj_t *audio_title = makeLabel(right, &lv_font_montserrat_16, kColorCaption);
    lv_obj_align(audio_title, LV_ALIGN_TOP_LEFT, 0, 104);
    lv_label_set_text(audio_title, "POWER");

    s_lbl_batt = makeLabel(right, &lv_font_montserrat_20, kColorSub);
    lv_obj_align(s_lbl_batt, LV_ALIGN_TOP_LEFT, 0, 138);
    lv_label_set_text(s_lbl_batt, "--  " LV_SYMBOL_BATTERY_EMPTY);

    makeTouchButton(scr, 22, 362, 180, 78, "VOL-", -1);
    makeTouchButton(scr, 222, 362, 356, 78, "CONFIG", 0);
    makeTouchButton(scr, 598, 362, 180, 78, "VOL+", 1);
}
#endif

#if NRL_BOARD == NRL_BOARD_BI4UMD
lv_obj_t *makeBi4umdMusicButton(lv_obj_t *parent, int x, const char *text,
                                lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, kContentHeight - 52);
    lv_obj_set_size(button, 38, 44);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x10212A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x087A82), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0x1C6B73), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = makeLabel(button, &lv_font_montserrat_16, kColorCallIdle);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

const char *bi4umdMusicBasename(const char *path)
{
    if (path == nullptr || path[0] == '\0') return "--";
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

void bi4umdMusicSelect(lv_event_t *event)
{
    const size_t index = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    const bool double_tap = s_music_tap_index == index &&
                            now - s_music_tap_ms <= 700u;
    if (!double_tap) {
        if (s_music_tap_row != nullptr && s_music_tap_row != row) {
            const int current = PLAYLIST_CurrentIndex();
            lv_obj_set_style_bg_color(
                s_music_tap_row,
                lv_color_hex(static_cast<int>(s_music_tap_index) == current ? 0x14505A : 0x101A24), 0);
        }
        s_music_tap_index = index;
        s_music_tap_ms = now;
        s_music_tap_row = row;
        lv_obj_set_style_bg_color(row, lv_color_hex(0x14505A), 0);
        return;
    }

    s_music_tap_index = SIZE_MAX;
    s_music_tap_ms = 0u;
    s_music_tap_row = nullptr;
    if (PLAYLIST_PlayIndex(index)) {
        rebuildBi4umdMusicList();
        refreshBi4umdMusic();
    }
}

void rebuildBi4umdMusicList()
{
    if (s_list_music == nullptr) return;
    s_music_tap_index = SIZE_MAX;
    s_music_tap_ms = 0u;
    s_music_tap_row = nullptr;
    lv_obj_clean(s_list_music);

    const size_t count = PLAYLIST_Count();
    const int current = PLAYLIST_CurrentIndex();
    if (count == 0u) {
        lv_obj_t *empty = makeLabel(s_list_music, &s_font_aprs_16, kColorSub);
        lv_obj_center(empty);
        lv_label_set_text(empty, "No music files");
        return;
    }

    const size_t row_count = count < kBi4umdMusicListMaxRows
                                 ? count : kBi4umdMusicListMaxRows;
    size_t start = 0u;
    if (current >= 0 && count > row_count) {
        start = static_cast<size_t>(current);
        if (start > row_count / 2u) start -= row_count / 2u;
        else start = 0u;
        if (start + row_count > count) start = count - row_count;
    }

    for (size_t row_index = 0; row_index < row_count; ++row_index) {
        const size_t i = start + row_index;
        const char *path = PLAYLIST_GetPath(i);
        lv_obj_t *row = lv_button_create(s_list_music);
        lv_obj_set_pos(row, 2, static_cast<int>(row_index * 32u));
        lv_obj_set_size(row, kWidth - 28, 29);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_color(row,
                                  lv_color_hex(static_cast<int>(i) == current ? 0x14505A : 0x101A24), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x087A82), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_event_cb(row, bi4umdMusicSelect, LV_EVENT_SHORT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));

        lv_obj_t *label = makeLabel(row, &s_font_aprs_16,
                                    static_cast<int>(i) == current ? kColorCallIdle : kColorSub);
        lv_obj_set_size(label, kWidth - 48, lv_font_get_line_height(&s_font_aprs_16));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
        lv_label_set_text(label, bi4umdMusicBasename(path));
    }
    if (count > row_count) {
        char text[64] = {};
        snprintf(text, sizeof(text), "Showing %u-%u of %u",
                 static_cast<unsigned>(start + 1u),
                 static_cast<unsigned>(start + row_count),
                 static_cast<unsigned>(count));
        lv_obj_t *more = makeLabel(s_list_music, &lv_font_montserrat_14, kColorSub);
        lv_obj_set_pos(more, 6, static_cast<int>(row_count * 32u + 4u));
        lv_label_set_text(more, text);
    }
}

void refreshBi4umdMusic()
{
    if (s_bi4umd_page != Bi4umdPage::Music || s_lbl_music_title == nullptr) return;

    const bool playing = MUSIC_IsPlaying();
    const char *path = MUSIC_CurrentPath();
    const MediaTrackInfo *track = MUSIC_GetTrackInfo();
    const bool changed = strncmp(s_shown_music_path, path, sizeof(s_shown_music_path)) != 0;
    if (changed || playing != s_shown_music_playing) {
        snprintf(s_shown_music_path, sizeof(s_shown_music_path), "%s", path);
        s_shown_music_playing = playing;

        const char *title = (track != nullptr && track->title[0] != '\0')
                                ? track->title : bi4umdMusicBasename(path);
        lv_label_set_text(s_lbl_music_title, title);
        lv_label_set_text(s_lbl_music_artist,
                          (track != nullptr && track->artist[0] != '\0') ? track->artist : "");
        lv_label_set_text(s_lbl_music_state, playing ? "PLAYING" : "STOPPED");
        lv_obj_set_style_text_color(s_lbl_music_state,
                                    lv_color_hex(playing ? kColorGood : kColorSub), 0);
        lv_label_set_text(s_btn_music_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

        char source[96] = {};
        if (bi4umdIsRadioPath(path)) {
            const int favorite = RADIO_FAV_IndexOfUrl(path);
            if (favorite >= 0) {
                (void)RADIO_FAV_Get(static_cast<size_t>(favorite), source, sizeof(source), nullptr, 0u);
            }
            if (source[0] == '\0') snprintf(source, sizeof(source), "Internet radio");
        } else if (path != nullptr && path[0] != '\0') {
            snprintf(source, sizeof(source), "%s", path);
        } else {
            snprintf(source, sizeof(source), "No track selected");
        }
        lv_label_set_text(s_lbl_music_source, source);
        rebuildBi4umdMusicList();
    }

    char format[40] = {};
    uint32_t rate = 0;
    uint8_t bits = 0;
    uint8_t channels = 0;
    if (playing && MUSIC_GetStreamInfo(&rate, &bits, &channels)) {
        snprintf(format, sizeof(format), "%lukHz  %ubit  %uch",
                 static_cast<unsigned long>(rate / 1000u),
                 static_cast<unsigned>(bits), static_cast<unsigned>(channels));
    }
    lv_label_set_text(s_lbl_music_format, format);
    lv_label_set_text(s_btn_music_repeat_label,
                      PLAYLIST_GetRepeatMode() == PLAYLIST_REPEAT_ONE ? "单" : "循");
}

void buildBi4umdMusicContent()
{
    lv_obj_t *content = prepareContent();

    lv_obj_t *brand = makeLabel(content, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 12, 7);
    const ExternalRadioConfig *config = EXTERNAL_RADIO_GetConfig();
    lv_label_set_text(brand, (config != nullptr && config->callsign[0] != '\0')
                            ? config->callsign : "NOCALL");
    lv_obj_t *heading = makeLabel(content, &lv_font_montserrat_20, kColorAccent);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 25);
    lv_label_set_text(heading, "Music Player");

    s_lbl_music_state = makeLabel(content, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(s_lbl_music_state, LV_ALIGN_TOP_RIGHT, -8, 9);
    lv_label_set_text(s_lbl_music_state, "STOPPED");

    s_lbl_music_title = makeLabel(content, &s_font_music_20, kColorCallIdle);
    lv_obj_set_width(s_lbl_music_title, kWidth - 24);
    lv_label_set_long_mode(s_lbl_music_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_lbl_music_title, LV_ALIGN_TOP_LEFT, 12, 58);
    lv_label_set_text(s_lbl_music_title, "--");

    s_lbl_music_artist = makeLabel(content, &s_font_aprs_16, kColorSub);
    lv_obj_add_flag(s_lbl_music_artist, LV_OBJ_FLAG_HIDDEN);

    s_lbl_music_format = makeLabel(content, &lv_font_montserrat_14, kColorAccent);
    lv_obj_align(s_lbl_music_format, LV_ALIGN_TOP_LEFT, 12, 86);

    s_lbl_music_source = makeLabel(content, &s_font_aprs_16, kColorCaption);
    lv_obj_set_width(s_lbl_music_source, kWidth - 24);
    lv_obj_set_style_text_align(s_lbl_music_source, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_lbl_music_source, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_lbl_music_source, LV_ALIGN_TOP_LEFT, 12, 108);

    auto make_upper_button = [content](int x, const char *text, lv_event_cb_t callback) {
        lv_obj_t *button = lv_button_create(content);
        lv_obj_set_pos(button, x, 136);
        lv_obj_set_size(button, 38, 44);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x10212A), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x087A82), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(button, lv_color_hex(0x1C6B73), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *label = makeLabel(button, &lv_font_montserrat_16, kColorCallIdle);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return button;
    };
    make_upper_button(31, LV_SYMBOL_PREV, bi4umdMusicPrev);
    lv_obj_t *play = make_upper_button(78, LV_SYMBOL_PLAY, bi4umdMusicToggle);
    s_btn_music_play_label = lv_obj_get_child(play, 0);
    make_upper_button(125, LV_SYMBOL_NEXT, bi4umdMusicNext);
    make_upper_button(172, LV_SYMBOL_REFRESH, bi4umdMusicRefresh);

    makeBi4umdMusicButton(content, 8, LV_SYMBOL_MINUS, bi4umdMusicVolumeDown);
    makeBi4umdMusicButton(content, 55, LV_SYMBOL_PLUS, bi4umdMusicVolumeUp);
    lv_obj_t *repeat = makeBi4umdMusicButton(content, 102, "循", bi4umdMusicRepeat);
    s_btn_music_repeat_label = lv_obj_get_child(repeat, 0);
    lv_obj_set_style_text_font(s_btn_music_repeat_label, &s_font_aprs_16, 0);
    makeBi4umdMusicButton(content, 149, LV_SYMBOL_LIST, bi4umdShowMusicListPage);
    makeBi4umdMusicButton(content, 196, LV_SYMBOL_HOME, bi4umdShowRadioPage);

    s_shown_music_path[0] = '\1';
    refreshBi4umdMusic();
}

void buildBi4umdMusicListContent()
{
    lv_obj_t *content = prepareContent();

    lv_obj_t *back = lv_button_create(content);
    lv_obj_set_pos(back, 8, 6);
    lv_obj_set_size(back, 32, 30);
    lv_obj_set_style_radius(back, 5, 0);
    lv_obj_add_event_cb(back, bi4umdShowMusicPage, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_label = makeLabel(back, &lv_font_montserrat_16, kColorCallIdle);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    lv_obj_t *heading = makeLabel(content, &s_font_music_20, kColorAccent);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 50, 10);
    lv_label_set_text(heading, "Music List");

    char count_text[24] = {};
    snprintf(count_text, sizeof(count_text), "%u tracks",
             static_cast<unsigned>(PLAYLIST_Count()));
    lv_obj_t *count = makeLabel(content, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -8, 13);
    lv_label_set_text(count, count_text);

    s_list_music = lv_obj_create(content);
    lv_obj_set_pos(s_list_music, 8, 44);
    lv_obj_set_size(s_list_music, kWidth - 16, kContentHeight - 52);
    lv_obj_set_style_bg_color(s_list_music, lv_color_hex(0x0B121A), 0);
    lv_obj_set_style_border_color(s_list_music, lv_color_hex(0x1C4B52), 0);
    lv_obj_set_style_border_width(s_list_music, 1, 0);
    lv_obj_set_style_radius(s_list_music, 5, 0);
    lv_obj_set_style_pad_all(s_list_music, 3, 0);
    lv_obj_set_scroll_dir(s_list_music, LV_DIR_VER);
    rebuildBi4umdMusicList();
}

void buildBi4umdSettingsContent()
{
    lv_obj_t *content = prepareContent();

    auto nav_button = [content](int x, int width, const char *text, lv_event_cb_t callback) {
        lv_obj_t *button = lv_button_create(content);
        lv_obj_set_pos(button, x, 22);
        lv_obj_set_size(button, width, 40);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x10212A), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x087A82), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(button, lv_color_hex(0x1C6B73), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *label = makeLabel(button, &s_font_aprs_16, kColorCallIdle);
        lv_obj_set_width(label, width - 8);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    };
    nav_button(8, 72, menuText("MAIN MENU", "主菜单"), bi4umdOpenMainMenu);
    nav_button(86, 54, "GPS", bi4umdOpenGpsPage);
    nav_button(146, 86, menuText("APRS RX", "APRS接收"), bi4umdOpenAprsListPage);

    auto square_button = [content](int x, int y, const char *text, lv_event_cb_t callback) {
        lv_obj_t *button = lv_button_create(content);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, 40, 38);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x10212A), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x087A82), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(button, lv_color_hex(0x1C6B73), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *label = makeLabel(button, &lv_font_montserrat_16, kColorCallIdle);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    };

    lv_obj_t *mic_name = makeLabel(content, &s_font_aprs_16, kColorSub);
    lv_obj_set_pos(mic_name, 12, 88);
    lv_label_set_text(mic_name, menuText("MIC", "麦克风"));
    square_button(88, 78, LV_SYMBOL_MINUS, bi4umdSettingsMicDown);
    s_lbl_settings_mic = makeLabel(content, &lv_font_montserrat_16, kColorCallIdle);
    lv_obj_set_width(s_lbl_settings_mic, 44);
    lv_obj_set_style_text_align(s_lbl_settings_mic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_settings_mic, 132, 88);
    square_button(184, 78, LV_SYMBOL_PLUS, bi4umdSettingsMicUp);

    lv_obj_t *volume_name = makeLabel(content, &s_font_aprs_16, kColorSub);
    lv_obj_set_pos(volume_name, 12, 142);
    lv_label_set_text(volume_name, menuText("VOLUME", "音量"));
    square_button(88, 132, LV_SYMBOL_MINUS, bi4umdSettingsVolumeDown);
    s_lbl_settings_volume = makeLabel(content, &lv_font_montserrat_16, kColorCallIdle);
    lv_obj_set_width(s_lbl_settings_volume, 44);
    lv_obj_set_style_text_align(s_lbl_settings_volume, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_settings_volume, 132, 142);
    square_button(184, 132, LV_SYMBOL_PLUS, bi4umdSettingsVolumeUp);

    lv_obj_t *ptt = lv_button_create(content);
    lv_obj_remove_style_all(ptt);
    lv_obj_set_pos(ptt, 60, 194);
    lv_obj_set_size(ptt, 120, 44);
    lv_obj_set_style_radius(ptt, 6, 0);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(0x142033), 0);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(0x8B1E2D), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(ptt, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ptt, lv_color_hex(kColorTx), 0);
    lv_obj_set_style_border_color(ptt, lv_color_hex(0xFF3030), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ptt, 0, 0);
    lv_obj_set_style_outline_width(ptt, 0, 0);
    lv_obj_set_style_shadow_width(ptt, 0, 0);
    lv_obj_add_event_cb(ptt, bi4umdPttEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(ptt, bi4umdPttEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(ptt, bi4umdPttEvent, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_t *ptt_label = makeLabel(ptt, &lv_font_montserrat_20, kColorCallIdle);
    lv_label_set_text(ptt_label, "PTT");
    lv_obj_center(ptt_label);

    lv_obj_t *home = lv_button_create(content);
    lv_obj_set_pos(home, kWidth - 48, kContentHeight - 48);
    lv_obj_set_size(home, 40, 40);
    lv_obj_set_style_radius(home, 6, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x10212A), 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x087A82), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(home, lv_color_hex(0x1C6B73), 0);
    lv_obj_set_style_border_width(home, 1, 0);
    lv_obj_add_event_cb(home, bi4umdShowRadioPage, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *home_label = makeLabel(home, &lv_font_montserrat_16, kColorCallIdle);
    lv_label_set_text(home_label, LV_SYMBOL_HOME);
    lv_obj_center(home_label);

    refreshBi4umdSettingsValues();
}
#endif

void buildHomeContent()
{
    lv_obj_t *content = prepareContent();

    s_lbl_caption = makeLabel(content, menuFont(&lv_font_montserrat_14), kColorCaption);
    lv_obj_align(s_lbl_caption, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(s_lbl_caption, menuText("STANDBY", "待机"));

    s_lbl_callsign = makeLabel(content, &lv_font_montserrat_48, kColorCallIdle);
    lv_obj_set_width(s_lbl_callsign, kWidth);
    lv_obj_set_style_text_align(s_lbl_callsign, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_callsign, LV_ALIGN_TOP_MID, 0, 24);
    lv_label_set_text(s_lbl_callsign, "----");

    s_lbl_ssid = makeLabel(content, &lv_font_montserrat_20, kColorSub);
    lv_obj_set_width(s_lbl_ssid, kWidth - 16);
    lv_obj_set_style_text_align(s_lbl_ssid, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_ssid, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_lbl_ssid, LV_ALIGN_TOP_MID, 0, 74);
    lv_label_set_text(s_lbl_ssid, "SSID -");

    s_lbl_time = makeLabel(content, &lv_font_montserrat_28, kColorTime);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_MID, 0, 100);
    lv_label_set_text(s_lbl_time, "--:--:--");

    // Gezipai is not a touch screen: this is deliberately only a notification.
    // Upgrade confirmation is AT+OTA=LATEST, the local /update page, or the
    // physical VOL+ + VOL- chord handled by status_io.cpp.
#if NRL_BOARD == NRL_BOARD_BI4UMD
    // BI4UMD has an extra 80 vertical pixels. Keep decoded MDC/DTMF/CTCSS
    // signaling on its own row so it never displaces the APRS monitor.
    s_lbl_signaling = makeLabel(content, &s_font_aprs_16, kColorAccent);
    lv_obj_set_width(s_lbl_signaling, kWidth);
    lv_obj_set_style_text_align(s_lbl_signaling, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_signaling, LV_ALIGN_TOP_MID, 0, 136);
    lv_label_set_long_mode(s_lbl_signaling, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_lbl_signaling, "");
#endif

    s_lbl_ota = makeLabel(content, &s_font_aprs_16, kColorApWarn);
    lv_obj_set_width(s_lbl_ota, kWidth);
    lv_obj_set_style_text_align(s_lbl_ota, LV_TEXT_ALIGN_CENTER, 0);
#if NRL_BOARD == NRL_BOARD_BI4UMD
    lv_obj_align(s_lbl_ota, LV_ALIGN_TOP_MID, 0, 164);
#else
    lv_obj_align(s_lbl_ota, LV_ALIGN_TOP_MID, 0, 136);
#endif
    // Doubles as the APRS monitor ticker; long packet lines scroll circularly.
    lv_label_set_long_mode(s_lbl_ota, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_lbl_ota, "");

    s_bar_ota = lv_bar_create(content);
    lv_obj_set_pos(s_bar_ota, 16, kContentHeight - 9);
    lv_obj_set_size(s_bar_ota, kWidth - 32, 5);
    lv_bar_set_range(s_bar_ota, 0, 100);
    lv_bar_set_value(s_bar_ota, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_ota, lv_color_hex(kColorTx), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar_ota, LV_OBJ_FLAG_HIDDEN);

#if NRL_BOARD == NRL_BOARD_BI4UMD
    lv_obj_t *music = lv_button_create(content);
    lv_obj_set_pos(music, 8, kContentHeight - 48);
    lv_obj_set_size(music, 40, 40);
    lv_obj_set_style_radius(music, 6, 0);
    lv_obj_set_style_bg_color(music, lv_color_hex(0x10212A), 0);
    lv_obj_set_style_bg_color(music, lv_color_hex(0x087A82), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(music, lv_color_hex(0x1C6B73), 0);
    lv_obj_set_style_border_width(music, 1, 0);
    lv_obj_add_event_cb(music, bi4umdShowMusicPage, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *music_label = makeLabel(music, &lv_font_montserrat_16, kColorCallIdle);
    lv_label_set_text(music_label, LV_SYMBOL_AUDIO);
    lv_obj_center(music_label);

    lv_obj_t *settings = lv_button_create(content);
    lv_obj_set_pos(settings, kWidth - 48, kContentHeight - 48);
    lv_obj_set_size(settings, 40, 40);
    lv_obj_set_style_radius(settings, 6, 0);
    lv_obj_set_style_bg_color(settings, lv_color_hex(0x10212A), 0);
    lv_obj_set_style_bg_color(settings, lv_color_hex(0x087A82), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(settings, lv_color_hex(0x1C6B73), 0);
    lv_obj_set_style_border_width(settings, 1, 0);
    lv_obj_add_event_cb(settings, bi4umdShowSettingsPage, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *settings_label = makeLabel(settings, &lv_font_montserrat_16, kColorCallIdle);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(settings_label);

    lv_obj_t *ptt = lv_button_create(content);
    lv_obj_remove_style_all(ptt);
    lv_obj_set_pos(ptt, 60, kContentHeight - 55);
    lv_obj_set_size(ptt, kWidth - 120, 44);
    lv_obj_set_style_radius(ptt, 6, 0);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(0x142033), 0);
    lv_obj_set_style_bg_color(ptt, lv_color_hex(0x8B1E2D), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(ptt, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ptt, lv_color_hex(kColorTx), 0);
    lv_obj_set_style_border_color(ptt, lv_color_hex(0xFF3030), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ptt, 0, 0);
    lv_obj_set_style_outline_width(ptt, 0, 0);
    lv_obj_set_style_shadow_width(ptt, 0, 0);
    lv_obj_add_event_cb(ptt, bi4umdPttEvent, LV_EVENT_ALL, nullptr);
    lv_obj_t *label = makeLabel(ptt, &lv_font_montserrat_20, kColorCallIdle);
    lv_label_set_text(label, "PTT");
    lv_obj_center(label);
#endif
}

void buildUi()
{
    lv_obj_t *scr = prepareScreen();
#if NRL_DISPLAY_BUS_RGB
    buildWideUi();
    return;
#endif

    // ---- Top status bar ----
    lv_obj_t *top = makeBar(scr, 0, kStatusBarHeight);

    lv_obj_t *accent = lv_obj_create(scr);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, kWidth, 2);
    lv_obj_set_pos(accent, 0, 34);
    lv_obj_set_style_bg_color(accent, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);

    s_lbl_wifi = makeLabel(top, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI "  --");

    s_lbl_vol = makeLabel(top, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(s_lbl_vol, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_lbl_vol, LV_SYMBOL_VOLUME_MID " --");

    s_lbl_batt = makeLabel(top, &lv_font_montserrat_14, kColorSub);
    lv_obj_align(s_lbl_batt, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_label_set_text(s_lbl_batt, "--  " LV_SYMBOL_BATTERY_EMPTY);

    // ---- Centre content ----
    buildHomeContent();

    // ---- Bottom IP bar: address on the left, per-core CPU load on the right ----
    lv_obj_t *bottom = makeBar(scr, kHeight - kBottomBarHeight, kBottomBarHeight);
    s_lbl_ip = makeLabel(bottom, &lv_font_montserrat_16, kColorIp);
    lv_obj_set_width(s_lbl_ip, 124);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_DOT);
    lv_obj_align(s_lbl_ip, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_text(s_lbl_ip, "---");

    s_lbl_gps = makeLabel(bottom, &lv_font_montserrat_16, kColorWeak);
    lv_obj_align(s_lbl_gps, LV_ALIGN_CENTER, 38, 0);
    lv_label_set_text(s_lbl_gps, LV_SYMBOL_GPS);

    s_lbl_cpu = makeLabel(bottom, &lv_font_montserrat_16, kColorSub);
    lv_obj_align(s_lbl_cpu, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_label_set_text(s_lbl_cpu, "--/--");
}

//================================ Refresh ====================================

// Updates a label only when its text actually changed (avoids redraw churn).
bool setLabel(lv_obj_t *label, char *cache, size_t cache_size, const char *text)
{
    if (strncmp(cache, text, cache_size) == 0) {
        return false;
    }
    snprintf(cache, cache_size, "%.*s", static_cast<int>(cache_size - 1u), text);
    lv_label_set_text(label, text);
    return true;
}

void refreshCaller()
{
    char voice_call[8] = {};
    unsigned voice_ssid = 0u;
    const bool rx = NRLAudioBridge_GetRemoteCaller(voice_call, sizeof(voice_call), &voice_ssid);
    const bool tx = STATUS_IO_IsSqlActive();
    const bool espnow_mode = ESPNOW_LINK_GetPttMode() == 1u;
    const bool espnow_tx = espnow_mode && tx && ESPNOW_LINK_IsEnabled();
    const bool espnow_rx = ESPNOW_LINK_IsReceiving();
    char espnow_peer[16] = {};
    if (espnow_rx) {
        ESPNOW_LINK_GetLastPeer(espnow_peer, sizeof(espnow_peer));
    }

    char media_name[160] = {};
    const char *playing_path = MUSIC_CurrentPath();
    const bool media_playing = MUSIC_IsPlaying() && playing_path != nullptr;
    const bool radio_playing = media_playing &&
                               (strncmp(playing_path, "http://", 7) == 0 ||
                                strncmp(playing_path, "https://", 8) == 0);
    // Voice/PTT and ESP-NOW remain above music in the display priority, just
    // as they are in the audio path. When idle, show the configured favorite
    // name; a URL played directly from AT/Web has no friendly metadata yet.
    const bool show_radio = radio_playing && !rx && !tx && !espnow_rx && !espnow_tx;
    const bool show_music = media_playing && !radio_playing &&
                            !rx && !tx && !espnow_rx && !espnow_tx;
    if (show_radio) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        if (strcmp(s_cached_radio_path, playing_path) != 0 ||
            s_radio_name_refresh_ms == 0u ||
            (now_ms - s_radio_name_refresh_ms) >= 1000u) {
            snprintf(s_cached_radio_path, sizeof(s_cached_radio_path), "%s", playing_path);
            s_cached_radio_name[0] = '\0';
            const int favorite = RADIO_FAV_IndexOfUrl(playing_path);
            if (favorite >= 0) {
                (void)RADIO_FAV_Get(static_cast<size_t>(favorite), s_cached_radio_name,
                                    sizeof(s_cached_radio_name), nullptr, 0u);
            }
            if (s_cached_radio_name[0] == '\0') {
                snprintf(s_cached_radio_name, sizeof(s_cached_radio_name), "%s",
                         menuText("INTERNET RADIO", "网络电台"));
            }
            s_radio_name_refresh_ms = now_ms;
        }
        snprintf(media_name, sizeof(media_name), "%s", s_cached_radio_name);
    } else if (show_music) {
        const MediaTrackInfo *track = MUSIC_GetTrackInfo();
        if (track != nullptr && track->title[0] != '\0') {
            snprintf(media_name, sizeof(media_name), "%s", track->title);
        } else {
            const char *basename = strrchr(playing_path, '/');
            basename = (basename != nullptr) ? basename + 1 : playing_path;
            snprintf(media_name, sizeof(media_name), "%s", basename);
            char *extension = strrchr(media_name, '.');
            if (extension != nullptr && extension != media_name) {
                *extension = '\0';
            }
        }
    }

    // While a voice stream is actually being received, the main area shows the
    // remote caller. Otherwise it shows this device's own callsign/SSID, read
    // straight from the local config -- heartbeats never feed this.
    char call_text[16];
    char ssid_text[160];
    if (rx && voice_call[0] != '\0') {
        snprintf(call_text, sizeof(call_text), "%s", voice_call);
        snprintf(ssid_text, sizeof(ssid_text), "SSID %u", voice_ssid);
    } else if (espnow_rx && espnow_peer[0] != '\0') {
        // espnow_peer arrives as "CALLSIGN-N". This layout has a dedicated
        // SSID line below the callsign, so split the pair: the "-N" suffix in
        // the large callsign font both duplicates the SSID line and wraps.
        char *dash = strrchr(espnow_peer, '-');
        if (dash != nullptr) {
            *dash = '\0';
            snprintf(ssid_text, sizeof(ssid_text), "SSID %s", dash + 1);
        } else {
            snprintf(ssid_text, sizeof(ssid_text), "SSID -");
        }
        snprintf(call_text, sizeof(call_text), "%s", espnow_peer);
    } else if (show_radio) {
        snprintf(call_text, sizeof(call_text), "RADIO");
        snprintf(ssid_text, sizeof(ssid_text), "%s", media_name);
    } else if (show_music) {
        snprintf(call_text, sizeof(call_text), "MUSIC");
        snprintf(ssid_text, sizeof(ssid_text), "%s", media_name);
    } else {
        const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
        if (cfg != nullptr && cfg->callsign[0] != '\0') {
            snprintf(call_text, sizeof(call_text), "%s", cfg->callsign);
            snprintf(ssid_text, sizeof(ssid_text), "SSID %u",
                     static_cast<unsigned>(cfg->callsign_ssid));
        } else {
            snprintf(call_text, sizeof(call_text), "----");
            snprintf(ssid_text, sizeof(ssid_text), "SSID -");
        }
    }

    setLabel(s_lbl_callsign, s_shown_callsign, sizeof(s_shown_callsign), call_text);
    setLabel(s_lbl_ssid, s_shown_ssid, sizeof(s_shown_ssid), ssid_text);
    const bool show_media = show_radio || show_music;
    if (show_media != s_shown_media) {
        s_shown_media = show_media;
        // The 16 px UI font falls back to the bundled GB2312 glyphs, allowing
        // Chinese favorite names without adding another large Gezipai font.
        lv_obj_set_style_text_font(s_lbl_ssid,
                                   show_media ? &s_font_aprs_16 : &lv_font_montserrat_20,
                                   0);
    }

    // Status caption above the callsign. Transmitting while also receiving is
    // shown as full duplex; otherwise TX wins over RX wins over standby.
    int state;
    if (espnow_tx && espnow_rx) {
        state = 6;  // ESP-NOW full duplex
    } else if (espnow_tx) {
        state = 5;  // ESP-NOW transmit
    } else if (espnow_rx) {
        state = 7;  // ESP-NOW receive
    } else if (tx && rx) {
        state = 4;  // full duplex
    } else if (tx) {
        state = 3;  // transmitting
    } else if (rx) {
        state = 2;  // receiving
    } else if (show_media) {
        state = 11;  // network radio or music file
    } else if (ESPNOW_LINK_IsEnabled()) {
        // Standby captions carry the PTT target (NRL / ESP-NOW) and that
        // link's own TX codec. Both are folded into the state value so
        // flipping either switch while idle re-renders the caption.
        state = (ESPNOW_LINK_GetTxCodec() == 1u) ? 9 : 8;
    } else {
        state = (NRLAudioBridge_GetVoiceCodec() == 1u) ? 10 : 0;
    }

    if (state != s_shown_state) {
        s_shown_state = state;
        const char *caption;
        uint32_t caption_color;
        uint32_t call_color;
        switch (state) {
            case 11:
                caption = menuText("NOW PLAYING", "正在播放"); caption_color = kColorGood;
                call_color = kColorGood;
                break;
            case 7:
                caption = menuText("ESP-NOW RX", "ESP-NOW 接收"); caption_color = kColorDuplex;
                call_color = kColorCallLive;
                break;
            case 6:
                caption = menuText("ESP-NOW FDX", "ESP-NOW 全双工"); caption_color = kColorDuplex;
                call_color = kColorCallLive;
                break;
            case 5:
                caption = menuText("ESP-NOW TX", "ESP-NOW 发送"); caption_color = kColorDuplex;
                call_color = kColorCallIdle;
                break;
            case 4:
                caption = menuText("FULL DUPLEX", "全双工"); caption_color = kColorDuplex;
                call_color = kColorCallLive;
                break;
            case 3:
                caption = menuText("TRANSMITTING", "正在发送"); caption_color = kColorTx;
                call_color = kColorCallIdle;
                break;
            case 2:
                caption = menuText("RECEIVING", "正在接收"); caption_color = kColorAccent;
                call_color = kColorCallLive;
                break;
            case 9:
                caption = menuText("STANDBY ESP-NOW OPUS", "待机 ESP-NOW OPUS");
                caption_color = kColorCaption;
                call_color = kColorCallIdle;
                break;
            case 8:
                caption = menuText("STANDBY ESP-NOW G711", "待机 ESP-NOW G711");
                caption_color = kColorCaption;
                call_color = kColorCallIdle;
                break;
            case 10:
                caption = menuText("STANDBY NRL OPUS", "待机 NRL OPUS");
                caption_color = kColorCaption;
                call_color = kColorCallIdle;
                break;
            default:
                caption = menuText("STANDBY NRL G711", "待机 NRL G711");
                caption_color = kColorCaption;
                call_color = kColorCallIdle;
                break;
        }
        lv_label_set_text(s_lbl_caption, caption);
        lv_obj_set_style_text_color(s_lbl_caption, lv_color_hex(caption_color), 0);
        lv_obj_set_style_text_color(s_lbl_callsign, lv_color_hex(call_color), 0);
    }
}

void refreshClock()
{
    if (!s_time_sync_started && nrlWifiStaConnected()) {
        // CST-8 == UTC+8 (China). NTP servers are tried in order.
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "ntp.ntsc.ac.cn");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        s_time_sync_started = true;
    }

    char time_text[16];
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2024) {
        snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        snprintf(time_text, sizeof(time_text), "--:--:--");
    }
    setLabel(s_lbl_time, s_shown_time, sizeof(s_shown_time), time_text);
}

void refreshWifi()
{
    char wifi_text[28];
    uint32_t color = kColorSub;
    if (nrlWifiStaConnected()) {
        wifi_ap_record_t ap_info = {};
        const bool have_ap = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
        const int rssi = have_ap ? ap_info.rssi : 0;
        // Bare channel number after the RSSI (no "CH" prefix -- the narrow
        // gezipai top bar can't fit it). Shown because ESP-NOW peers only hear
        // each other on the same WiFi channel, which STA inherits from the AP.
        snprintf(wifi_text, sizeof(wifi_text), LV_SYMBOL_WIFI "  %ddB %u",
                 rssi, have_ap ? static_cast<unsigned>(ap_info.primary) : 0u);
        if (rssi >= -65) {
            color = kColorGood;
        } else if (rssi >= -78) {
            color = kColorApWarn;
        } else {
            color = kColorWeak;
        }
    } else {
        uint8_t channel = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        (void)esp_wifi_get_channel(&channel, &second);
        snprintf(wifi_text, sizeof(wifi_text), LV_SYMBOL_WIFI "  AP %u",
                 static_cast<unsigned>(channel));
        color = kColorApWarn;
    }
    if (setLabel(s_lbl_wifi, s_shown_wifi, sizeof(s_shown_wifi), wifi_text)) {
        lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(color), 0);
    }
}

void refreshVolume()
{
    char vol_text[16];
    uint32_t color = kColorSub;
    const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
    if (cfg != nullptr) {
        // line_out_volume is the ES8311 speaker volume, stored 0..255.
        const int pct = (static_cast<int>(cfg->line_out_volume) * 100 + 127) / 255;
        const char *symbol = LV_SYMBOL_VOLUME_MAX;
        if (pct == 0) {
            symbol = LV_SYMBOL_MUTE;
            color = kColorWeak;
        } else if (pct < 55) {
            symbol = LV_SYMBOL_VOLUME_MID;
        }
        snprintf(vol_text, sizeof(vol_text), "%s %d%%", symbol, pct);
    } else {
        snprintf(vol_text, sizeof(vol_text), LV_SYMBOL_VOLUME_MID " --");
    }
    if (setLabel(s_lbl_vol, s_shown_vol, sizeof(s_shown_vol), vol_text)) {
        lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(color), 0);
    }
}

void refreshBattery()
{
    char batt_text[20];
    uint32_t color = kColorSub;
    if (s_adc_ready && s_battery_mv > 0) {
        int pct = (s_battery_mv - kBatteryMinMv) * 100 / (kBatteryMaxMv - kBatteryMinMv);
        if (pct < 0) {
            pct = 0;
        } else if (pct > 100) {
            pct = 100;
        }
        const char *symbol = LV_SYMBOL_BATTERY_EMPTY;
        if (pct >= 80) {
            symbol = LV_SYMBOL_BATTERY_FULL;
        } else if (pct >= 55) {
            symbol = LV_SYMBOL_BATTERY_3;
        } else if (pct >= 30) {
            symbol = LV_SYMBOL_BATTERY_2;
        } else if (pct >= 12) {
            symbol = LV_SYMBOL_BATTERY_1;
        }
        snprintf(batt_text, sizeof(batt_text), "%d.%02dV  %s",
                 s_battery_mv / 1000, (s_battery_mv % 1000) / 10, symbol);
        color = (pct <= 15) ? kColorWeak : kColorSub;
    } else {
        snprintf(batt_text, sizeof(batt_text), "--  %s", LV_SYMBOL_BATTERY_EMPTY);
    }
    if (setLabel(s_lbl_batt, s_shown_batt, sizeof(s_shown_batt), batt_text)) {
        lv_obj_set_style_text_color(s_lbl_batt, lv_color_hex(color), 0);
    }
}

// Per-core CPU load ("c0/c1") from the FreeRTOS idle-task runtime counters,
// sampled on the 500 ms refresh tick. A 3% hysteresis keeps the label from
// redrawing on measurement jitter.
void refreshCpu()
{
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    if (s_lbl_cpu == nullptr) {
        return;
    }
    static uint32_t s_last_idle[2] = {};
    static uint32_t s_last_us = 0;
    static int s_pct_shown[2] = {-100, -100};
    const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t idle[2] = {
        static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(0)),
        static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(1)),
    };
    const uint32_t dt = now_us - s_last_us; // wrap-safe unsigned math
    if (s_last_us != 0u && dt > 0u) {
        int pct[2];
        for (int core = 0; core < 2; ++core) {
            const uint32_t di = idle[core] - s_last_idle[core];
            const uint64_t idle_pct = static_cast<uint64_t>(di) * 100u / dt;
            long load = 100 - static_cast<long>(idle_pct);
            if (load < 0) load = 0;
            if (load > 100) load = 100;
            pct[core] = static_cast<int>(load);
        }
        const int d0 = pct[0] - s_pct_shown[0];
        const int d1 = pct[1] - s_pct_shown[1];
        if (d0 >= 3 || d0 <= -3 || d1 >= 3 || d1 <= -3) {
            s_pct_shown[0] = pct[0];
            s_pct_shown[1] = pct[1];
            char text[16];
            snprintf(text, sizeof(text), "%d/%d", pct[0], pct[1]);
            if (setLabel(s_lbl_cpu, s_shown_cpu, sizeof(s_shown_cpu), text)) {
                lv_obj_set_style_text_color(
                    s_lbl_cpu,
                    lv_color_hex((pct[0] > 85 || pct[1] > 85) ? kColorWeak : kColorSub), 0);
            }
        }
    }
    s_last_us = now_us;
    s_last_idle[0] = idle[0];
    s_last_idle[1] = idle[1];
#endif
}

void buildProvisioningUi()
{
    lv_obj_t *scr = prepareScreen();

    lv_obj_t *title = makeLabel(scr, &s_font_aprs_16, kColorAccent);
    lv_obj_set_width(title, kWidth - 16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, 8, 14);
    lv_label_set_text(title, "WiFi / BLE 配网");

    lv_obj_t *state = makeLabel(scr, &s_font_aprs_16, kColorApWarn);
    lv_obj_set_width(state, kWidth - 16);
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(state, 8, 44);
    lv_label_set_text(state, "等待网络设置...");

    lv_obj_t *wifi = makeLabel(scr, &s_font_aprs_16, kColorCallIdle);
    lv_obj_set_width(wifi, kWidth - 24);
    lv_obj_set_pos(wifi, 12, 72);
    lv_label_set_text(wifi, "1. 连接 WiFi 热点");

    s_lbl_provision_ssid = makeLabel(scr, &lv_font_montserrat_14, kColorGood);
    lv_obj_set_width(s_lbl_provision_ssid, kWidth - 16);
    lv_obj_set_style_text_align(s_lbl_provision_ssid, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_provision_ssid, 8, 96);
    lv_label_set_text(s_lbl_provision_ssid, "NRL-ESP32-XXXXXX");

    s_lbl_provision_ip = makeLabel(scr, &lv_font_montserrat_16, kColorIp);
    lv_obj_set_width(s_lbl_provision_ip, kWidth - 24);
    lv_obj_set_pos(s_lbl_provision_ip, 12, 120);
    lv_label_set_text(s_lbl_provision_ip, "http://192.168.4.1");

    lv_obj_t *wechat = makeLabel(scr, &s_font_aprs_16, kColorCallIdle);
    lv_obj_set_width(wechat, kWidth - 24);
    lv_obj_set_pos(wechat, 12, 148);
    lv_label_set_text(wechat, "2. 微信小程序 NRL互联");

    lv_obj_t *ble = makeLabel(scr, &s_font_aprs_16, kColorSub);
    lv_obj_set_width(ble, kWidth - 24);
    lv_obj_set_pos(ble, 12, 174);
    lv_label_set_text(ble, "打开设置，使用蓝牙配网");

    s_lbl_provision_ble = makeLabel(scr, &lv_font_montserrat_14, kColorGood);
    lv_obj_set_width(s_lbl_provision_ble, kWidth - 16);
    lv_obj_set_style_text_align(s_lbl_provision_ble, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_provision_ble, 8, kHeight - 30);
    lv_label_set_text(s_lbl_provision_ble, "BLE CHECKING...");
}

void refreshProvisioningUi()
{
    if (s_lbl_provision_ip == nullptr || s_lbl_provision_ssid == nullptr ||
        s_lbl_provision_ble == nullptr) {
        return;
    }
    char ssid[32] = {};
    WifiConfigPortal_GetApSsid(ssid, sizeof(ssid));
    lv_label_set_text(s_lbl_provision_ssid, ssid);
    char ip[16] = "192.168.4.1";
    const uint32_t ap_ip = nrlWifiApIp();
    if (ap_ip != 0u) {
        nrlIpToString(ap_ip, ip, sizeof(ip));
    }
    char url[32] = {};
    snprintf(url, sizeof(url), "http://%s", ip);
    lv_label_set_text(s_lbl_provision_ip, url);
    const bool ble_ready = BLEConfig_IsReady();
    lv_label_set_text(s_lbl_provision_ble,
                      ble_ready ? "BLE READY: NRL-ESP32-CFG" : "BLE FAILED - USE WIFI");
    lv_obj_set_style_text_color(s_lbl_provision_ble,
                                lv_color_hex(ble_ready ? kColorGood : kColorWeak), 0);
}

void refreshGpsStatus()
{
    if (s_lbl_gps == nullptr) {
        return;
    }
    AprsGpsInfo gps{};
    APRS_SERVICE_GetGpsInfo(&gps);

    char text[sizeof(s_shown_gps)] = {};
    uint32_t color = kColorSub;
    if (gps.has_fix) {
        const int satellites = gps.satellites >= 0 ? gps.satellites : 0;
        snprintf(text, sizeof(text), LV_SYMBOL_GPS " %d", satellites);
        color = kColorGood;
    } else {
        snprintf(text, sizeof(text), LV_SYMBOL_GPS);
        color = gps.uart_enabled ? kColorWeak : kColorSub;
    }
    if (setLabel(s_lbl_gps, s_shown_gps, sizeof(s_shown_gps), text)) {
        lv_obj_set_style_text_color(s_lbl_gps, lv_color_hex(color), 0);
    }
}

void refreshIp()
{
    char ip_text[96];
    uint32_t color;
    // The bar shows the bare address only -- no icon, no prefix label -- so a
    // long host/IP still fits. The text colour alone marks the state:
    // red = transmitting, cyan = receiving voice, blue = STA IP, amber = AP.
    char rx_call[8];
    unsigned rx_ssid = 0u;
    const bool rx = NRLAudioBridge_GetRemoteCaller(rx_call, sizeof(rx_call), &rx_ssid);
    const bool tx = STATUS_IO_IsSqlActive();
    const bool espnow_mode = ESPNOW_LINK_GetPttMode() == 1u;
    const bool espnow_tx = espnow_mode && tx && ESPNOW_LINK_IsEnabled();
    const bool espnow_rx = ESPNOW_LINK_IsReceiving();
    if (espnow_tx || espnow_rx) {
        // TX shows the intercom's own TX codec; RX adapts to what the peer is
        // actually sending (per-packet auto-detect) -- the two ends may be
        // configured differently.
        const uint8_t codec = espnow_tx ? ESPNOW_LINK_GetTxCodec()
                                        : ESPNOW_LINK_GetRxCodec();
        snprintf(ip_text, sizeof(ip_text), "ESP-NOW %s",
                 (codec == 1u) ? "OPUS" : "G711");
        color = kColorDuplex;
    } else if (tx || rx) {
        // Transmitting or receiving voice -> show the configured NRL server
        // host (the host string as configured, not a resolved IP address).
        const ExternalRadioConfig *cfg = EXTERNAL_RADIO_GetConfig();
        const char *host = (cfg != nullptr && cfg->server_host[0] != '\0')
                               ? cfg->server_host
                               : "---";
        snprintf(ip_text, sizeof(ip_text), "%s", host);
        color = tx ? kColorTx : kColorAccent;
    } else if (nrlWifiStaConnected()) {
        // No idle "PTT ESP-NOW" takeover here: the standby caption already
        // reads "STANDBY ESP-NOW" while the intercom is armed, so the address
        // bar keeps showing the IP; it switches to "ESP-NOW <codec>" only for
        // the duration of actual intercom TX/RX (branch above).
        char sta_buf[16] = {};
        nrlIpToString(nrlWifiStaIp(), sta_buf, sizeof(sta_buf));
        snprintf(ip_text, sizeof(ip_text), "%s", sta_buf);
        color = kColorIp;
    } else {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
            // WiFi join failed -> config AP is up; show the hotspot address.
            char ap_buf[16] = {};
            nrlIpToString(nrlWifiApIp(), ap_buf, sizeof(ap_buf));
            snprintf(ip_text, sizeof(ip_text), "%s", ap_buf);
            color = kColorApWarn;
        } else {
            snprintf(ip_text, sizeof(ip_text), "---");
            color = kColorSub;
        }
    }
    if (setLabel(s_lbl_ip, s_shown_ip, sizeof(s_shown_ip), ip_text)) {
        lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(color), 0);
    }
}

void refreshOtaNotice()
{
    if (s_lbl_ota == nullptr) {
        return;
    }
    const NrlOtaStatus *status = otaUiSnapshot();
    char text[sizeof(s_shown_ota)] = {};
    uint32_t color = kColorApWarn;
    int progress_percent = -1;
    DisplayNoticeSnapshot notice = {};
    DISPLAY_NOTICE_Get(&notice);
#if NRL_BOARD == NRL_BOARD_BI4UMD
    char signaling[sizeof(s_shown_signaling)] = {};
    SIGNALING_GetLastResult(signaling, sizeof(signaling));
    if (s_lbl_signaling != nullptr &&
        setLabel(s_lbl_signaling, s_shown_signaling,
                 sizeof(s_shown_signaling), signaling)) {
        lv_obj_set_style_text_color(s_lbl_signaling, lv_color_hex(kColorAccent), 0);
    }
#endif
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    bool notice_active = notice.text[0] != '\0' &&
        (notice.duration_ms == 0u || now - notice.posted_ms < notice.duration_ms);
#if NRL_BOARD == NRL_BOARD_BI4UMD
    // Signaling_service also posts each decode as a generic notice. It is
    // already visible on the dedicated row, so do not duplicate it below.
    if (notice_active && signaling[0] != '\0' &&
        strcmp(notice.text, signaling) == 0) {
        notice_active = false;
    }
#endif
    if (notice_active) {
        localizeDisplayNotice(text, sizeof(text), notice.text);
        progress_percent = notice.progress_percent;
        if (notice.level == DISPLAY_NOTICE_SUCCESS) color = kColorGood;
        else if (notice.level == DISPLAY_NOTICE_ERROR) color = kColorWeak;
        else if (notice.level == DISPLAY_NOTICE_WARNING) color = kColorApWarn;
        else color = kColorAccent;
    } else if (status != nullptr && status->updating) {
        if (status->update_size > 0u) {
            snprintf(text, sizeof(text),
                     menuText("OTA UPDATING %u%%", "OTA升级中 %u%%"),
                     static_cast<unsigned>(status->update_percent));
            progress_percent = status->update_percent;
        } else {
            snprintf(text, sizeof(text), "%s",
                     menuText("OTA UPDATING...", "OTA升级中..."));
        }
        color = kColorTx;
    } else if (status != nullptr && status->checking) {
        snprintf(text, sizeof(text), "%s",
                 menuText("OTA CHECKING...", "OTA检查中..."));
    } else if (status != nullptr && status->latest_version[0] != '\0' &&
               strcmp(status->latest_version, NRL_FIRMWARE_VERSION) != 0) {
        snprintf(text, sizeof(text),
                 menuText("NEW FW %.20s VOL+/-", "新固件 %.20s 音量+/-"),
                 status->latest_version);
        color = kColorGood;
    } else if (APRS_SERVICE_IsEnabled()) {
        // Every parsed APRS packet gets a compact summary. Unlike the station
        // list, this also covers text/status packets that have no position.
        char summary[sizeof(text)] = {};
        if (APRS_SERVICE_GetLastSummary(summary, sizeof(summary)) != 0u &&
            summary[0] != '\0') {
            snprintf(text, sizeof(text), "APRS %.*s",
                     static_cast<int>(sizeof(text) - 6u), summary);
            color = kColorAccent;
        }
    }
    if (setLabel(s_lbl_ota, s_shown_ota, sizeof(s_shown_ota), text)) {
        lv_obj_set_style_text_color(s_lbl_ota, lv_color_hex(color), 0);
    }
    if (s_bar_ota != nullptr) {
        if (progress_percent >= 0) {
            lv_bar_set_value(s_bar_ota, progress_percent, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_bar_ota, lv_color_hex(color), LV_PART_INDICATOR);
            lv_obj_remove_flag(s_bar_ota, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_bar_ota, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

size_t menuItemCount()
{
    if (s_menu_page == MenuPage::Main) return 9u;
    if (s_menu_page == MenuPage::Language) return 3u;
    if (s_menu_page == MenuPage::About) return 1u;
    if (s_menu_page == MenuPage::Aprs) return 5u;
    if (s_menu_page == MenuPage::AprsSettings) return 8u;
    if (s_menu_page == MenuPage::AprsList) return 1u;
    if (s_menu_page == MenuPage::AprsGps) return 1u;
    if (s_menu_page == MenuPage::Signaling) return 4u;
    if (s_menu_page == MenuPage::Ctcss) return 3u;
    if (s_menu_page == MenuPage::Mdc || s_menu_page == MenuPage::Dtmf) return 5u;
    const NrlOtaStatus *ota = otaUiSnapshot();
    return (ota != nullptr ? ota->release_count : 0u) + 1u; // Back row + releases
}

void activateMainMenu()
{
    s_menu_page = MenuPage::Main;
    s_menu_index = 0u;
    s_menu_ota_requested = false;
    s_menu_ota_state[0] = '\0';
    buildMenuUi();
}

void confirmMainMenu()
{
    switch (s_menu_index) {
        case 0:
            s_menu_active = false;
            s_menu_message[0] = '\0';
            buildHomeContent();
            break;
        case 1: {
            const bool select_espnow = ESPNOW_LINK_GetPttMode() == 0u;
            if (ESPNOW_LINK_SetEnabled(select_espnow)) {
                setMenuMessage(select_espnow ? "PTT -> ESP-NOW" : "PTT -> NRL");
            } else {
                setMenuMessage("ESP-NOW ENABLE FAILED");
            }
            buildMenuUi();
            break;
        }
        case 2: {
            const uint8_t codec = NRLAudioBridge_GetVoiceCodec() == 1u ? 0u : 1u;
            if (NRLAudioBridge_SetVoiceCodec(codec)) {
                setMenuMessage(codec == 1u ? "NRL CODEC -> OPUS" : "NRL CODEC -> G711");
            } else {
                setMenuMessage("NRL OPUS: NO MEMORY");
            }
            buildMenuUi();
            break;
        }
        case 3: {
            const uint8_t codec = ESPNOW_LINK_GetTxCodec() == 1u ? 0u : 1u;
            if (ESPNOW_LINK_SetTxCodec(codec)) {
                setMenuMessage(codec == 1u ? "NOW CODEC -> OPUS" : "NOW CODEC -> G711");
            } else {
                setMenuMessage("NOW OPUS: NO MEMORY");
            }
            buildMenuUi();
            break;
        }
        case 4:
            s_menu_page = MenuPage::Signaling;
            s_menu_index = 0u;
            buildMenuUi();
            break;
        case 5: {
            const NrlOtaStatus *ota = otaUiSnapshot();
            s_menu_page = MenuPage::Ota;
            s_menu_index = 0u;
            s_menu_ota_check_baseline_ms = ota != nullptr ? ota->last_check_ms : 0u;
            s_menu_ota_requested = OtaService_CheckNow();
            setMenuMessage(s_menu_ota_requested
                               ? menuText("CHECK REQUESTED", "已请求检查")
                               : menuText("OTA SERVER NOT SET", "未设置 OTA 服务器"));
            s_menu_ota_state[0] = '\0';
            buildMenuUi();
            break;
        }
        case 6:
            s_menu_page = MenuPage::Aprs;
            s_menu_index = 0u;
            s_menu_aprs_refresh_ms = 0u;
            s_menu_aprs_revision = APRS_SERVICE_GetStationRevision();
            buildMenuUi();
            break;
        case 7:
            s_menu_page = MenuPage::Language;
            s_menu_index = 0u;
            buildMenuUi();
            break;
        case 8:
            s_menu_page = MenuPage::About;
            s_menu_index = 0u;
            buildMenuUi();
            break;
        default:
            break;
    }
}

void confirmLanguageMenu()
{
    if (s_menu_index == 0u) {
        activateMainMenu();
        return;
    }
    s_menu_chinese = s_menu_index == 1u;
    saveMenuLanguage();
    s_menu_index = 0u;
    activateMainMenu();
}

void confirmSignalingMenu()
{
    if (s_menu_index == 0u) {
        activateMainMenu();
    } else {
        s_menu_page = s_menu_index == 1u ? MenuPage::Mdc
                    : s_menu_index == 2u ? MenuPage::Dtmf : MenuPage::Ctcss;
        s_menu_index = 0u;
        buildMenuUi();
    }
}

void confirmAprsMenu()
{
    if (s_menu_index == 0u) {
        activateMainMenu();
    } else if (s_menu_index == 1u) {
        s_menu_page = MenuPage::AprsSettings;
        s_menu_index = 0u;
        buildMenuUi();
    } else if (s_menu_index == 2u) {
#if NRL_BOARD == NRL_BOARD_BI4UMD
        s_bi4umd_aprs_from_settings = false;
#endif
        s_menu_page = MenuPage::AprsList;
        s_menu_index = 0u;
        s_menu_aprs_refresh_ms = 0u;
        s_menu_aprs_revision = APRS_SERVICE_GetStationRevision();
        buildMenuUi();
    } else if (s_menu_index == 3u) {
#if NRL_BOARD == NRL_BOARD_BI4UMD
        s_bi4umd_aprs_from_settings = false;
#endif
        s_menu_page = MenuPage::AprsGps;
        s_menu_index = 0u;
        s_menu_aprs_refresh_ms = 0u;
        buildMenuUi();
    } else {
        const bool ok = APRS_SERVICE_SendBeaconNow();
        setMenuMessage(ok ? "BEACON QUEUED" : "ENABLE APRS FIRST");
        buildMenuUi();
    }
}

void leaveAprsDetailMenu()
{
#if NRL_BOARD == NRL_BOARD_BI4UMD
    if (s_bi4umd_aprs_from_settings) {
        s_bi4umd_aprs_from_settings = false;
        s_menu_active = false;
        s_menu_message[0] = '\0';
        s_bi4umd_page = Bi4umdPage::Settings;
        buildBi4umdSettingsContent();
        return;
    }
#endif
    s_menu_page = MenuPage::Aprs;
    s_menu_index = 0u;
    buildMenuUi();
}

void confirmAprsSettingsMenu()
{
    if (s_menu_index == 0u) {
        s_menu_page = MenuPage::Aprs;
        s_menu_index = 0u;
        buildMenuUi();
        return;
    }

    AprsConfig cfg{};
    APRS_SERVICE_GetConfig(&cfg);
    bool ok = false;
    const char *message = "SAVE FAILED";
    switch (s_menu_index) {
        case 1:
            ok = APRS_SERVICE_SetEnabled(!cfg.enabled);
            message = !cfg.enabled ? "APRS ON" : "APRS OFF";
            break;
        case 2:
            ok = APRS_SERVICE_SetNetEnabled(!cfg.net_enabled);
            message = !cfg.net_enabled ? "APRS-IS ON" : "APRS-IS OFF";
            break;
        case 3:
            ok = APRS_SERVICE_SetRfTxEnabled(!cfg.rf_tx_enabled);
            message = !cfg.rf_tx_enabled ? "RF TX ON" : "RF TX OFF";
            break;
        case 4:
            ok = APRS_SERVICE_SetRfRxEnabled(!cfg.rf_rx_enabled);
            message = !cfg.rf_rx_enabled ? "RF RX ON" : "RF RX OFF";
            break;
        case 5:
            ok = APRS_SERVICE_SetAutoInterval(!cfg.auto_interval);
            message = !cfg.auto_interval ? "AUTO PERIOD ON" : "AUTO PERIOD OFF";
            break;
        case 6: {
            ok = APRS_SERVICE_SetFixedBeaconWithoutGps(!cfg.fixed_beacon_without_gps);
            message = !cfg.fixed_beacon_without_gps ? "FIXED POS ON" : "FIXED POS OFF";
            break;
        }
        case 7: {
            static constexpr uint16_t periods[] = {30u, 60u, 120u, 300u, 600u, 1200u, 3600u};
            uint16_t next = periods[0];
            for (const uint16_t period : periods) {
                if (period > cfg.beacon_interval_s) {
                    next = period;
                    break;
                }
            }
            ok = APRS_SERVICE_SetBeaconInterval(next);
            message = "PERIOD UPDATED";
            break;
        }
        case 8:
            ok = APRS_SERVICE_SetSsid(static_cast<uint8_t>((cfg.ssid + 1u) & 0x0Fu));
            message = "SSID UPDATED";
            break;
        default:
            break;
    }
    setMenuMessage(ok ? message : "SAVE FAILED");
    buildMenuUi();
}

void confirmCtcssMenu()
{
    if (s_menu_index == 0u) {
        s_menu_page = MenuPage::Signaling;
        s_menu_index = 0u;
        buildMenuUi();
        return;
    }
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);
    const SignalingRoute route = s_menu_index == 1u
                                     ? SIGNAL_ROUTE_RX_MIC : SIGNAL_ROUTE_RX_NRL;
    const bool enabled = s_menu_index == 1u ? !cfg.ctcss_rx_mic : !cfg.ctcss_rx_nrl;
    const bool ok = SIGNALING_SetCtcssRoute(route, enabled);
    setMenuMessage(ok ? (enabled ? "CTCSS RX ON" : "CTCSS RX OFF") : "SAVE FAILED");
    buildMenuUi();
}

void confirmProtocolMenu(bool mdc)
{
    if (s_menu_index == 0u) {
        s_menu_page = MenuPage::Signaling;
        s_menu_index = 0u;
        buildMenuUi();
        return;
    }
    const size_t index = s_menu_index - 1u;
    SignalingConfig cfg{};
    SIGNALING_GetConfig(&cfg);
    const bool enabled = !signalingRouteEnabled(cfg, mdc, index);
    const SignalingRoute route = static_cast<SignalingRoute>(index);
    const bool ok = mdc ? SIGNALING_SetMdcRoute(route, enabled)
                        : SIGNALING_SetDtmfRoute(route, enabled);
    setMenuMessage(ok ? (enabled ? "SIGNAL ROUTE ON" : "SIGNAL ROUTE OFF") : "SAVE FAILED");
    buildMenuUi();
}

void confirmOtaMenu()
{
    if (s_menu_index == 0u) {
        activateMainMenu();
        return;
    }
    const NrlOtaStatus *ota = otaUiSnapshot();
    if (ota == nullptr) {
        setMenuMessage(menuText("OTA STATUS UNAVAILABLE", "OTA状态不可用"));
    } else if (ota->checking || ota->updating || s_menu_ota_requested) {
        setMenuMessage(menuText("OTA BUSY", "OTA正忙"));
    } else if (s_menu_index > ota->release_count) {
        setMenuMessage(menuText("SELECT A VERSION", "请选择版本"));
    } else {
        const char *version = ota->releases[s_menu_index - 1u].version;
        if (strcmp(version, NRL_FIRMWARE_VERSION) == 0) {
            setMenuMessage(menuText("ALREADY INSTALLED", "此版本已安装"));
        } else if (OtaService_UpdateVersion(version)) {
            setMenuMessage(menuText("INSTALL REQUESTED", "已请求安装"), 10000u);
        } else {
            setMenuMessage(menuText("INSTALL REQUEST FAILED", "安装请求失败"));
        }
    }
    buildMenuUi();
}

void processMenuInput(uint32_t now)
{
    if (s_menu_open_requested) {
        s_menu_open_requested = false;
#if NRL_BOARD == NRL_BOARD_BI4UMD
        STATUS_IO_SetSoftPtt(false);
        s_bi4umd_page = Bi4umdPage::Radio;
        s_bi4umd_aprs_from_settings = false;
#endif
        s_menu_nav_pending = 0;
        s_menu_confirm_pending = 0u;
        s_menu_message[0] = '\0';
        activateMainMenu();
    }
    if (!s_menu_active) return;

    const int nav = s_menu_nav_pending;
    s_menu_nav_pending = 0;
    if (nav != 0) {
        const size_t count = menuItemCount();
        const int steps = nav > 0 ? nav : -nav;
        for (int i = 0; i < steps; ++i) {
            if (nav > 0) { // VOL+ = up
                s_menu_index = s_menu_index == 0u ? count - 1u : s_menu_index - 1u;
            } else {       // VOL- = down
                s_menu_index = (s_menu_index + 1u) % count;
            }
        }
        buildMenuUi();
    }

    if (s_menu_confirm_pending != 0u) {
        s_menu_confirm_pending = 0u;
        if (s_menu_page == MenuPage::Main) confirmMainMenu();
        else if (s_menu_page == MenuPage::Language) confirmLanguageMenu();
        else if (s_menu_page == MenuPage::About) activateMainMenu();
        else if (s_menu_page == MenuPage::Aprs) confirmAprsMenu();
        else if (s_menu_page == MenuPage::AprsSettings) confirmAprsSettingsMenu();
        else if (s_menu_page == MenuPage::AprsList) {
            leaveAprsDetailMenu();
        }
        else if (s_menu_page == MenuPage::AprsGps) {
            leaveAprsDetailMenu();
        }
        else if (s_menu_page == MenuPage::Signaling) confirmSignalingMenu();
        else if (s_menu_page == MenuPage::Ctcss) confirmCtcssMenu();
        else if (s_menu_page == MenuPage::Mdc) confirmProtocolMenu(true);
        else if (s_menu_page == MenuPage::Dtmf) confirmProtocolMenu(false);
        else confirmOtaMenu();
    }
    if (!s_menu_active) return;

    if (s_menu_message[0] != '\0' && static_cast<int32_t>(now - s_menu_message_until_ms) >= 0) {
        s_menu_message[0] = '\0';
        buildMenuUi();
    }

    if (s_menu_page == MenuPage::Ota &&
        (s_menu_ota_refresh_ms == 0u || now - s_menu_ota_refresh_ms >= 250u)) {
        s_menu_ota_refresh_ms = now;
        const NrlOtaStatus *ota = otaUiSnapshot();
        if (ota == nullptr) {
            setMenuMessage(menuText("OTA STATUS UNAVAILABLE", "OTA状态不可用"));
            buildMenuUi();
            return;
        }
        if (ota->checking || ota->last_check_ms != s_menu_ota_check_baseline_ms) {
            s_menu_ota_requested = false;
        }
        char state[sizeof(s_menu_ota_state)] = {};
        int used = snprintf(state, sizeof(state), "%u|%u|%u|%u|%lu|%u|%s|%s",
                            ota->checking ? 1u : 0u, ota->updating ? 1u : 0u,
                            static_cast<unsigned>(ota->release_count),
                              s_menu_ota_requested ? 1u : 0u,
                              static_cast<unsigned long>(ota->update_size),
                              static_cast<unsigned>(ota->update_percent),
                              ota->latest_version, ota->last_error);
        for (size_t i = 0; i < ota->release_count && used > 0 &&
                           static_cast<size_t>(used) < sizeof(state) - 2u; ++i) {
            used += snprintf(state + used, sizeof(state) - static_cast<size_t>(used),
                             "|%s", ota->releases[i].version);
        }
        if (strncmp(state, s_menu_ota_state, sizeof(s_menu_ota_state)) != 0) {
            snprintf(s_menu_ota_state, sizeof(s_menu_ota_state), "%s", state);
            if (s_menu_index > ota->release_count) s_menu_index = 0u;
            buildMenuUi();
        }
    }

    // Keep the APRS station page live: redraw when a packet lands and every
    // few seconds anyway so the age column ticks.
    if (s_menu_page == MenuPage::AprsList) {
        const uint32_t revision = APRS_SERVICE_GetStationRevision();
        if (revision != s_menu_aprs_revision ||
            s_menu_aprs_refresh_ms == 0u || now - s_menu_aprs_refresh_ms >= 3000u) {
            s_menu_aprs_revision = revision;
            s_menu_aprs_refresh_ms = now;
            buildMenuUi();
        }
    }
    if (s_menu_page == MenuPage::AprsGps &&
        (s_menu_aprs_refresh_ms == 0u || now - s_menu_aprs_refresh_ms >= 1000u)) {
        s_menu_aprs_refresh_ms = now;
        buildMenuUi();
    }
}

} // namespace

//================================ Public API =================================

extern "C" void Display_MenuOpen(void)
{
    s_menu_active = true;
    s_menu_open_requested = true;
}

extern "C" bool Display_MenuIsActive(void)
{
    return s_menu_active;
}

extern "C" void Display_MenuNavigate(const int direction)
{
    if (!s_menu_active || direction == 0) return;
    int pending = s_menu_nav_pending + (direction > 0 ? 1 : -1);
    if (pending > 8) pending = 8;
    if (pending < -8) pending = -8;
    s_menu_nav_pending = pending;
}

extern "C" void Display_MenuConfirm(void)
{
    const unsigned pending = s_menu_confirm_pending;
    if (s_menu_active && pending < 4u) {
        s_menu_confirm_pending = pending + 1u;
    }
}

extern "C" void Display_Init(void)
{
    if (s_ready) {
        return;
    }
    if (!initPanel()) {
        return;
    }
    if (!initLvgl()) {
        return;
    }
    s_font_aprs_16 = lv_font_montserrat_16;
    s_font_aprs_16.fallback = &lv_font_cjk_16;
    loadMenuLanguage();
#if NRL_BOARD == NRL_BOARD_BI4UMD
    s_font_music_20 = lv_font_montserrat_20;
    s_font_music_20.fallback = &lv_font_cjk_16;
#endif
#if NRL_DISPLAY_BUS_RGB
    initTouch();
#elif NRL_BOARD == NRL_BOARD_BI4UMD
    initBi4umdTouch();
#endif

    initBatteryAdc();
    s_battery_mv = readBatteryMv();

    if (s_provisioning_mode) {
        buildProvisioningUi();
        refreshProvisioningUi();
    } else {
        buildUi();
    }
    lv_refr_now(nullptr);  // paint the first frame before the backlight is lit

#if NRL_BOARD == NRL_BOARD_BI4UMD
    BI4UMD_Display_SetBacklight(true);
#elif NRL_PIN_DISPLAY_BL >= 0
    gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_DISPLAY_BL), 1);
#endif
    s_ready = true;
    ESP_LOGI(TAG,"[LCD] display ready");
}

extern "C" void Display_Poll(void)
{
    if (!s_ready) {
        return;
    }

    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (s_provisioning_mode) {
        if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
            s_last_refresh_ms = now;
            refreshProvisioningUi();
        }
        lv_timer_handler();
        return;
    }
    processMenuInput(now);
    if (s_last_battery_ms == 0u || (now - s_last_battery_ms) >= kBatteryIntervalMs) {
        s_last_battery_ms = now;
        s_battery_mv = readBatteryMv();
    }
    if (s_menu_active) {
        // The menu replaces only the centre content; keep both persistent bars
        // live so network, volume, battery and IP state remain current.
        refreshIp();
        refreshVolume();
        if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
            s_last_refresh_ms = now;
            refreshWifi();
            refreshBattery();
            refreshCpu();
            refreshGpsStatus();
        }
        lv_timer_handler();
        return;
    }

#if NRL_BOARD == NRL_BOARD_BI4UMD
    if (s_bi4umd_page != Bi4umdPage::Radio) {
        if (s_bi4umd_page == Bi4umdPage::Music) {
            refreshBi4umdMusic();
        }
        refreshIp();
        refreshVolume();
        if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
            s_last_refresh_ms = now;
            refreshWifi();
            refreshBattery();
            refreshCpu();
            refreshGpsStatus();
        }
        lv_timer_handler();
        return;
    }
#endif

    // The caller caption, IP bar and volume readout react to PTT / button
    // presses, so refresh them every poll for snappy feedback. setLabel()
    // still only redraws when the text actually changed.
    refreshCaller();
    refreshIp();
    refreshVolume();
    refreshOtaNotice();

    if (s_last_refresh_ms == 0u || (now - s_last_refresh_ms) >= kRefreshIntervalMs) {
        s_last_refresh_ms = now;
        refreshClock();
        refreshWifi();
        refreshBattery();
        refreshCpu();
        refreshGpsStatus();
    }

    lv_timer_handler();
}

extern "C" int Display_GetBatteryRawMv(void)
{
    return readBatteryRawMv();
}

extern "C" void Display_SetProvisioningMode(bool enabled)
{
    if (s_provisioning_mode == enabled) {
        return;
    }
    s_provisioning_mode = enabled;
    s_last_refresh_ms = 0u;
    if (!s_ready) {
        return;
    }
    if (enabled) {
        buildProvisioningUi();
        refreshProvisioningUi();
    } else {
        buildUi();
    }
    lv_refr_now(nullptr);
}

extern "C" int Display_GetBatteryCalibratedMv(void)
{
    return applyBatteryCalibration(readBatteryRawMv());
}

// CJK font engine switching exists only on the S31 800x480 panel.
extern "C" bool Display_SetCjkFontEngine(int) { return false; }
extern "C" int Display_GetCjkFontEngine(void) { return DISPLAY_CJK_FONT_BITMAP; }

// Framebuffer benchmark exists only on the S31 RGB panel.
extern "C" long Display_FramebufferBenchMBps(void) { return -1; }

#elif NRL_BOARD != NRL_BOARD_S31_KORVO

extern "C" void Display_Init(void) {}
extern "C" void Display_Poll(void) {}
extern "C" void Display_SetProvisioningMode(bool) {}
extern "C" void Display_MenuOpen(void) {}
extern "C" bool Display_MenuIsActive(void) { return false; }
extern "C" void Display_MenuNavigate(int) {}
extern "C" void Display_MenuConfirm(void) {}
extern "C" int Display_GetBatteryRawMv(void) { return 0; }
extern "C" int Display_GetBatteryCalibratedMv(void) { return 0; }
extern "C" bool Display_SetCjkFontEngine(int) { return false; }
extern "C" int Display_GetCjkFontEngine(void) { return DISPLAY_CJK_FONT_BITMAP; }
extern "C" long Display_FramebufferBenchMBps(void) { return -1; }

#endif
